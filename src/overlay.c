#include "overlay.h"
#include "shield.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#define SP_OVERLAY_APPLY (WM_APP+70)
#define SP_OVERLAY_WAIT 1500u
#define SP_OVERLAY_ORDER_TIMER 1u
#define SP_OVERLAY_FRAME_MS 16u
#define SP_OVERLAY_ALPHA_ATTEMPTS 3u
#define SP_Z_LIMIT 4096u

typedef enum { SURFACE_HIDE_NOW, SURFACE_SHOW, SURFACE_HIDE_FADE } SurfaceAction;
typedef struct {
    SpWindow members[SP_CAPACITY];
    size_t count;
    SpRect work;
    uintptr_t generation;
    SurfaceAction action;
} Surface;
typedef struct {
    UINT_PTR timer;
    uintptr_t generation;
    ULONGLONG started;
    BYTE from;
    bool hiding;
    unsigned alpha_failures;
} Fade;
struct SpOverlay {
    HWND host,window;
    HANDLE thread,ready,stop;
    SRWLOCK lock;
    Surface pending,live;
    SpOverlayHideResult hide_result; /* Deferred result for pending.generation, under lock. */
    wchar_t member_tag[96],error[256],reported_error[256];
    BYTE target_alpha; /* Endpoint and duration immutable after starting the thread. */
    UINT fade_ms;
    BYTE alpha; /* Last successful native write; owner thread only. */
    Fade fade;
    UINT_PTR timer_serial;
    bool shown,neutral;
    uint64_t key_epoch;
    SpShieldPress keys[768],buttons[5];
};
/* Callbacks and live state belong exclusively to the independent surface thread. */
static SpOverlay *current_overlay;

static void overlay_error(SpOverlay *o,const wchar_t *message,DWORD code) {
    AcquireSRWLockExclusive(&o->lock);
    swprintf(o->error,256,L"%ls (Windows error %lu).",message,(unsigned long)code);
    ReleaseSRWLockExclusive(&o->lock);
}
const wchar_t *sp_overlay_error(const SpOverlay *overlay) {
    SpOverlay *o=(SpOverlay *)overlay;
    if (!o) return L"Workspace input surface is unavailable.";
    AcquireSRWLockShared(&o->lock);
    wcscpy(o->reported_error,o->error);
    ReleaseSRWLockShared(&o->lock);
    return o->reported_error;
}
static bool member(const SpOverlay *o,HWND h) {
    for (size_t i=0;i<o->live.count;++i) {
        const SpWindow *w=&o->live.members[i];
        if ((HWND)w->handle!=h) continue;
        DWORD pid=0,tid=GetWindowThreadProcessId(h,&pid);
        return tid==w->thread_id && pid==w->process_id &&
            (uintptr_t)GetPropW(h,o->member_tag)==w->cookie;
    }
    return false;
}
static bool family(const SpOverlay *o,HWND h) {
    h=GetAncestor(h,GA_ROOT);
    for (unsigned i=0;h && i<SP_CAPACITY;++i,h=GetWindow(h,GW_OWNER))
        if (h==o->window || h==o->host || member(o,h)) return true;
    return false;
}
static RECT work_rect(const SpOverlay *o) {
    SpRect r=o->live.work; return (RECT){r.x,r.y,r.x+r.w,r.y+r.h};
}
/* Only windows actually underneath this monitor-local surface are shielded.
   Other monitors, the taskbar and unrelated popups above it remain usable. */
static bool underneath(const SpOverlay *o,HWND h) {
    if (!o->shown || !h || family(o,h)) return false;
    h=GetAncestor(h,GA_ROOT);
    RECT r,intersection,area=work_rect(o);
    if (!GetWindowRect(h,&r) || !IntersectRect(&intersection,&r,&area)) return false;
    HWND next=GetWindow(o->window,GW_HWNDNEXT);
    for (unsigned i=0;next && i<SP_Z_LIMIT;++i,next=GetWindow(next,GW_HWNDNEXT))
        if (next==h) return true;
    return false;
}
static HWND lowest_member(const SpOverlay *o) {
    HWND last=HWND_TOPMOST,h=GetTopWindow(NULL);
    for (unsigned i=0;h && i<SP_Z_LIMIT;++i,h=GetWindow(h,GW_HWNDNEXT))
        if (IsWindowVisible(h) && (GetWindowLongPtrW(h,GWL_EXSTYLE)&WS_EX_TOPMOST) && member(o,h)) last=h;
    return last;
}
static void focus_surface(SpOverlay *o) {
    /* Do not steal foreground from another monitor or a popup above the workspace. */
    if (!underneath(o,GetForegroundWindow())) return;
    for (size_t i=o->live.count;i>0;--i) {
        HWND h=(HWND)o->live.members[i-1].handle;
        if (member(o,h) && IsWindowVisible(h) && !IsIconic(h)) { (void)SetForegroundWindow(h); return; }
    }
    (void)SetForegroundWindow(o->window);
}
static LRESULT CALLBACK overlay_keyboard(int code,WPARAM wp,LPARAM lp) {
    LRESULT next=CallNextHookEx(NULL,code,wp,lp);
    SpOverlay *o=current_overlay;
    if (code<0 || !o) return next;
    bool down=wp==WM_KEYDOWN || wp==WM_SYSKEYDOWN,up=wp==WM_KEYUP || wp==WM_SYSKEYUP;
    if (!down && !up) return next;
    const KBDLLHOOKSTRUCT *key=(const KBDLLHOOKSTRUCT *)lp;
    unsigned scan=(key->scanCode&0xff)|((key->flags&LLKHF_EXTENDED)?0x100:0);
    unsigned slot=sp_shield_key(key->vkCode,scan);
    if (slot>=768) return next;
    ++o->key_epoch;
    /* Let the configured hotkey hook see recovery chords first. Synthetic input
       is not a bypass for this guard (the chord matcher still ignores it). */
    bool blocked=sp_shield_press(&o->keys[slot],down,!next && underneath(o,GetForegroundWindow()));
    return next?next:blocked?1:0;
}
static bool pointer_blocked(const SpOverlay *o,POINT point,bool wheel,bool movement) {
    if (!o->shown) return false;
    RECT area=work_rect(o);
    if (!PtInRect(&area,point)) return false;
    GUITHREADINFO gui={0};gui.cbSize=sizeof(gui);
    if (GetGUIThreadInfo(0,&gui) && gui.hwndCapture) {
        if (family(o,gui.hwndCapture)) return false; /* Preserve scratch dragging. */
        if (underneath(o,gui.hwndCapture)) return true;
    }
    if (movement) return false; /* Let the real surface receive hover/cursor updates. */
    HWND hit=WindowFromPoint(point);
    if (wheel && underneath(o,GetForegroundWindow())) return true;
    return hit==o->window || underneath(o,hit);
}
static LRESULT CALLBACK overlay_mouse(int code,WPARAM wp,LPARAM lp) {
    LRESULT next=CallNextHookEx(NULL,code,wp,lp);
    SpOverlay *o=current_overlay;
    if (code<0 || !o) return next;
    const MSLLHOOKSTRUCT *mouse=(const MSLLHOOKSTRUCT *)lp;
    bool wheel=wp==WM_MOUSEWHEEL || wp==WM_MOUSEHWHEEL;
    bool block=!next && pointer_blocked(o,mouse->pt,wheel,wp==WM_MOUSEMOVE);
    unsigned button=5;bool down=false;
    switch (wp) {
    case WM_LBUTTONDOWN: down=true; /* fall through */
    case WM_LBUTTONUP: button=0;break;
    case WM_RBUTTONDOWN: down=true; /* fall through */
    case WM_RBUTTONUP: button=1;break;
    case WM_MBUTTONDOWN: down=true; /* fall through */
    case WM_MBUTTONUP: button=2;break;
    case WM_XBUTTONDOWN: down=true; /* fall through */
    case WM_XBUTTONUP: button=HIWORD(mouse->mouseData)==XBUTTON1?3:4;break;
    }
    if (button<5) block=sp_shield_press(&o->buttons[button],down,block);
    return next?next:block?1:0;
}
static void poll_neutral(SpOverlay *o) {
    if (o->neutral) return;
    uint64_t epoch=o->key_epoch;bool up=true;
    /* Install before polling, and invalidate polls reentered by a key event.
       Never turn a startup-held key's unknown down into an owned release. */
    for (int vk=1;vk<256;++vk) if (GetAsyncKeyState(vk)&0x8000) up=false;
    if (epoch!=o->key_epoch || !up) return;
    for (size_t i=0;i<768;++i) if (o->keys[i].pressed) return;
    o->neutral=true;
}
static bool mouse_busy(const SpOverlay *o) {
    const int keys[]={VK_LBUTTON,VK_RBUTTON,VK_MBUTTON,VK_XBUTTON1,VK_XBUTTON2};
    for (size_t i=0;i<sizeof(keys)/sizeof(keys[0]);++i) if (GetAsyncKeyState(keys[i])&0x8000) return true;
    GUITHREADINFO gui={0};gui.cbSize=sizeof(gui);
    return GetGUIThreadInfo(0,&gui) && !family(o,gui.hwndActive) &&
        (gui.hwndCapture || (gui.flags&(GUI_INMENUMODE|GUI_INMOVESIZE)));
}
static bool set_alpha(SpOverlay *o,BYTE alpha) {
    if (!alpha) alpha=1; /* Alpha 0 makes the layered window click-through. */
    if (o->alpha==alpha) return true;
    if (!SetLayeredWindowAttributes(o->window,0,alpha,LWA_ALPHA)) return false;
    o->alpha=alpha;return true;
}
static void cancel_fade(SpOverlay *o) {
    UINT_PTR timer=o->fade.timer;
    o->fade.timer=0;
    if (timer) (void)KillTimer(o->window,timer);
}
static void tint_error(SpOverlay *o) {
    overlay_error(o,L"Cannot apply workspace tint; the input shield remains active",GetLastError());
}
static void finish_fade_in(SpOverlay *o) {
    if (set_alpha(o,o->target_alpha)) cancel_fade(o);
    else if (++o->fade.alpha_failures>=SP_OVERLAY_ALPHA_ATTEMPTS) {
        tint_error(o);
        cancel_fade(o);
    }
}
static bool schedule_fade(SpOverlay *o,uintptr_t generation,bool hiding) {
    if (o->fade.timer && o->fade.hiding==hiding) {
        /* Adopt the timer without resetting its tick or deadline. Don't wait
           for another tick to try the endpoint if messages starved the fade. */
        o->fade.generation=generation;
        if (GetTickCount64()-o->fade.started>=o->fade_ms) {
            if (hiding) return false; /* Apply will confirm native hide now. */
            finish_fade_in(o);
        }
        return true;
    }
    cancel_fade(o);
    /* KillTimer does not remove queued WM_TIMERs. Give each fade its own ID,
       and dispatch only via the owning window, never via a TIMERPROC. */
    if (++o->timer_serial<=SP_OVERLAY_ORDER_TIMER) o->timer_serial=SP_OVERLAY_ORDER_TIMER+1;
    UINT_PTR timer=o->timer_serial;
    if (!SetTimer(o->window,timer,SP_OVERLAY_FRAME_MS,NULL)) return false;
    /* Reversing starts at the last alpha actually applied to the window. */
    o->fade=(Fade){.timer=timer,.generation=generation,.started=GetTickCount64(),.from=o->alpha,.hiding=hiding};
    return true;
}
static bool current_request(SpOverlay *o,uintptr_t generation) {
    AcquireSRWLockShared(&o->lock);
    bool current=o->pending.generation==generation;
    ReleaseSRWLockShared(&o->lock);
    return current;
}
static bool hide_surface(SpOverlay *o) {
    cancel_fade(o);
    /* ShowWindow returns the previous visibility, not a success indication.
       Keep the guard and live state until the actual native surface is hidden. */
    SetLastError(ERROR_SUCCESS);
    (void)ShowWindow(o->window,SW_HIDE);
    if (IsWindowVisible(o->window)) {
        DWORD error=GetLastError();
        /* Refusal leaves a live shield. Make it apparent rather than leaving
           a nearly invisible input blocker at the end of the fade. */
        (void)set_alpha(o,o->target_alpha);
        overlay_error(o,L"Cannot remove the workspace input surface; retry Restore all",error?error:ERROR_BUSY);
        return false;
    }
    o->shown=false;KillTimer(o->window,SP_OVERLAY_ORDER_TIMER);return true;
}
static void fade_tick(SpOverlay *o,UINT_PTR timer) {
    Fade fade=o->fade;
    if (!fade.timer || timer!=fade.timer) return;
    if (!o->shown) {cancel_fade(o);return;}
    if (!current_request(o,fade.generation)) return; /* New apply will adopt or cancel this timer. */

    ULONGLONG elapsed=GetTickCount64()-fade.started;
    if (!fade.alpha_failures && elapsed<o->fade_ms) {
        BYTE end=fade.hiding?1:o->target_alpha;
        BYTE alpha=(BYTE)((fade.from*(o->fade_ms-elapsed)+end*elapsed+o->fade_ms/2)/o->fade_ms);
        /* Round-to-target frames must use the same bounded endpoint write. */
        if ((fade.hiding || alpha!=end) && set_alpha(o,alpha)) return;
        /* A visual write failure must not strand a pending hide. */
    }
    if (!fade.hiding) {finish_fade_in(o);return;}
    bool hidden=hide_surface(o);
    AcquireSRWLockExclusive(&o->lock);
    bool current=o->pending.generation==fade.generation;
    if (current) {
        if (hidden) o->live=o->pending;
        o->hide_result=hidden?SP_OVERLAY_HIDE_DONE:SP_OVERLAY_HIDE_FAILED;
    }
    ReleaseSRWLockExclusive(&o->lock);
    /* The result remains queryable if the host queue cannot accept a wake-up. */
    if (current) (void)PostMessageW(o->host,SP_OVERLAY_HIDE_COMPLETE,(WPARAM)fade.generation,0);
}
static LRESULT apply_surface(SpOverlay *o,uintptr_t generation) {
    Surface next;
    AcquireSRWLockShared(&o->lock);next=o->pending;ReleaseSRWLockShared(&o->lock);
    if (generation!=next.generation) return 0;
    if (next.action!=SURFACE_SHOW) {
        if (next.action==SURFACE_HIDE_FADE && o->shown && o->fade_ms && o->alpha>1 &&
            schedule_fade(o,generation,true)) return SP_OVERLAY_HIDE_PENDING;
        if (!hide_surface(o)) return 0;
        o->live=next;return 1;
    }
    if (o->fade.hiding) cancel_fade(o);
    poll_neutral(o);
    if (!o->shown && (!o->neutral || mouse_busy(o))) {
        overlay_error(o,L"Release startup-held keys and mouse buttons/menus before showing the scratchpad",ERROR_BUSY);return 0;
    }
    bool entering=!o->shown;
    if (entering && o->fade_ms) (void)set_alpha(o,1);
    o->live=next;o->shown=true;
    if (!SetWindowPos(o->window,lowest_member(o),next.work.x,next.work.y,next.work.w,next.work.h,
        SWP_NOACTIVATE|SWP_SHOWWINDOW)) {
        overlay_error(o,L"Cannot show the workspace input surface",GetLastError());
        (void)hide_surface(o);return 0;
    }
    if (entering || !next.count) (void)SetForegroundWindow(o->window);
    if (!SetTimer(o->window,SP_OVERLAY_ORDER_TIMER,100,NULL)) {
        overlay_error(o,L"Cannot maintain workspace input surface ordering",GetLastError());
        (void)hide_surface(o);return 0;
    }
    if (!current_request(o,generation)) {(void)hide_surface(o);return 0;}
    if (o->alpha!=o->target_alpha && o->fade_ms && schedule_fade(o,generation,false)) return 1;
    cancel_fade(o);
    /* Without an animation timer, do not dismantle an otherwise valid shield
       if a cosmetic write fails; its last successfully applied alpha is safe. */
    if (!set_alpha(o,o->target_alpha)) tint_error(o);
    return 1;
}
static LRESULT CALLBACK overlay_proc(HWND h,UINT msg,WPARAM wp,LPARAM lp) {
    SpOverlay *o=(SpOverlay *)GetWindowLongPtrW(h,GWLP_USERDATA);
    if (msg==WM_NCCREATE) {
        o=((CREATESTRUCTW *)lp)->lpCreateParams;
        SetWindowLongPtrW(h,GWLP_USERDATA,(LONG_PTR)o);o->window=h;
    }
    if (o) switch (msg) {
    case SP_OVERLAY_APPLY: return apply_surface(o,(uintptr_t)wp);
    case WM_NCHITTEST: return HTCLIENT;
    case WM_MOUSEACTIVATE: return MA_NOACTIVATEANDEAT;
    case WM_LBUTTONDOWN:case WM_LBUTTONUP:case WM_LBUTTONDBLCLK:
    case WM_RBUTTONDOWN:case WM_RBUTTONUP:case WM_RBUTTONDBLCLK:
    case WM_MBUTTONDOWN:case WM_MBUTTONUP:case WM_XBUTTONDOWN:case WM_XBUTTONUP:
    case WM_MOUSEWHEEL:case WM_MOUSEHWHEEL:case WM_CONTEXTMENU:case WM_CLOSE: return 0;
    case WM_WINDOWPOSCHANGING:
        if (o->shown && !(((WINDOWPOS *)lp)->flags&SWP_NOZORDER))
            ((WINDOWPOS *)lp)->hwndInsertAfter=lowest_member(o);
        break;
    case WM_TIMER:
        if (wp==SP_OVERLAY_ORDER_TIMER) {
            if (o->shown) {
                (void)SetWindowPos(h,lowest_member(o),0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE);
                focus_surface(o);
            }
        } else fade_tick(o,(UINT_PTR)wp);
        return 0;
    case WM_NCDESTROY:
        cancel_fade(o);
        o->shown=false;
        SetWindowLongPtrW(h,GWLP_USERDATA,0);
        break;
    }
    return DefWindowProcW(h,msg,wp,lp);
}
static DWORD WINAPI overlay_thread(void *ctx) {
    SpOverlay *o=ctx;HINSTANCE instance=GetModuleHandleW(NULL);
    (void)SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    WNDCLASSW cls={0};cls.lpfnWndProc=overlay_proc;cls.hInstance=instance;
    cls.lpszClassName=SP_OVERLAY_CLASS;cls.hbrBackground=GetStockObject(BLACK_BRUSH);cls.hCursor=LoadCursorW(NULL,IDC_ARROW);
    HHOOK keyboard=NULL,mouse=NULL;bool registered=RegisterClassW(&cls)!=0;
    if (registered) {
        /* Deliberately unowned: a cross-thread owner attaches input queues and
           can deadlock show/destroy while the host waits for this thread. The
           host explicitly joins us before releasing application storage. */
        o->window=CreateWindowExW(WS_EX_LAYERED|WS_EX_TOOLWINDOW|WS_EX_TOPMOST,cls.lpszClassName,
            L"CloakSpace workspace",WS_POPUP,0,0,0,0,NULL,NULL,instance,o);
        if (o->window && set_alpha(o,o->target_alpha)) {
            current_overlay=o;
            keyboard=SetWindowsHookExW(WH_KEYBOARD_LL,overlay_keyboard,instance,0);
            mouse=SetWindowsHookExW(WH_MOUSE_LL,overlay_mouse,instance,0);
        }
    }
    bool ready=o->window && keyboard && mouse;
    if (!ready) overlay_error(o,L"Cannot initialize the workspace input surface",GetLastError());
    SetEvent(o->ready);
    if (ready) {
        poll_neutral(o);
        MSG message;bool quitting=false;
        while (!quitting) {
            DWORD wait=MsgWaitForMultipleObjects(1,&o->stop,FALSE,o->neutral?INFINITE:25,QS_ALLINPUT);
            if (wait!=WAIT_OBJECT_0+1 && wait!=WAIT_TIMEOUT) break;
            while (PeekMessageW(&message,NULL,0,0,PM_REMOVE)) {
                if (message.message==WM_QUIT || WaitForSingleObject(o->stop,0)==WAIT_OBJECT_0) {quitting=true;break;}
                TranslateMessage(&message);DispatchMessageW(&message);
            }
            poll_neutral(o);
        }
    }
    o->shown=false;
    if (mouse) UnhookWindowsHookEx(mouse);if (keyboard) UnhookWindowsHookEx(keyboard);
    if (current_overlay==o) current_overlay=NULL;
    if (o->window) {DestroyWindow(o->window);o->window=NULL;}
    if (registered) UnregisterClassW(cls.lpszClassName,instance);
    return ready?0:1;
}
SpOverlay *sp_overlay_create_with_fade(HWND host,const wchar_t *tag,BYTE alpha,UINT fade_ms,
    wchar_t *error,size_t capacity) {
    SpOverlay *o=calloc(1,sizeof(*o));
    if (!o) {swprintf(error,capacity,L"Cannot allocate the workspace input surface.");return NULL;}
    o->host=host;o->target_alpha=alpha?alpha:1;o->fade_ms=fade_ms;InitializeSRWLock(&o->lock);
    if (!host || !tag || wcslen(tag)>=96) {free(o);swprintf(error,capacity,L"Invalid workspace input surface owner/tag.");return NULL;}
    wcscpy(o->member_tag,tag);
    o->ready=CreateEventW(NULL,TRUE,FALSE,NULL);o->stop=CreateEventW(NULL,TRUE,FALSE,NULL);
    if (o->ready && o->stop) o->thread=CreateThread(NULL,0,overlay_thread,o,0,NULL);
    bool signaled=o->thread && WaitForSingleObject(o->ready,3000)==WAIT_OBJECT_0;
    AcquireSRWLockShared(&o->lock);bool has_error=o->error[0]!=0;ReleaseSRWLockShared(&o->lock);
    bool ready=signaled && !has_error;
    if (ready) return o;
    if (!has_error) overlay_error(o,L"Cannot start the workspace input surface",ERROR_NOT_READY);
    swprintf(error,capacity,L"%ls",sp_overlay_error(o));sp_overlay_destroy(o);return NULL;
}
SpOverlay *sp_overlay_create(HWND host,const wchar_t *tag,BYTE alpha,wchar_t *error,size_t capacity) {
    return sp_overlay_create_with_fade(host,tag,alpha,0,error,capacity);
}
/* Caller holds the lock. Never issue the no-request token (zero). */
static uintptr_t next_request(SpOverlay *o,SurfaceAction action) {
    o->pending.action=action;o->hide_result=SP_OVERLAY_HIDE_PENDING;
    if (++o->pending.generation==0) ++o->pending.generation;
    return o->pending.generation;
}
static SpOverlayHideResult request_surface(SpOverlay *o,const SpState *state,SurfaceAction action,
    SpRect work,uintptr_t *request) {
    if (request) *request=0;
    if (!o || !o->window || !state || state->count>SP_CAPACITY) return SP_OVERLAY_HIDE_FAILED;
    /* Core preflight already checked geometry; keep the public seam defensive. */
    if (action==SURFACE_SHOW && (work.w<=0 || work.h<=0 || (int64_t)work.x+work.w>INT32_MAX ||
        (int64_t)work.y+work.h>INT32_MAX)) return SP_OVERLAY_HIDE_FAILED;
    AcquireSRWLockExclusive(&o->lock);
    o->pending.count=state->count;memcpy(o->pending.members,state->windows,state->count*sizeof(SpWindow));
    o->pending.work=work;
    uintptr_t generation=next_request(o,action);
    ReleaseSRWLockExclusive(&o->lock);
    /* Wait only for the native setup/scheduling decision, never for a frame. */
    DWORD_PTR applied=0;
    bool sent=SendMessageTimeoutW(o->window,SP_OVERLAY_APPLY,(WPARAM)generation,0,
        SMTO_BLOCK|SMTO_ABORTIFHUNG,SP_OVERLAY_WAIT,&applied)!=0;
    if (sent && applied) {
        if (request && applied==SP_OVERLAY_HIDE_PENDING) *request=generation;
        return (SpOverlayHideResult)applied;
    }
    /* Invalidate delayed entry/ticks and try immediate cleanup. No message
       contains a caller-owned pointer that can outlive a timeout. */
    AcquireSRWLockExclusive(&o->lock);
    uintptr_t cleanup=o->pending.generation==generation?next_request(o,SURFACE_HIDE_NOW):0;
    ReleaseSRWLockExclusive(&o->lock);
    if (cleanup) (void)PostMessageW(o->window,SP_OVERLAY_APPLY,(WPARAM)cleanup,0);
    if (!sent) overlay_error(o,action==SURFACE_SHOW?L"Workspace surface did not acknowledge; activation cancelled":
        L"Workspace deactivation not acknowledged; input may remain blocked; retry Restore all",ERROR_TIMEOUT);
    return SP_OVERLAY_HIDE_FAILED;
}
bool sp_overlay_workspace(void *ctx,const SpState *state,bool active,SpRect work) {
    return request_surface(ctx,state,active?SURFACE_SHOW:SURFACE_HIDE_NOW,work,NULL)==SP_OVERLAY_HIDE_DONE;
}
SpOverlayHideResult sp_overlay_hide(SpOverlay *o,const SpState *state,uintptr_t *request) {
    if (!request) return SP_OVERLAY_HIDE_FAILED;
    return request_surface(o,state,SURFACE_HIDE_FADE,(SpRect){0},request);
}
bool sp_overlay_hide_completion(SpOverlay *o,uintptr_t request,bool *hidden) {
    if (!o || !request || !hidden) return false;
    AcquireSRWLockShared(&o->lock);
    bool complete=o->pending.generation==request && o->pending.action==SURFACE_HIDE_FADE &&
        o->hide_result!=SP_OVERLAY_HIDE_PENDING;
    if (complete) *hidden=o->hide_result==SP_OVERLAY_HIDE_DONE;
    ReleaseSRWLockShared(&o->lock);
    return complete;
}
void sp_overlay_destroy(SpOverlay *o) {
    if (!o) return;
    if (o->stop) SetEvent(o->stop);
    if (o->thread) {WaitForSingleObject(o->thread,INFINITE);CloseHandle(o->thread);}
    if (o->ready) CloseHandle(o->ready);if (o->stop) CloseHandle(o->stop);
    free(o);
}
