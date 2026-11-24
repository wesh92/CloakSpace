#include "win32.h"
#include <dwmapi.h>
#include <stdio.h>
#include <wchar.h>

#define SP_WAIT_MS 400
#define SP_STYLE_PROBE_MS 20
#define SP_LAYER_INCOMPATIBLE (WS_EX_NOREDIRECTIONBITMAP|WS_EX_TRANSPARENT)
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
    a->get_border_attribute=DwmGetWindowAttribute;
    a->set_border_attribute=DwmSetWindowAttribute;
    swprintf(a->tag,96,L"CloakSpace.Member.%lu.%llu",(unsigned long)a->own_pid,
             (unsigned long long)GetTickCount64());
    swprintf(a->pending_tag,96,L"%ls.PendingHide",a->tag);
    swprintf(a->pending_tick_tag,96,L"%ls.PendingTick",a->tag);
    if (!integrity(GetCurrentProcess(),&a->integrity)) return fail(a,L"Cannot read own integrity level");
    return true;
}
static bool alive(void *ctx, SpWindow w) {
    SpWin32 *a=ctx; HWND h=(HWND)w.handle; DWORD pid=0;
    if (!h || !w.cookie || !w.process_id || !w.thread_id) return false;
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
bool sp_win32_member_visible(SpWin32 *a, SpWindow w) {
    HWND h=(HWND)w.handle; DWORD cloaked=0;
    return alive(a,w) && IsWindowVisible(h) && !IsIconic(h) &&
        SUCCEEDED(DwmGetWindowAttribute(h,DWMWA_CLOAKED,&cloaked,sizeof(cloaked))) && !cloaked;
}
HWND sp_win32_foreground_member(SpWin32 *a, const SpState *s, HWND foreground) {
    if (!foreground || !s->visible || s->ctx!=a || !s->count) return NULL;
    /* Claims are top-level windows. Check each owner, not just GA_ROOTOWNER:
       an owned top-level window might itself be a member. Bound traversal
       like the shield's family check, since foreign handles can change. */
    HWND h=GetAncestor(foreground,GA_ROOT);
    for (unsigned depth=0;h && depth<SP_CAPACITY;++depth,h=GetWindow(h,GW_OWNER)) {
        for (size_t i=0;i<s->count;++i) {
            SpWindow w=s->windows[i];
            if ((HWND)w.handle==h && sp_win32_member_visible(a,w)) return h;
        }
    }
    return NULL;
}
static bool read_exstyle(HWND h, LONG_PTR *style) {
    SetLastError(ERROR_SUCCESS);
    *style=GetWindowLongPtrW(h,GWL_EXSTYLE);
    return *style || GetLastError()==ERROR_SUCCESS;
}
static bool read_layered(HWND h, SpWin32LayeredAttrs *attrs) {
    SetLastError(ERROR_SUCCESS);
    return GetLayeredWindowAttributes(h,&attrs->key,&attrs->alpha,&attrs->flags)!=0;
}
static SpWin32Appearance *find_appearance(SpWin32 *a, SpWindow w) {
    for (size_t i=0;i<SP_CAPACITY;++i) {
        SpWindow m=a->appearance[i].member;
        if (m.handle==w.handle && m.cookie==w.cookie &&
            m.process_id==w.process_id && m.thread_id==w.thread_id) return &a->appearance[i];
    }
    return NULL;
}
static SpWin32Appearance *reserve_appearance(SpWin32 *a, SpWindow w) {
    for (size_t pass=0;pass<2;++pass) for (size_t i=0;i<SP_CAPACITY;++i) {
        SpWin32Appearance *m=&a->appearance[i];
        if (!m->busy && (!m->member.cookie || (pass && !alive(a,m->member)))) {
            *m=(SpWin32Appearance){.member=w};
            return m;
        }
    }
    return NULL;
}
static void finish_appearance(SpWin32Appearance *m, bool released) {
    if (released || (!m->opacity.remembered && m->border.state==SP_BORDER_UNSET)) *m=(SpWin32Appearance){0};
    else m->busy=false;
}
static SpWin32Appearance *remember_opacity(SpWin32 *a, SpWindow w, SpWin32Appearance *m, BYTE alpha) {
    HWND h=(HWND)w.handle; LONG_PTR style;
    SpWin32LayeredAttrs original={0};
    if (!read_exstyle(h,&style)) return NULL;
    if (style&WS_EX_LAYERED) {
        /* No attributes to round-trip on a per-pixel or uninitialized layered window. */
        if (!read_layered(h,&original) || !original.flags || (original.flags&~(LWA_ALPHA|LWA_COLORKEY)))
            return NULL;
    } else if ((style&SP_LAYER_INCOMPATIBLE) ||
        (GetClassLongPtrW(h,GCL_STYLE)&(CS_OWNDC|CS_CLASSDC|CS_PARENTDC))) return NULL;
    if (alpha==((original.flags&LWA_ALPHA)?original.alpha:255)) return NULL;
    /* Keep the first snapshot for the entire cookie, even after going native.
       If the app later changes its layering, yield rather than resnapshot it. */
    if (!m) m=reserve_appearance(a,w);
    if (m) m->opacity=(SpWin32Opacity){.original=original,.remembered=true};
    return m;
}
static SpWin32LayeredAttrs desired_attrs(const SpWin32Opacity *o, BYTE alpha) {
    SpWin32LayeredAttrs attrs=o->original;
    if (alpha) {attrs.flags|=LWA_ALPHA;attrs.alpha=alpha;}
    return attrs;
}
static bool observe_opacity(SpWin32Appearance *m, LONG_PTR *style) {
    SpWin32Opacity *o=&m->opacity; HWND h=(HWND)m->member.handle;
    if (!read_exstyle(h,style)) return false;
    bool layered=(*style&WS_EX_LAYERED)!=0;
    if (layered==(o->original.flags!=0 || o->added_layered)) {
        if (!layered) return true;
        SpWin32LayeredAttrs now={0}, expected=desired_attrs(o,o->applied_alpha);
        if (!read_layered(h,&now)) {
            DWORD error=GetLastError();
            if (error!=ERROR_SUCCESS && error!=ERROR_INVALID_PARAMETER) return false;
            /* Still layered but no constant attributes: the app can have reset
               the bit and switched to UpdateLayeredWindow. Leave it alone. */
        } else if (o->added_layered && !o->applied_alpha &&
            (!now.flags || (now.flags==LWA_ALPHA && now.alpha==255))) {
            /* On addition Windows can expose no flags or an opaque default. */
            return true;
        } else if (now.flags==expected.flags && (!(now.flags&LWA_ALPHA) || now.alpha==expected.alpha) &&
            (!(now.flags&LWA_COLORKEY) || now.key==expected.key)) return true;
    }
    /* The app changed its layering independently. Don't overwrite its new state. */
    o->superseded=true;o->added_layered=false;o->applied_alpha=0;
    return true;
}
static bool opacity_target(SpWin32 *a, SpWindow w, bool visible_only) {
    return visible_only?sp_win32_member_visible(a,w):alive(a,w);
}
static bool change_layered_bit(SpWin32 *a, SpWin32Appearance *m, bool add, bool visible_only) {
    SpWin32Opacity *o=&m->opacity; SpWindow w=m->member; HWND h=(HWND)w.handle; DWORD_PTR ignored;
    if (!opacity_target(a,w,visible_only)) return false;
    /* A style change can send synchronous messages. Skip a stalled target rather
       than knowingly waiting on it in the UI thread. This probe is not a lock. */
    if (w.thread_id!=GetCurrentThreadId() &&
        !SendMessageTimeoutW(h,WM_NULL,0,0,SMTO_BLOCK|SMTO_ABORTIFHUNG|SMTO_ERRORONEXIT,SP_STYLE_PROBE_MS,&ignored))
        return false;
    LONG_PTR observed_style, style;
    if (!opacity_target(a,w,visible_only) || !observe_opacity(m,&observed_style)) return false;
    if (o->superseded) return !add;
    if (add && (GetClassLongPtrW(h,GCL_STYLE)&(CS_OWNDC|CS_CLASSDC|CS_PARENTDC))) return false;
    /* Win32 has no atomic style bit operation; preserve unrelated bits from
       the last available read and don't write if the layering mode moved. */
    if (!alive(a,w) || !read_exstyle(h,&style) || ((style^observed_style)&WS_EX_LAYERED) ||
        (add && (style&SP_LAYER_INCOMPATIBLE))) return false;
    SetLastError(ERROR_SUCCESS);
    if (!SetWindowLongPtrW(h,GWL_EXSTYLE,add?style|WS_EX_LAYERED:style&~(LONG_PTR)WS_EX_LAYERED) &&
        GetLastError()!=ERROR_SUCCESS) return false;
    if (add) o->added_layered=true; /* Track this even if the following alpha write fails. */
    if (!alive(a,w) || !read_exstyle(h,&style)) return false;
    if (((style&WS_EX_LAYERED)!=0)!=add) {
        if (add) o->added_layered=false;
        return false; /* The target can veto WM_STYLECHANGING. */
    }
    if (!add) {
        o->added_layered=false;o->applied_alpha=0;
        if (alive(a,w)) (void)RedrawWindow(h,NULL,NULL,RDW_ERASE|RDW_INVALIDATE|RDW_FRAME|RDW_ALLCHILDREN);
    }
    return true;
}
static bool change_alpha(SpWin32 *a, SpWin32Appearance *m, BYTE alpha, bool visible_only) {
    SpWin32Opacity *o=&m->opacity; SpWindow w=m->member; HWND h=(HWND)w.handle; LONG_PTR style;
    if (!opacity_target(a,w,visible_only) || !observe_opacity(m,&style)) return false;
    if (o->superseded) return true;
    /* WM_STYLECHANGING can add an incompatible flag while we add layering. */
    if (alpha && o->added_layered && (style&SP_LAYER_INCOMPATIBLE)) return change_layered_bit(a,m,false,visible_only);
    SpWin32LayeredAttrs attrs=desired_attrs(o,alpha);
    if (!(style&WS_EX_LAYERED) || !alive(a,w) || !SetLayeredWindowAttributes(h,attrs.key,attrs.alpha,attrs.flags))
        return false;
    o->applied_alpha=alpha;
    return true;
}
static bool restore_opacity(SpWin32 *a, SpWin32Appearance *m, bool visible_only) {
    if (m->opacity.added_layered) return change_layered_bit(a,m,false,visible_only);
    return !m->opacity.applied_alpha || change_alpha(a,m,0,visible_only);
}
static void update_visible_opacity(SpWin32 *a, const SpState *s, HWND focused, unsigned base, unsigned inactive,
    bool workspace_visible) {
    if (s->ctx!=a || base>255 || inactive>255) return;
    for (size_t i=0;s->visible==workspace_visible && i<s->count;++i) {
        SpWindow w=s->windows[i];
        BYTE alpha=(BYTE)((HWND)w.handle==focused?base:inactive);
        /* A claim property alone is not membership (e.g. window.center). */
        SpWin32Appearance *m=find_appearance(a,w);
        SpWin32Opacity *o=m?&m->opacity:NULL;
        if ((!alpha && (!o || (!o->applied_alpha && !o->added_layered))) ||
            (m && (m->busy || o->superseded)) || !sp_win32_member_visible(a,w)) continue;
        if (!o || !o->remembered) m=remember_opacity(a,w,m,alpha);
        if (!m) continue;
        o=&m->opacity;
        BYTE native=(o->original.flags&LWA_ALPHA)?o->original.alpha:255;
        if (alpha==native) alpha=0;
        if (alpha==o->applied_alpha && (alpha || !o->added_layered)) continue;
        m->busy=true;
        if (!alpha) (void)restore_opacity(a,m,true);
        else {
            bool ready=o->original.flags || o->added_layered || change_layered_bit(a,m,true,true);
            /* A synchronous style notification can let a nested Restore all run. */
            if (ready && s->visible) (void)change_alpha(a,m,alpha,true);
            if (o->added_layered && !o->applied_alpha) (void)restore_opacity(a,m,true);
        }
        m->busy=false;
    }
}
void sp_win32_apply_member_opacity(SpWin32 *a, const SpState *s, HWND focused, unsigned base, unsigned inactive) {
    update_visible_opacity(a,s,focused,base,inactive,true);
}
void sp_win32_restore_exposed_member_opacity(SpWin32 *a, const SpState *s) {
    update_visible_opacity(a,s,NULL,0,0,false);
}
static bool read_border(SpWin32 *a, HWND h, COLORREF *color) {
    /* DWM can expose a setter without a getter. Never infer DEFAULT from a
       failed read, a missing value or the color currently drawn on screen. */
    *color=0x01000000u;
    return a->get_border_attribute(h,DWMWA_BORDER_COLOR,color,sizeof(*color))==S_OK &&
        (*color<=0xffffffu || *color==DWMWA_COLOR_DEFAULT || *color==DWMWA_COLOR_NONE);
}
static bool write_border(SpWin32 *a, SpWindow w, COLORREF color) {
    return alive(a,w) && a->set_border_attribute((HWND)w.handle,DWMWA_BORDER_COLOR,&color,sizeof(color))==S_OK;
}
static bool border_pending(const SpWin32Border *b) {
    return b->state==SP_BORDER_APPLIED || b->state==SP_BORDER_VERIFY_RESTORE;
}
static void observe_border(SpWin32Border *b, COLORREF now) {
    if (b->state==SP_BORDER_VERIFY_RESTORE && now==b->original) b->state=SP_BORDER_NATIVE;
    else if ((border_pending(b) && now!=b->applied_color) || (b->state==SP_BORDER_NATIVE && now!=b->original))
        b->state=SP_BORDER_SUPERSEDED; /* Do not resnapshot an app takeover on this claim. */
}
static bool restore_border(SpWin32 *a, SpWin32Appearance *m) {
    SpWin32Border *b=&m->border;
    if (!border_pending(b)) return true;
    SpWindow w=m->member; HWND h=(HWND)w.handle;
    COLORREF now;
    if (!alive(a,w) || !read_border(a,h,&now)) return false;
    observe_border(b,now);
    if (!border_pending(b)) return true;
    if (!write_border(a,w,b->original)) return false;
    b->state=SP_BORDER_VERIFY_RESTORE;
    /* If DWM did not confirm removal, don't drop a live claim while our
       value can still be there. A different app value must be left alone. */
    if (!alive(a,w) || !read_border(a,h,&now)) return false;
    observe_border(b,now);
    return !border_pending(b);
}
static void apply_border(SpWin32 *a, const SpState *s, SpWindow w, COLORREF color) {
    SpWin32Appearance *m=find_appearance(a,w);
    if (m && (m->busy || m->border.state==SP_BORDER_SUPERSEDED)) return;
    if (!s->visible || !sp_win32_member_visible(a,w)) return;
    if (!m) m=reserve_appearance(a,w);
    if (!m) return;
    m->busy=true;
    SpWin32Border *b=&m->border;
    COLORREF now;
    if (read_border(a,(HWND)w.handle,&now)) {
        observe_border(b,now);
        if (b->state!=SP_BORDER_SUPERSEDED && now!=color && sp_win32_member_visible(a,w) && s->visible && write_border(a,w,color)) {
            if (b->state==SP_BORDER_UNSET) b->original=now;
            b->applied_color=color;
            b->state=SP_BORDER_APPLIED;
        }
    }
    finish_appearance(m,false);
}
void sp_win32_apply_member_border(SpWin32 *a, const SpState *s, HWND focused, unsigned rgb) {
    if (s->ctx!=a || rgb>0xffffffu) return;
    /* Restore before applying the next highlight. A hidden/minimized member
       can still carry our DWM override; attempt to restore it there too. */
    for (size_t i=0;i<SP_CAPACITY;++i) {
        SpWin32Appearance *m=&a->appearance[i];
        if (m->busy || !border_pending(&m->border)) continue;
        if (s->visible && (HWND)m->member.handle==focused && sp_win32_member_visible(a,m->member)) continue;
        m->busy=true;
        (void)restore_border(a,m);
        m->busy=false;
    }
    /* Only a current scratch member can get a new override, not a temporary
       window.* claim or the untracked child/dialog used to resolve focus. */
    for (size_t i=0;focused && s->visible && i<s->count;++i) {
        SpWindow w=s->windows[i];
        if ((HWND)w.handle==focused) {
            apply_border(a,s,w,RGB(rgb>>16,rgb>>8,rgb));
            break;
        }
    }
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
    SpWin32Appearance *m=find_appearance(a,w);
    if (m && m->busy) {
        wcscpy(a->error,L"Window appearance is still being updated; retry Restore all.");return false;
    }
    if (!m) m=reserve_appearance(a,w); /* Block a reentrant first write during release. */
    bool released=false;
    if (m) {
        /* Restore even a hidden member, before showing it or dropping its claim. */
        m->busy=true;
        bool opacity_ok=restore_opacity(a,m,false);
        bool border_ok=restore_border(a,m);
        if (!opacity_ok || !border_ok) {
            wcscpy(a->error,L"Could not restore window appearance. Let the target respond, then retry Restore all.");
            goto done;
        }
    }
    if (!alive(a,w)) goto done;
    if (!SetWindowPos(h,HWND_NOTOPMOST,0,0,0,0,
        SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE|SWP_ASYNCWINDOWPOS|SWP_SHOWWINDOW)) {
        (void)fail(a,L"Cannot release window");goto done;
    }
    if (!wait_state(a,w,true,false,false,NULL) || !alive(a,w)) goto done;
    RemovePropW(h,a->pending_tag);RemovePropW(h,a->pending_tick_tag);RemovePropW(h,a->tag);
    released=true;
done:
    if (m) finish_appearance(m,released);
    return released;
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
