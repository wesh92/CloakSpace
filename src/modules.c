#include "modules.h"
#include "launch_args.h"
#include <stdio.h>
#include <wchar.h>

static bool scratch_action(SpModuleContext *c,const SpBinding *b) {
    SpResult r={SP_NOOP,0};
    if (!wcscmp(b->action,L"scratch.toggle")) r=sp_toggle(c->scratch,c->work);
    else if (!wcscmp(b->action,L"scratch.member")) r=sp_toggle_member(c->scratch,(uintptr_t)c->target,c->work);
    else if (!wcscmp(b->action,L"scratch.promote")) r=sp_promote(c->scratch,(uintptr_t)c->target,c->work);
    else if (!wcscmp(b->action,L"scratch.restore")) r=sp_release_all(c->scratch);
    c->report(c->host,r); return r.status==SP_OK||r.status==SP_NOOP;
}
static bool background_action(SpModuleContext *c,const SpBinding *b) { return c->background(c->host,b); }
static bool toggle_topmost(SpModuleContext *c,SpWindowOps ops,SpWindow w) {
    bool desired=(GetWindowLongPtrW(c->target,GWL_EXSTYLE)&WS_EX_TOPMOST)==0;
    if (!SetWindowPos(c->target,desired?HWND_TOPMOST:HWND_NOTOPMOST,0,0,0,0,
        SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE|SWP_ASYNCWINDOWPOS)) return false;
    /* The UI thread serializes calls, but an accepted async request is not a
       completed toggle. Do not let the next successful call reuse old state. */
    ULONGLONG start=GetTickCount64();
    do {
        if (!ops.alive(c->windows,w)) { SetLastError(ERROR_INVALID_WINDOW_HANDLE); return false; }
        if (((GetWindowLongPtrW(c->target,GWL_EXSTYLE)&WS_EX_TOPMOST)!=0)==desired) return true;
        Sleep(5);
    } while (GetTickCount64()-start<400);
    SetLastError(ERROR_TIMEOUT); return false;
}
static bool window_action(SpModuleContext *c,const SpBinding *b) {
    /* These non-owning utilities never hide windows. Scratch members must be released first,
       so a transient validation claim cannot replace the scratchpad's identity cookie. */
    for (size_t i=0;i<c->scratch->count;++i) if(c->scratch->windows[i].handle==(uintptr_t)c->target) {
        c->notify(c->host,L"Release this scratchpad member before applying a standalone window action."); return false;
    }
    SpWindowOps ops=sp_win32_ops(); SpWindow w;
    if (!ops.claim(c->windows,(uintptr_t)c->target,&w)) { c->notify(c->host,c->windows->error); return false; }
    bool ok=false;
    if (!wcscmp(b->action,L"window.center")) {
        SpRect r;
        if (sp_layout_with(c->work,1,&r,1,c->profile->layout)==SP_OK)
            ok=SetWindowPos(c->target,NULL,r.x,r.y,r.w,r.h,SWP_ASYNCWINDOWPOS|SWP_NOZORDER|SWP_NOACTIVATE)!=0;
    } else if (!wcscmp(b->action,L"window.minimize")) ok=ShowWindowAsync(c->target,SW_MINIMIZE)!=0;
    else if (!wcscmp(b->action,L"window.toggle-topmost")) ok=toggle_topmost(c,ops,w);
    DWORD error=GetLastError();
    if (ops.alive(c->windows,w)) RemovePropW(c->target,c->windows->tag);
    if (!ok) {
        wchar_t message[256];
        if (error==ERROR_TIMEOUT && !wcscmp(b->action,L"window.toggle-topmost"))
            swprintf(message,256,L"Topmost toggle was not acknowledged within 400 ms; the queued request may still apply. Wait for the target to respond before toggling again.");
        else swprintf(message,256,L"Window module failed (Windows error %lu).",(unsigned long)error);
        c->notify(c->host,message);
    }
    return ok;
}
/* Extension point: register additional C callbacks here. Configuration and the hook
   are independent of module implementations. No dynamically loaded code is trusted. */
static const SpAction actions[]={
    {L"scratch.toggle",0,0,scratch_action,L"Show/hide and tile the ordered scratchpad"},
    {L"scratch.member",0,0,scratch_action,L"Take/release the foreground window"},
    {L"scratch.promote",0,0,scratch_action,L"Promote focused scratch member to the first tile"},
    {L"scratch.restore",0,0,scratch_action,L"Show/release all members; retain failures for retry"},
    {L"window.center",0,0,window_action,L"Center/resize a nonmember using single-window fractions"},
    {L"window.minimize",0,0,window_action,L"Minimize a nonmember (never hide it)"},
    {L"window.toggle-topmost",0,0,window_action,L"Toggle a nonmember's topmost status"},
    {L"terminal.open",0,0,background_action,L"Open Windows Terminal's default or configured profile in the quake window"},
    {L"terminal.container",0,SP_MAX_ARGS,background_action,L"Discover a running Podman container and open /bin/sh or a configured command"},
    {L"terminal.quake",1,SP_MAX_ARGS,background_action,L"Open a named Terminal profile with optional command arguments"},
    {L"process.exec",1,SP_MAX_ARGS,background_action,L"Run an executable plus arguments directly (trusted config)"}
};
const SpAction *sp_actions(size_t *count) { *count=sizeof(actions)/sizeof(actions[0]); return actions; }
static const SpAction *find_action(const wchar_t *name) {
    for (size_t i=0;i<sizeof(actions)/sizeof(actions[0]);++i) if (!wcscmp(actions[i].name,name)) return &actions[i];
    return NULL;
}
bool sp_modules_validate(const SpProfile *p,wchar_t *error,size_t cap) {
    const wchar_t *problem=sp_terminal_settings_error(p);
    if (problem) { swprintf(error,cap,L"Profile '%ls': %ls",p->name,problem); return false; }
    for (size_t i=0;i<p->binding_count;++i) {
        const SpBinding *b=&p->bindings[i]; const SpAction *a=find_action(b->action);
        if (!a || b->argc<a->min_args || b->argc>a->max_args) {
            swprintf(error,cap,L"Profile '%ls', binding %zu: unknown action '%ls' or invalid argument count.",p->name,i+1,b->action);
            return false;
        }
        problem=sp_launch_binding_error(b);
        if (problem) {
            swprintf(error,cap,L"Profile '%ls', binding %zu (%ls): %ls",p->name,i+1,b->action,problem);
            return false;
        }
    }
    return true;
}
bool sp_modules_run(SpModuleContext *c,const SpBinding *b) {
    const SpAction *a=find_action(b->action);
    if (!a || b->argc<a->min_args || b->argc>a->max_args) {
        c->notify(c->host,L"Invalid action dispatch; check the module registry and configuration."); return false;
    }
    const wchar_t *problem=sp_launch_binding_error(b);
    if (problem) { c->notify(c->host,problem); return false; }
    return a->run(c,b);
}
