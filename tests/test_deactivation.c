#include "host.h"
#include "overlay.h"
#include "process.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

/* Replay the actual core, native surface and host/control client together.
   Hooks, foreground and member operations are fixtures: no global input, real
   application members, foreground changes or production fault switches. */
static LONG checks,fail_hide,delay_hide,fail_timer,fail_callback,hook_count;
static LONG drop_next_completion,hold_next_hide;
static LONG64 animation_clock;
static HWND fixture_background;
static void fixture_wait_for_resume(void);
static HHOOK WINAPI fixture_hook(int id,HOOKPROC proc,HINSTANCE module,DWORD thread) {
    (void)proc;(void)module;(void)thread;InterlockedIncrement(&hook_count);return (HHOOK)(UINT_PTR)id;
}
static BOOL WINAPI fixture_unhook(HHOOK hook){(void)hook;InterlockedDecrement(&hook_count);return TRUE;}
static LRESULT WINAPI fixture_next(HHOOK hook,int code,WPARAM wp,LPARAM lp){(void)hook;(void)code;(void)wp;(void)lp;return 0;}
static HWND WINAPI fixture_foreground(void){return fixture_background;}
static BOOL WINAPI fixture_focus(HWND h){(void)h;return TRUE;}
static SHORT WINAPI fixture_async(int vk){(void)vk;return 0;}
static BOOL WINAPI fixture_gui(DWORD thread,PGUITHREADINFO gui){(void)thread;gui->hwndActive=fixture_background;return TRUE;}
static BOOL WINAPI fixture_show(HWND h,int command){
    if(command==SW_HIDE){
        if(InterlockedExchange(&hold_next_hide,0))fixture_wait_for_resume();
        LONG delay=InterlockedExchange(&delay_hide,0);if(delay)Sleep((DWORD)delay);
        if(InterlockedCompareExchange(&fail_hide,0,0)){SetLastError(ERROR_ACCESS_DENIED);return IsWindowVisible(h);}
    }
    return ShowWindow(h,command);
}
static UINT_PTR WINAPI fixture_timer(HWND h,UINT_PTR id,UINT elapsed,TIMERPROC proc){
    if(InterlockedCompareExchange(&fail_timer,0,0)){SetLastError(ERROR_NOT_ENOUGH_MEMORY);return 0;}
    return SetTimer(h,id,elapsed,proc);
}
static ULONGLONG WINAPI fixture_clock(void){return (ULONGLONG)InterlockedCompareExchange64(&animation_clock,0,0);}
static BOOL WINAPI fixture_post(HWND h,UINT msg,WPARAM wp,LPARAM lp){
    if(msg==SP_OVERLAY_HIDE_COMPLETE && InterlockedExchange(&drop_next_completion,0)){
        SetLastError(ERROR_NOT_ENOUGH_QUOTA);return FALSE;
    }
    return PostMessageW(h,msg,wp,lp);
}
#define SetWindowsHookExW fixture_hook
#define UnhookWindowsHookEx fixture_unhook
#define CallNextHookEx fixture_next
#define GetForegroundWindow fixture_foreground
#define SetForegroundWindow fixture_focus
#define GetAsyncKeyState fixture_async
#define GetGUIThreadInfo fixture_gui
#define ShowWindow fixture_show
#define SetTimer fixture_timer
#define GetTickCount64 fixture_clock
#define PostMessageW fixture_post
#include "../src/overlay.c"
#undef SetWindowsHookExW
#undef UnhookWindowsHookEx
#undef CallNextHookEx
#undef GetForegroundWindow
#undef SetForegroundWindow
#undef GetAsyncKeyState
#undef GetGUIThreadInfo
#undef ShowWindow
#undef SetTimer
#undef PostMessageW
#include "../src/main.c"
#undef GetTickCount64

#define CHECK(x) do{InterlockedIncrement(&checks);if(!(x)){fprintf(stderr,"%s:%d: %s\n",__FILE__,__LINE__,#x);exit(1);}}while(0)
#define FIXTURE_RESET (WM_APP+180)
#define FIXTURE_TOGGLE (WM_APP+181)
#define FIXTURE_STOP (WM_APP+182)
#define FIXTURE_STALL (WM_APP+183)
#define FIXTURE_PREPARE (WM_APP+184)
#define FIXTURE_INSPECT (WM_APP+185)
#define FIXTURE_MEMBERS (WM_APP+186)
#define FIXTURE_ENABLE_FADE (WM_APP+187)
#define FIXTURE_FADE_TICK (WM_APP+188)
#define FIXTURE_HOLD_WORKER (WM_APP+189)
enum { FIXTURE_FADE_MS=400 };

typedef struct {
    bool native_visible,surface_shown,underneath;
    LRESULT key_down,key_up,wheel;
    UINT_PTR timer;
    BYTE alpha;
} FixtureSurface;
typedef struct {
    unsigned hides,tiles,releases,focuses,waits_with_shield;
    bool claimed[3],shown[3],closing,hide_watchdog;
    uintptr_t hide_request;
    UINT_PTR failed_entry_timer;
    SpResult result;
} FixtureMembers;
static HANDLE host_thread,host_ready,stall_entered,stall_resume,surface_inspected,members_inspected;
static HANDLE worker_resume;
static DWORD host_thread_id;
static HWND fixture_host,fixture_surface;
static SpRect fixture_area;
static wchar_t host_class[128],self[32768];
static unsigned hide_calls,tile_calls,release_calls,focus_calls,member_waits_with_shield;
static bool claimed[3],shown[3],fail_after_show;
static UINT_PTR failed_entry_timer;
/* Each owner publishes once before signaling its event; the harness copies only
   after waiting and never requests another publication until that copy is done. */
static FixtureSurface surface_observation;
static FixtureMembers member_observation;
static SpResult fixture_result;
static bool surface_stalled; /* Harness-owned: use the frozen stall snapshot. */

static void fixture_wait_for_resume(void){
    CHECK(SetEvent(stall_entered));
    CHECK(WaitForSingleObject(stall_resume,15000)==WAIT_OBJECT_0);
}
static bool fixture_claim_member(void *ctx,uintptr_t handle,SpWindow *window){
    (void)ctx;if(!handle||handle>2)return false;claimed[handle]=true;
    *window=(SpWindow){handle,1,1,1};return true;
}
static bool fixture_alive_member(void *ctx,SpWindow window){(void)ctx;return window.handle>0&&window.handle<=2&&claimed[window.handle];}
static bool fixture_hide_member(void *ctx,SpWindow window){
    (void)ctx;++hide_calls;if(IsWindowVisible(fixture_surface))++member_waits_with_shield;
    shown[window.handle]=false;return true;
}
static bool fixture_tile_member(void *ctx,SpWindow window,SpRect rect){(void)ctx;(void)rect;++tile_calls;shown[window.handle]=true;return true;}
static bool fixture_release_member(void *ctx,SpWindow window){
    (void)ctx;++release_calls;if(IsWindowVisible(fixture_surface))++member_waits_with_shield;
    claimed[window.handle]=false;shown[window.handle]=true;return true;
}
static bool fixture_focus_member(void *ctx,SpWindow window){++focus_calls;return fixture_alive_member(ctx,window);}
/* Advance only on the owner, in the same dispatch as the selected tick. No
   recovery request can wait for the clock to move unless the test moves it. */
static UINT_PTR fixture_tick(UINT_PTR id,UINT ms){
    DWORD_PTR timer=0;
    CHECK(SendMessageTimeoutW(fixture_surface,FIXTURE_FADE_TICK,id,ms,SMTO_BLOCK|SMTO_ABORTIFHUNG,2500,&timer) && timer);
    return (UINT_PTR)timer;
}
static bool fixture_workspace(void *ctx,const SpState *state,bool active,SpRect area){
    if(!active&&InterlockedCompareExchange(&fail_callback,0,0)){
        wcscpy(app.api.error,L"Injected unacknowledged removal; retry Restore all.");return false;
    }
    bool ok=workspace(ctx,state,active,area);
    if(ok && active && fail_after_show){
        /* Fail after the native show has actually reached a partial alpha,
           not at alpha 1 where an ordinary Hide could also be immediate. */
        failed_entry_timer=fixture_tick(0,FIXTURE_FADE_MS/2);
        BYTE alpha=0;
        CHECK(IsWindowVisible(fixture_surface) && GetLayeredWindowAttributes(fixture_surface,NULL,&alpha,NULL));
        CHECK(alpha>1 && alpha<sp_tint_alpha(NULL));
        wcscpy(app.api.error,L"Injected failure after native fade-in began.");return false;
    }
    return ok;
}
static void fixture_snapshot(void){
    CHECK(GetCurrentThreadId()==GetWindowThreadProcessId(fixture_surface,NULL));
    FixtureSurface snapshot={0};
    snapshot.native_visible=!!IsWindowVisible(fixture_surface);snapshot.surface_shown=app.overlay->shown;
    snapshot.underneath=underneath(app.overlay,fixture_background);
    snapshot.timer=app.overlay->fade.timer;
    CHECK(GetLayeredWindowAttributes(fixture_surface,NULL,&snapshot.alpha,NULL));
    KBDLLHOOKSTRUCT key={0};key.vkCode='A';key.scanCode=0x1e;
    snapshot.key_down=overlay_keyboard(HC_ACTION,WM_KEYDOWN,(LPARAM)&key);
    snapshot.key_up=overlay_keyboard(HC_ACTION,WM_KEYUP,(LPARAM)&key);
    MSLLHOOKSTRUCT mouse={0};mouse.pt=(POINT){fixture_area.x+5,fixture_area.y+5};
    snapshot.wheel=overlay_mouse(HC_ACTION,WM_MOUSEWHEEL,(LPARAM)&mouse);
    surface_observation=snapshot;
}
static LRESULT CALLBACK fixture_surface_proc(HWND h,UINT msg,WPARAM wp,LPARAM lp){
    if(msg==FIXTURE_INSPECT){fixture_snapshot();CHECK(SetEvent(surface_inspected));return 0;}
    if(msg==FIXTURE_FADE_TICK){
        UINT_PTR timer=wp?wp:app.overlay->fade.timer;CHECK(timer);
        InterlockedAdd64(&animation_clock,lp);
        (void)overlay_proc(h,WM_TIMER,timer,0);
        return (LRESULT)timer;
    }
    if(msg==FIXTURE_STALL){
        fixture_snapshot();fixture_wait_for_resume();return 0;
    }
    return overlay_proc(h,msg,wp,lp);
}
static bool fixture_create_surface(HWND host,UINT fade_ms){
    wchar_t error[256];app.overlay=sp_overlay_create_with_fade(host,app.api.tag,(BYTE)sp_tint_alpha(NULL),fade_ms,error,256);
    if(!app.overlay){fwprintf(stderr,L"%ls\n",error);return false;}
    HWND h=app.overlay->window;
    if(!SetWindowLongPtrW(h,GWLP_WNDPROC,(LONG_PTR)fixture_surface_proc))return false;
    fixture_surface=h;return true;
}
static DWORD WINAPI fixture_worker(void *resume){
    DWORD result=WaitForSingleObject(resume,INFINITE);
    CloseHandle(resume);return result;
}
static LRESULT CALLBACK fixture_host_proc(HWND h,UINT msg,WPARAM wp,LPARAM lp){
    if(msg==FIXTURE_RESET||msg==FIXTURE_PREPARE){
        CHECK(!app.scratch.visible&&!app.scratch.count);
        SpWindowOps ops={fixture_claim_member,fixture_alive_member,fixture_hide_member,fixture_tile_member,fixture_release_member,fixture_focus_member};
        CHECK(sp_init(&app.scratch,ops,NULL));app.scratch.workspace=fixture_workspace;app.scratch.workspace_ctx=&app;
        app.scratch.workspace_hide=app.overlay->fade_ms?hide_workspace:NULL;
        failed_entry_timer=0;
        memset(claimed,0,sizeof(claimed));memset(shown,0,sizeof(shown));
        for(uintptr_t i=1;i<=wp;++i)CHECK(sp_toggle_member(&app.scratch,i,fixture_area).status==SP_OK);
        fixture_result=msg==FIXTURE_RESET?sp_toggle(&app.scratch,fixture_area):(SpResult){SP_OK,0};
        hide_calls=tile_calls=release_calls=focus_calls=member_waits_with_shield=0;
        return (LRESULT)fixture_result.status+1;
    }
    if(msg==FIXTURE_TOGGLE){
        fail_after_show=!!wp;fixture_result=sp_toggle(&app.scratch,fixture_area);fail_after_show=false;
        return (LRESULT)fixture_result.status+1;
    }
    if(msg==FIXTURE_ENABLE_FADE){
        CHECK(!app.scratch.visible && !app.scratch.count && !app.scratch.hide_request);
        sp_overlay_destroy(app.overlay);CHECK(!IsWindow(fixture_surface) && !hook_count);
        return fixture_create_surface(h,FIXTURE_FADE_MS);
    }
    if(msg==FIXTURE_HOLD_WORKER){
        CHECK(wp && !app.worker);
        /* Give the worker its own handle: failure cleanup may close the
           fixture's event handle before the worker reaches its wait. */
        HANDLE process=GetCurrentProcess(),resume;
        if(!DuplicateHandle(process,(HANDLE)wp,process,&resume,0,FALSE,DUPLICATE_SAME_ACCESS))return 0;
        app.worker=CreateThread(NULL,0,fixture_worker,resume,0,NULL);
        if(!app.worker)CloseHandle(resume);
        return app.worker!=NULL;
    }
    if(msg==FIXTURE_MEMBERS){
        CHECK(GetCurrentThreadId()==host_thread_id);
        FixtureMembers snapshot={.hides=hide_calls,.tiles=tile_calls,.releases=release_calls,.focuses=focus_calls,
            .waits_with_shield=member_waits_with_shield,.closing=app.closing,.hide_watchdog=app.hide_deadline!=0,
            .hide_request=app.scratch.hide_request,.failed_entry_timer=failed_entry_timer,.result=fixture_result};
        memcpy(snapshot.claimed,claimed,sizeof(claimed));memcpy(snapshot.shown,shown,sizeof(shown));
        member_observation=snapshot;CHECK(SetEvent(members_inspected));return 0;
    }
    if(msg==FIXTURE_STOP){(void)sp_release_all(&app.scratch);DestroyWindow(h);return 0;}
    return host_proc(h,msg,wp,lp);
}
static DWORD WINAPI fixture_host_thread(void *ctx){
    (void)ctx;WNDCLASSW cls={0};cls.lpfnWndProc=fixture_host_proc;cls.hInstance=GetModuleHandleW(NULL);cls.lpszClassName=host_class;
    if(RegisterClassW(&cls))fixture_host=CreateWindowExW(WS_EX_TOOLWINDOW,host_class,L"Disposable deactivation host",WS_POPUP,0,0,0,0,NULL,NULL,cls.hInstance,NULL);
    app.window=fixture_host;app.control=RegisterWindowMessageW(SP_CONTROL_NAME);
    app.cancel=CreateEventW(NULL,TRUE,FALSE,NULL);wcscpy(app.api.tag,L"CloakSpace.DeactivationFixture");
    if(fixture_host && app.control && app.cancel)(void)fixture_create_surface(fixture_host,0);
    SetEvent(host_ready);MSG message;
    while(GetMessageW(&message,NULL,0,0)>0){TranslateMessage(&message);DispatchMessageW(&message);}
    if(app.cancel)CloseHandle(app.cancel);
    UnregisterClassW(host_class,cls.hInstance);return 0;
}
static void cleanup_fixture(void){
    InterlockedExchange(&fail_hide,0);InterlockedExchange(&fail_timer,0);InterlockedExchange(&fail_callback,0);
    if(stall_resume)SetEvent(stall_resume);
    if(worker_resume)SetEvent(worker_resume);
    if(host_thread){if(WaitForSingleObject(host_thread,0)==WAIT_TIMEOUT && fixture_host)PostMessageW(fixture_host,FIXTURE_STOP,0,0);
        (void)WaitForSingleObject(host_thread,10000);CloseHandle(host_thread);host_thread=NULL;}
    if(worker_resume){CloseHandle(worker_resume);worker_resume=NULL;}
    if(fixture_background){DestroyWindow(fixture_background);fixture_background=NULL;}
    if(host_ready)CloseHandle(host_ready);if(stall_entered)CloseHandle(stall_entered);if(stall_resume)CloseHandle(stall_resume);
    if(surface_inspected)CloseHandle(surface_inspected);if(members_inspected)CloseHandle(members_inspected);
    host_ready=stall_entered=stall_resume=surface_inspected=members_inspected=NULL;
}
static DWORD_PTR fixture_send(UINT message,WPARAM action){
    DWORD_PTR response=0;CHECK(SendMessageTimeoutW(fixture_host,message,action,0,SMTO_BLOCK|SMTO_ABORTIFHUNG,7000,&response));return response;
}
static DWORD_PTR fixture_status(void){return fixture_send(app.control,SP_CONTROL_STATUS);}
static FixtureMembers fixture_members(void){
    CHECK(ResetEvent(members_inspected));CHECK(PostMessageW(fixture_host,FIXTURE_MEMBERS,0,0));
    CHECK(WaitForSingleObject(members_inspected,5000)==WAIT_OBJECT_0);return member_observation;
}
static void native_state(bool active){
    if(!surface_stalled){
        /* Posted inspection cannot reenter an apply handler: returning from
           ShowWindow or becoming invisible alone is not a completion barrier. */
        CHECK(ResetEvent(surface_inspected));CHECK(PostMessageW(fixture_surface,FIXTURE_INSPECT,0,0));
        CHECK(WaitForSingleObject(surface_inspected,5000)==WAIT_OBJECT_0);
    }
    FixtureSurface snapshot=surface_observation;
    CHECK(snapshot.native_visible==active);CHECK(snapshot.surface_shown==active);
    CHECK(snapshot.underneath==active);
    CHECK(snapshot.key_down==(active?1:0));CHECK(snapshot.key_up==(active?1:0));CHECK(snapshot.wheel==(active?1:0));
    if(surface_stalled)CHECK(!!IsWindowVisible(fixture_surface)==active);
}
static void reset_case(size_t members){
    CHECK(fixture_send(FIXTURE_RESET,members)==SP_OK+1);native_state(true);
    FixtureMembers observed=fixture_members();
    CHECK(fixture_status()==0x100+members+1);CHECK(!observed.hides&&!observed.releases&&!observed.waits_with_shield);
}
static void stall_surface(void){
    CHECK(ResetEvent(stall_entered));CHECK(ResetEvent(stall_resume));
    CHECK(PostMessageW(fixture_surface,FIXTURE_STALL,0,0));CHECK(WaitForSingleObject(stall_entered,2000)==WAIT_OBJECT_0);
    surface_stalled=true;
}
static void resume_surface(void){
    CHECK(SetEvent(stall_resume));surface_stalled=false;
    /* All timeout cleanup posts precede this fresh inspection in the queue. */
    native_state(false);
}
static void client(unsigned action,int exit_code,const char *state,bool failed){
    const wchar_t *args[]={self,L"--fixture-client",host_class,action==SP_CONTROL_RESTORE?L"restore":action==SP_CONTROL_QUIT?L"quit":L"status"};
    SpProcessResult r;CHECK(sp_capture(self,args,4,12000,NULL,&r));CHECK(r.exit_code==(DWORD)exit_code);
    if(state)CHECK(strstr(r.out,state));
    if(action==SP_CONTROL_RESTORE)CHECK(!!strstr(r.err,"not confirmed")==failed);
    if(action==SP_CONTROL_QUIT)CHECK(strstr(r.err,"refused or not confirmed"));
}
static void failed_entry_rollback(size_t members){
    CHECK(fixture_send(FIXTURE_PREPARE,members)==SP_OK+1);native_state(false);
    CHECK(fixture_status()==members+1);
    InterlockedExchange(&fail_timer,1);InterlockedExchange(&fail_hide,1);
    CHECK(fixture_send(FIXTURE_TOGGLE,0)==SP_WINDOW_ERROR+1);native_state(true);
    FixtureMembers observed=fixture_members();
    CHECK(!observed.hides&&!observed.tiles&&!observed.releases&&!observed.focuses&&!observed.waits_with_shield);
    for(size_t i=1;i<=members;++i)CHECK(observed.claimed[i]&&!observed.shown[i]);
    CHECK(fixture_status()==0x100+members+1);
    CHECK(observed.result.status==SP_WINDOW_ERROR&&observed.result.failed==2);
    InterlockedExchange(&fail_timer,0);InterlockedExchange(&fail_hide,0);
    CHECK(fixture_send(FIXTURE_TOGGLE,0)==SP_OK+1);native_state(false);
    CHECK(fixture_status()==members+1);observed=fixture_members();
    CHECK(observed.hides==members&&!observed.tiles&&!observed.releases&&!observed.focuses&&!observed.waits_with_shield);
    CHECK(fixture_send(app.control,SP_CONTROL_RESTORE)==1);native_state(false);
    observed=fixture_members();CHECK(fixture_status()==1&&observed.releases==members&&!observed.waits_with_shield);
}
static void timeout_hide(void){
    reset_case(2);stall_surface();
    CHECK(fixture_send(FIXTURE_TOGGLE,0)==SP_WINDOW_ERROR+1);native_state(true);
    FixtureMembers observed=fixture_members();
    CHECK(fixture_status()==0x103&&!observed.hides&&!observed.releases&&observed.shown[1]&&observed.shown[2]);
    CHECK(wcsstr(sp_overlay_error(app.overlay),L"deactivation not acknowledged"));
    resume_surface();CHECK(fixture_status()==0x103); /* Conservative until the retry is acknowledged. */
    CHECK(fixture_send(FIXTURE_TOGGLE,0)==SP_OK+1);native_state(false);
    observed=fixture_members();CHECK(fixture_status()==3&&observed.hides==2&&!observed.waits_with_shield);
    CHECK(fixture_send(app.control,SP_CONTROL_RESTORE)==1);observed=fixture_members();CHECK(observed.releases==2);
}
static void timeout_restore(size_t members){
    reset_case(members);stall_surface();
    CHECK(fixture_send(app.control,SP_CONTROL_RESTORE)==(SP_CONTROL_FAILED|0x100|(members+1)));
    native_state(true);CHECK(fixture_status()==0x100+members+1);
    client(SP_CONTROL_RESTORE,1,"visible=1",true);client(SP_CONTROL_STATUS,0,"visible=1",false);
    FixtureMembers observed=fixture_members();CHECK(!observed.releases&&!observed.hides&&!observed.waits_with_shield);
    if(members)CHECK(observed.claimed[1]&&observed.claimed[2]&&observed.shown[1]&&observed.shown[2]);
    resume_surface();CHECK(fixture_status()==0x100+members+1);
    client(SP_CONTROL_RESTORE,0,"members=0 visible=0",false);
    observed=fixture_members();CHECK(fixture_status()==1&&observed.releases==members&&!observed.waits_with_shield);native_state(false);
}
static void refused_removal(size_t members){
    reset_case(members);InterlockedExchange(&fail_hide,1);
    CHECK(fixture_send(FIXTURE_TOGGLE,0)==SP_WINDOW_ERROR+1);native_state(true);
    FixtureMembers observed=fixture_members();CHECK(fixture_status()==0x100+members+1&&!observed.hides&&!observed.releases);
    CHECK(fixture_send(app.control,SP_CONTROL_RESTORE)==(SP_CONTROL_FAILED|0x100|(members+1)));
    client(SP_CONTROL_RESTORE,1,"visible=1",true);native_state(true);
    observed=fixture_members();CHECK(!observed.hides&&!observed.releases&&!observed.waits_with_shield);
    InterlockedExchange(&fail_hide,0);client(SP_CONTROL_RESTORE,0,"members=0 visible=0",false);
    native_state(false);observed=fixture_members();CHECK(observed.releases==members&&!observed.waits_with_shield);
}
static void delayed_removal(size_t members,bool restore){
    reset_case(members);InterlockedExchange(&delay_hide,80);
    CHECK(fixture_send(restore?app.control:FIXTURE_TOGGLE,restore?SP_CONTROL_RESTORE:0)==1);
    native_state(false);FixtureMembers observed=fixture_members();CHECK(!observed.waits_with_shield);
    CHECK(restore?observed.releases==members:observed.hides==members);
    CHECK(fixture_send(app.control,SP_CONTROL_RESTORE)==1);
}
static void failed_empty_inactive_restore(void){
    CHECK(fixture_status()==1);InterlockedExchange(&fail_callback,1);
    CHECK(fixture_send(app.control,SP_CONTROL_RESTORE)==(SP_CONTROL_FAILED|1));
    client(SP_CONTROL_RESTORE,1,"members=0 visible=0",true);native_state(false);
    InterlockedExchange(&fail_callback,0);client(SP_CONTROL_RESTORE,0,"members=0 visible=0",false);
}
static void refused_empty_exit(void){
    reset_case(0);InterlockedExchange(&fail_hide,1);
    CHECK(fixture_send(app.control,SP_CONTROL_QUIT)==0);CHECK(fixture_status()==0x101);
    client(SP_CONTROL_QUIT,1,NULL,true);native_state(true);FixtureMembers observed=fixture_members();CHECK(!observed.closing);
    InterlockedExchange(&fail_hide,0);client(SP_CONTROL_RESTORE,0,"members=0 visible=0",false);native_state(false);
}

typedef struct {UINT_PTR timer;uintptr_t request;} FixtureHide;

static void check_fade_members(FixtureMembers observed,bool owned,bool visible){
    CHECK(!observed.waits_with_shield);
    for(size_t i=1;i<=2;++i)CHECK(observed.claimed[i]==owned && observed.shown[i]==visible);
}
static FixtureHide pending_fade_hide(void){
    native_state(true);FixtureMembers observed=fixture_members();
    CHECK(fixture_status()==0x103 && observed.hide_request && observed.hide_watchdog);
    CHECK(surface_observation.timer && !observed.hides && !observed.releases);
    check_fade_members(observed,true,true);
    return (FixtureHide){surface_observation.timer,observed.hide_request};
}
static FixtureHide hide_during_fade_in(void){
    reset_case(2);UINT_PTR entering=surface_observation.timer;
    CHECK(entering && surface_observation.alpha==1);
    fixture_tick(entering,FIXTURE_FADE_MS/2);native_state(true);
    CHECK(surface_observation.alpha>1 && surface_observation.alpha<sp_tint_alpha(NULL));
    CHECK(fixture_send(FIXTURE_TOGGLE,0)==SP_OK+1);
    FixtureHide hiding=pending_fade_hide();CHECK(hiding.timer!=entering);
    return hiding;
}
static FixtureMembers await_hide_result(void){
    ULONGLONG start=GetTickCount64();FixtureMembers observed;
    do {
        observed=fixture_members();
        if(!observed.hide_request)break;
        Sleep(5);
    } while(GetTickCount64()-start<2500);
    CHECK(!observed.hide_request && !observed.hide_watchdog);
    return observed;
}
static void replay_cancelled_hide(FixtureHide stale){
    fixture_tick(stale.timer,FIXTURE_FADE_MS);
    CHECK(fixture_send(SP_OVERLAY_HIDE_COMPLETE,stale.request)==0);
    CHECK(fixture_send(WM_TIMER,SP_HOST_HIDE_TIMER)==0);
}
static DWORD WINAPI fixture_recovery_sender(void *action){
    DWORD_PTR response=0;
    return SendMessageTimeoutW(fixture_host,app.control,(WPARAM)action,0,SMTO_BLOCK|SMTO_ABORTIFHUNG,4500,&response)?
        (DWORD)response:MAXDWORD;
}
static void confirm_recovery_reply(unsigned action){
    /* A snapshot posted after the reply could run behind a queued hide. Hold
       the actual native hide, and don't advance the animation clock either. */
    CHECK(ResetEvent(stall_entered) && ResetEvent(stall_resume));
    InterlockedExchange(&hold_next_hide,1);
    HANDLE sender=CreateThread(NULL,0,fixture_recovery_sender,(void *)(UINT_PTR)action,0,NULL);CHECK(sender);
    HANDLE gates[]={sender,stall_entered};
    bool entered=WaitForMultipleObjects(2,gates,FALSE,2500)==WAIT_OBJECT_0+1;
    bool visible=entered && IsWindowVisible(fixture_surface);
    bool held_until_hide=entered && WaitForSingleObject(sender,100)==WAIT_TIMEOUT;
    CHECK(SetEvent(stall_resume));
    DWORD response=0;
    CHECK(WaitForSingleObject(sender,2500)==WAIT_OBJECT_0 && GetExitCodeThread(sender,&response));
    CHECK(CloseHandle(sender));
    CHECK(held_until_hide && visible && response==1);
}
static void faded_hide_refusal_and_retry(void){
    FixtureHide hiding=hide_during_fade_in();
    /* The message is only a wake-up, even if it carries the current token. */
    CHECK(fixture_send(SP_OVERLAY_HIDE_COMPLETE,hiding.request)==0);
    CHECK(pending_fade_hide().request==hiding.request);
    InterlockedExchange(&fail_hide,1);
    fixture_tick(hiding.timer,FIXTURE_FADE_MS);native_state(true);
    FixtureMembers observed=await_hide_result();
    CHECK(!surface_observation.timer && surface_observation.alpha==sp_tint_alpha(NULL));
    CHECK(fixture_status()==0x103 && !observed.hides && !observed.releases);
    check_fade_members(observed,true,true);

    InterlockedExchange(&fail_hide,0);
    CHECK(fixture_send(FIXTURE_TOGGLE,0)==SP_OK+1);
    FixtureHide retry=pending_fade_hide();CHECK(retry.request!=hiding.request && retry.timer!=hiding.timer);
    InterlockedExchange(&drop_next_completion,1);
    fixture_tick(retry.timer,FIXTURE_FADE_MS);native_state(false);
    CHECK(!InterlockedCompareExchange(&drop_next_completion,0,0));
    observed=await_hide_result(); /* Real host watchdog, no completion post. */
    CHECK(fixture_status()==3 && observed.hides==2 && !observed.releases);
    check_fade_members(observed,true,false);
    CHECK(fixture_send(app.control,SP_CONTROL_RESTORE)==1);
}
static void reverse_pending_hide(void){
    FixtureHide hiding=hide_during_fade_in();
    CHECK(fixture_send(FIXTURE_TOGGLE,0)==SP_OK+1);native_state(true);
    CHECK(surface_observation.timer && surface_observation.timer!=hiding.timer);
    replay_cancelled_hide(hiding);native_state(true);
    FixtureMembers observed=fixture_members();
    CHECK(fixture_status()==0x103 && !observed.hide_request && !observed.hide_watchdog);
    CHECK(!observed.hides && !observed.releases);check_fade_members(observed,true,true);
    CHECK(fixture_send(app.control,SP_CONTROL_RESTORE)==1);
}
static void restore_during_fade(void){
    FixtureHide hiding=hide_during_fade_in();
    confirm_recovery_reply(SP_CONTROL_RESTORE);
    native_state(false);CHECK(!surface_observation.timer);
    replay_cancelled_hide(hiding);native_state(false);CHECK(fixture_status()==1);
    FixtureMembers observed=fixture_members();
    CHECK(!observed.hide_request && !observed.hide_watchdog && !observed.hides && observed.releases==2);
    check_fade_members(observed,false,true);
}
static void rollback_during_fade(void){
    CHECK(fixture_send(FIXTURE_PREPARE,2)==SP_OK+1);native_state(false);
    CHECK(fixture_send(FIXTURE_TOGGLE,1)==SP_WINDOW_ERROR+1);
    native_state(false);CHECK(!surface_observation.timer && fixture_status()==3);
    FixtureMembers observed=fixture_members();
    CHECK(observed.failed_entry_timer && observed.result.failed==1 && !observed.hide_request && !observed.hide_watchdog);
    CHECK(!observed.hides && !observed.tiles && !observed.releases && !observed.focuses);
    check_fade_members(observed,true,false);
    fixture_tick(observed.failed_entry_timer,FIXTURE_FADE_MS);native_state(false);
    CHECK(fixture_send(app.control,SP_CONTROL_RESTORE)==1);
    observed=fixture_members();CHECK(observed.releases==2 && !observed.hides);check_fade_members(observed,false,true);
}
static void exit_during_fade(void){
    FixtureHide hiding=hide_during_fade_in();
    /* Delay teardown independently so we can inspect the live but hidden
       HWND and replay obsolete messages while production exit is pending. */
    worker_resume=CreateEventW(NULL,TRUE,FALSE,NULL);CHECK(worker_resume);
    CHECK(fixture_send(FIXTURE_HOLD_WORKER,(WPARAM)worker_resume)==1);
    confirm_recovery_reply(SP_CONTROL_QUIT);
    native_state(false);CHECK(!surface_observation.timer);
    replay_cancelled_hide(hiding);native_state(false);CHECK(fixture_status()==0x201);
    FixtureMembers observed=fixture_members();
    CHECK(observed.closing && !observed.hide_request && !observed.hide_watchdog && !observed.hides && observed.releases==2);
    check_fade_members(observed,false,true);
    CHECK(SetEvent(worker_resume));CHECK(WaitForSingleObject(host_thread,5000)==WAIT_OBJECT_0);
    CHECK(!IsWindow(fixture_host) && !IsWindow(fixture_surface));
}
int wmain(int argc,wchar_t **argv){
    if(argc==4&&!wcscmp(argv[1],L"--fixture-client"))return control(argv[2],!wcscmp(argv[3],L"restore")?SP_CONTROL_RESTORE:!wcscmp(argv[3],L"quit")?SP_CONTROL_QUIT:SP_CONTROL_STATUS);
    CHECK(argc==1);atexit(cleanup_fixture);sp_win32_dpi();
    CHECK(GetModuleFileNameW(NULL,self,32768));
    swprintf(host_class,128,L"%lsdeactivation-%lu",SP_HOST_PREFIX,(unsigned long)GetCurrentProcessId());
    SpWin32 area_api={0};SpRect monitor;CHECK(sp_win32_workarea(&area_api,&monitor));
    fixture_area=(SpRect){monitor.x+30,monitor.y+30,400,300};
    WNDCLASSW cls={0};cls.lpfnWndProc=DefWindowProcW;cls.hInstance=GetModuleHandleW(NULL);cls.lpszClassName=L"CloakSpace.DeactivationBackground";
    cls.hbrBackground=GetStockObject(WHITE_BRUSH);CHECK(RegisterClassW(&cls));
    fixture_background=CreateWindowExW(WS_EX_TOOLWINDOW|WS_EX_TOPMOST,cls.lpszClassName,L"Disposable deactivation background",WS_POPUP|WS_VISIBLE,
        fixture_area.x,fixture_area.y,fixture_area.w,fixture_area.h,NULL,NULL,cls.hInstance,NULL);CHECK(fixture_background);
    host_ready=CreateEventW(NULL,TRUE,FALSE,NULL);stall_entered=CreateEventW(NULL,TRUE,FALSE,NULL);stall_resume=CreateEventW(NULL,TRUE,FALSE,NULL);
    surface_inspected=CreateEventW(NULL,TRUE,FALSE,NULL);members_inspected=CreateEventW(NULL,TRUE,FALSE,NULL);
    CHECK(host_ready&&stall_entered&&stall_resume&&surface_inspected&&members_inspected);
    host_thread=CreateThread(NULL,0,fixture_host_thread,NULL,0,&host_thread_id);CHECK(host_thread);
    CHECK(WaitForSingleObject(host_ready,5000)==WAIT_OBJECT_0&&fixture_host&&fixture_surface);
    failed_entry_rollback(2);failed_entry_rollback(0);
    timeout_hide();timeout_restore(0);timeout_restore(2);
    refused_removal(0);refused_removal(2);
    for(size_t members=0;members<=2;members+=2){delayed_removal(members,false);delayed_removal(members,true);}
    failed_empty_inactive_restore();refused_empty_exit();
    CHECK(fixture_send(FIXTURE_ENABLE_FADE,0)==1);
    faded_hide_refusal_and_retry();reverse_pending_hide();restore_during_fade();rollback_during_fade();exit_during_fade();
    CHECK(IsWindowEnabled(fixture_background));cleanup_fixture();CHECK(hook_count==0);
    CHECK(!IsWindow(fixture_surface));CHECK(UnregisterClassW(cls.lpszClassName,cls.hInstance));
    printf("deactivation: %ld checks passed (native surface, fade cancellation, immediate recovery, timeout/refusal/retry; fixture hooks/members)\n",(long)checks);return 0;
}
