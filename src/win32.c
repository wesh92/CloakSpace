#include "win32.h"
#include <dwmapi.h>
#include <stdio.h>
#include <wchar.h>

#define SP_WAIT_MS 400
/* One window-owning adapter per process. The observer only acknowledges HIDE events;
   desktop commands and ownership changes remain on the UI thread. */
static SpWin32 *observed;
static void CALLBACK hidden_event(HWINEVENTHOOK hook,DWORD event,HWND h,LONG object,LONG child,DWORD thread,DWORD when) {
    (void)hook;(void)thread;
    SpWin32 *a=observed;
    if (!a || event!=EVENT_OBJECT_HIDE || object!=OBJID_WINDOW || child!=CHILDID_SELF) return;
    uintptr_t pending=(uintptr_t)GetPropW(h,a->pending_tag);
    uintptr_t tick=(uintptr_t)GetPropW(h,a->pending_tick_tag);
    if (pending && tick && pending==(uintptr_t)GetPropW(h,a->tag) &&
        (LONG)(when-(DWORD)(tick-1))>=0) {
        RemovePropW(h,a->pending_tag); RemovePropW(h,a->pending_tick_tag);
    }
}
static DWORD WINAPI observe_windows(void *ctx) {
    SpWin32 *a=ctx; MSG message;
    PeekMessageW(&message,NULL,0,0,PM_NOREMOVE);
    HWINEVENTHOOK hook=NULL;a->observer_error=ERROR_CANCELLED;
    if (WaitForSingleObject(a->observer_stop,0)!=WAIT_OBJECT_0) {
        hook=SetWinEventHook(EVENT_OBJECT_HIDE,EVENT_OBJECT_HIDE,NULL,hidden_event,0,0,WINEVENT_OUTOFCONTEXT);
        a->observer_error=hook?0:GetLastError();
        if(!hook && !a->observer_error)a->observer_error=ERROR_GEN_FAILURE;
    }
    SetEvent(a->observer_ready);
    if (hook) {
        for (;;) {
            DWORD wait=MsgWaitForMultipleObjects(1,&a->observer_stop,FALSE,INFINITE,QS_ALLINPUT);
            if (wait!=WAIT_OBJECT_0+1) break;
            while (PeekMessageW(&message,NULL,0,0,PM_REMOVE)) {
                TranslateMessage(&message);DispatchMessageW(&message);
            }
        }
        UnhookWinEvent(hook);
    }
    return 0;
}
void sp_win32_dispose(SpWin32 *a) {
    if (a->observer_stop) SetEvent(a->observer_stop);
    if (a->observer_thread) { WaitForSingleObject(a->observer_thread,INFINITE);CloseHandle(a->observer_thread);a->observer_thread=NULL; }
    if (a->observer_ready) {CloseHandle(a->observer_ready);a->observer_ready=NULL;}
    if (a->observer_stop) {CloseHandle(a->observer_stop);a->observer_stop=NULL;}
    if (observed==a) observed=NULL;
}
static bool ensure_observer(SpWin32 *a) {
    if (a->observer_thread) return true;
    if (observed && observed!=a) {wcscpy(a->error,L"Only one window-owning adapter per process is supported.");return false;}
    a->observer_ready=CreateEventW(NULL,TRUE,FALSE,NULL);a->observer_stop=CreateEventW(NULL,TRUE,FALSE,NULL);
    if (!a->observer_ready||!a->observer_stop) {sp_win32_dispose(a);return false;}
    observed=a;
    a->observer_thread=CreateThread(NULL,0,observe_windows,a,0,NULL);
    if (!a->observer_thread || WaitForSingleObject(a->observer_ready,3000)!=WAIT_OBJECT_0 || a->observer_error) {
        sp_win32_dispose(a);wcscpy(a->error,L"Cannot start the window-completion observer; no window was hidden.");return false;
    }
    return true;
}
static bool fail(SpWin32 *a, const wchar_t *what) {
    DWORD e=GetLastError();
    swprintf(a->error,256,L"%ls (Windows error %lu).",what,(unsigned long)e);
    return false;
}
static bool integrity(HANDLE process, DWORD *level) {
    HANDLE token=NULL; DWORD size=0; bool ok=false;
    if (!OpenProcessToken(process,TOKEN_QUERY,&token)) return false;
    GetTokenInformation(token,TokenIntegrityLevel,NULL,0,&size);
    TOKEN_MANDATORY_LABEL *label=LocalAlloc(LMEM_FIXED,size);
    if (label && GetTokenInformation(token,TokenIntegrityLevel,label,size,&size) &&
        IsValidSid(label->Label.Sid)) {
        BYTE n=*GetSidSubAuthorityCount(label->Label.Sid);
        if (n) { *level=*GetSidSubAuthority(label->Label.Sid,n-1); ok=true; }
    }
    if (label) LocalFree(label);
    CloseHandle(token); return ok;
}
void sp_win32_dpi(void) {
    /* An embedded manifest normally sets this. Already configured is harmless. */
    (void)SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
}
bool sp_win32_init(SpWin32 *a, bool allow_own) {
    ZeroMemory(a,sizeof(*a)); a->own_pid=GetCurrentProcessId(); a->allow_own=allow_own;
    swprintf(a->tag,96,L"CloakSpace.Member.%lu.%llu",(unsigned long)a->own_pid,
             (unsigned long long)GetTickCount64());
    swprintf(a->pending_tag,96,L"%ls.PendingHide",a->tag);
    swprintf(a->pending_tick_tag,96,L"%ls.PendingTick",a->tag);
    if (!integrity(GetCurrentProcess(),&a->integrity)) return fail(a,L"Cannot read own integrity level");
    return true;
}
static bool alive(void *ctx, SpWindow w) {
    SpWin32 *a=ctx; HWND h=(HWND)w.handle; DWORD pid=0;
    DWORD tid=GetWindowThreadProcessId(h,&pid);
    return IsWindow(h) && tid==w.thread_id && pid==w.process_id &&
           (uintptr_t)GetPropW(h,a->tag)==w.cookie;
}
static bool claim(void *ctx, uintptr_t handle, SpWindow *out) {
    SpWin32 *a=ctx; HWND h=(HWND)handle; DWORD pid=0;
    DWORD tid=GetWindowThreadProcessId(h,&pid);
    wchar_t cls[80]={0}; GetClassNameW(h,cls,80);
    if (!tid || !pid || !IsWindowVisible(h) || GetAncestor(h,GA_ROOT)!=h ||
        h==GetDesktopWindow() || h==GetShellWindow() ||
        (!a->allow_own && pid==a->own_pid) ||
        !wcscmp(cls,L"Progman") || !wcscmp(cls,L"WorkerW") ||
        !wcscmp(cls,L"Shell_TrayWnd") || !wcscmp(cls,L"Shell_SecondaryTrayWnd")) {
        wcscpy(a->error,L"Only a visible, ordinary top-level application window can be taken.");
        return false;
    }
    DWORD cloaked=0;
    if (SUCCEEDED(DwmGetWindowAttribute(h,DWMWA_CLOAKED,&cloaked,sizeof(cloaked))) && cloaked) {
        wcscpy(a->error,L"Cloaked or other-virtual-desktop windows are not supported."); return false;
    }
    HANDLE p=OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,FALSE,pid);
    DWORD level=0; bool ok=p && integrity(p,&level);
    if (p) CloseHandle(p);
    if (!ok || level>a->integrity) {
        wcscpy(a->error,L"Access refused: target is protected or has higher integrity. No UIAccess/elevation is requested.");
        return false;
    }
    if (!ensure_observer(a)) return false;
    if (++a->next_cookie==0) ++a->next_cookie;
    if (!SetPropW(h,a->tag,(HANDLE)a->next_cookie)) return fail(a,L"Cannot mark window ownership");
    *out=(SpWindow){handle,a->next_cookie,pid,tid};
    if (!alive(a,*out)) return fail(a,L"Window closed while being taken");
    return true;
}
static bool topmost(HWND h) { return (GetWindowLongPtrW(h,GWL_EXSTYLE)&WS_EX_TOPMOST)!=0; }
static bool wait_state(SpWin32 *a, SpWindow w, bool shown, bool top, bool normal, const SpRect *expected) {
    ULONGLONG start=GetTickCount64();
    do {
        if (!alive(a,w)) { wcscpy(a->error,L"Window closed or its identity changed."); return false; }
        HWND h=(HWND)w.handle;
        RECT rect;
        bool placed=!expected || (GetWindowRect(h,&rect) && rect.left==expected->x && rect.top==expected->y &&
            rect.right-rect.left==expected->w && rect.bottom-rect.top==expected->h);
        if (!!IsWindowVisible(h)==shown && topmost(h)==top &&
            (!normal || (!IsIconic(h) && !IsZoomed(h))) && placed) return true;
        Sleep(5);
    } while (GetTickCount64()-start<SP_WAIT_MS);
    wcscpy(a->error,L"Window did not acknowledge the operation within 400 ms; ownership is retained for retry.");
    return false;
}
static bool hide(void *ctx, SpWindow w) {
    SpWin32 *a=ctx; HWND h=(HWND)w.handle;
    if (!alive(a,w)) return false;
    if ((uintptr_t)GetPropW(h,a->pending_tag)==w.cookie && IsWindowVisible(h)) {
        wcscpy(a->error,L"A previous hide is still pending; wait for the target to respond, then restore again.");
        return false;
    }
    if (!SetPropW(h,a->pending_tick_tag,(HANDLE)((uintptr_t)GetTickCount()+1)) ||
        !SetPropW(h,a->pending_tag,(HANDLE)w.cookie)) return fail(a,L"Cannot track pending hide");
    if (!SetWindowPos(h,HWND_NOTOPMOST,0,0,0,0,
        SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE|SWP_ASYNCWINDOWPOS|SWP_HIDEWINDOW)) {
        RemovePropW(h,a->pending_tag);RemovePropW(h,a->pending_tick_tag);return fail(a,L"Cannot hide window");
    }
    if (!wait_state(a,w,false,false,false,NULL)) return false;
    RemovePropW(h,a->pending_tag);RemovePropW(h,a->pending_tick_tag);return true;
}
static bool tile(void *ctx, SpWindow w, SpRect r) {
    SpWin32 *a=ctx; HWND h=(HWND)w.handle;
    if (!alive(a,w)) return false;
    if ((uintptr_t)GetPropW(h,a->pending_tag)==w.cookie && IsWindowVisible(h)) {
        wcscpy(a->error,L"A previous hide has not completed; restore/retry when the application responds.");
        return false;
    }
    RemovePropW(h,a->pending_tag);RemovePropW(h,a->pending_tick_tag);
    if (IsIconic(h) || IsZoomed(h)) {
        if (!ShowWindowAsync(h,SW_RESTORE)) return fail(a,L"Cannot restore window before tiling");
        /* Do not send placement while a preceding restore is still queued. */
        ULONGLONG start=GetTickCount64();
        while ((IsIconic(h)||IsZoomed(h)) && GetTickCount64()-start<SP_WAIT_MS) Sleep(5);
        if (IsIconic(h)||IsZoomed(h)) { wcscpy(a->error,L"Window restore timed out; retry or release it."); return false; }
    }
    if (!alive(a,w)) return false;
    if (!SetWindowPos(h,HWND_TOPMOST,r.x,r.y,r.w,r.h,
        SWP_NOACTIVATE|SWP_ASYNCWINDOWPOS|SWP_SHOWWINDOW)) return fail(a,L"Cannot tile window");
    return wait_state(a,w,true,true,true,&r);
}
static bool release(void *ctx, SpWindow w) {
    SpWin32 *a=ctx; HWND h=(HWND)w.handle;
    if (!alive(a,w)) return false;
    /* Do not forget a queued hide just because the old state is still visible. */
    if ((uintptr_t)GetPropW(h,a->pending_tag)==w.cookie && IsWindowVisible(h)) {
        wcscpy(a->error,L"Hide is still pending. Let the target respond (or close it), then retry Restore all.");
        return false;
    }
    if (!SetWindowPos(h,HWND_NOTOPMOST,0,0,0,0,
        SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE|SWP_ASYNCWINDOWPOS|SWP_SHOWWINDOW))
        return fail(a,L"Cannot release window");
    if (!wait_state(a,w,true,false,false,NULL)) return false;
    RemovePropW(h,a->pending_tag);RemovePropW(h,a->pending_tick_tag);RemovePropW(h,a->tag);return true;
}
static bool focus(void *ctx, SpWindow w) {
    SpWin32 *a=ctx;
    if (!alive(a,w)) return false;
    if (GetForegroundWindow()==(HWND)w.handle || SetForegroundWindow((HWND)w.handle)) return true;
    wcscpy(a->error,L"Windows denied foreground focus. Click the desired tile; placement/membership are unchanged.");
    return false;
}
SpWindowOps sp_win32_ops(void) { return (SpWindowOps){claim,alive,hide,tile,release,focus}; }
static bool monitor_workarea(SpWin32 *a, SpRect *out, bool *vertical) {
    POINT p={0}; GetCursorPos(&p);
    HMONITOR m=MonitorFromPoint(p,MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO info={0}; info.cbSize=sizeof(info);
    if (!GetMonitorInfoW(m,&info)) return fail(a,L"Cannot read monitor work area");
    RECT r=info.rcWork;
    *out=(SpRect){r.left,r.top,r.right-r.left,r.bottom-r.top};
    r=info.rcMonitor;
    if (vertical) *vertical=(int64_t)r.bottom-r.top>(int64_t)r.right-r.left;
    return true;
}
bool sp_win32_workarea(SpWin32 *a, SpRect *out) { return monitor_workarea(a,out,NULL); }
bool sp_win32_scratch_workarea(SpWin32 *a, SpState *s, SpRect *out) {
    return monitor_workarea(a,out,&s->vertical_screen);
}
