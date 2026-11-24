#include "host.h"
#include <dwmapi.h>
#include <stdio.h>
#include <stdlib.h>

/* Replay WinEvents to the real host procedure. Window families, styles, claim
   cookies and the posted message queue are native; no desktop focus or input is changed. */
static unsigned checks, registrations, unhooks, foreground_reads;
#define CHECK(x) do { ++checks; if (!(x)) { \
    fprintf(stderr,"%s:%d: %s\n",__FILE__,__LINE__,#x); exit(1); } } while (0)
static struct {
    DWORD first,last;
    WINEVENTPROC callback;
} hooks[]={
    {EVENT_SYSTEM_FOREGROUND,EVENT_SYSTEM_FOREGROUND,NULL},
    {EVENT_SYSTEM_MINIMIZESTART,EVENT_SYSTEM_MINIMIZEEND,NULL},
    {EVENT_OBJECT_DESTROY,EVENT_OBJECT_HIDE,NULL},
    {EVENT_OBJECT_LOCATIONCHANGE,EVENT_OBJECT_LOCATIONCHANGE,NULL},
    {EVENT_OBJECT_CLOAKED,EVENT_OBJECT_UNCLOAKED,NULL}
};
#define HOOK_COUNT (sizeof(hooks)/sizeof(hooks[0]))
#define FIXTURE_HOOK(i) ((HWINEVENTHOOK)(UINT_PTR)((i)+1))
static HWND actual_foreground, requested_focus;
static bool refuse_hooks;

static size_t event_slot(DWORD event) {
    for (size_t i=0;i<HOOK_COUNT;++i) if (event>=hooks[i].first && event<=hooks[i].last) return i;
    return HOOK_COUNT;
}
static HWINEVENTHOOK WINAPI fixture_hook(DWORD first,DWORD last,HMODULE dll,WINEVENTPROC proc,
    DWORD process,DWORD thread,DWORD flags) {
    size_t i=event_slot(first);
    CHECK(i<HOOK_COUNT && first==hooks[i].first && last==hooks[i].last && !hooks[i].callback);
    CHECK(!dll && proc && !process && !thread && flags==WINEVENT_OUTOFCONTEXT);
    ++registrations;
    if (refuse_hooks) return NULL;
    hooks[i].callback=proc;return FIXTURE_HOOK(i);
}
static BOOL WINAPI fixture_unhook(HWINEVENTHOOK hook) {
    size_t i=(size_t)(UINT_PTR)hook-1;
    CHECK(i<HOOK_COUNT && hooks[i].callback);++unhooks;hooks[i].callback=NULL;return TRUE;
}
static HWND WINAPI fixture_foreground(void) {++foreground_reads;return actual_foreground;}
static bool fixture_workarea(SpWin32 *api,SpState *state,SpRect *work) {
    (void)api;(void)state;*work=(SpRect){60,60,800,500};return true;
}
#define SetWinEventHook fixture_hook
#define UnhookWinEvent fixture_unhook
#define GetForegroundWindow fixture_foreground
#define sp_win32_scratch_workarea fixture_workarea
#include "../src/main.c"
#undef sp_win32_scratch_workarea
#undef GetForegroundWindow
#undef UnhookWinEvent
#undef SetWinEventHook

static HWND make_window(const WNDCLASSW *cls,DWORD style,HWND parent) {
    HWND h=CreateWindowExW(WS_EX_TOOLWINDOW|WS_EX_NOACTIVATE,cls->lpszClassName,L"Disposable foreground fixture",
        style|WS_VISIBLE,100,100,200,150,parent,NULL,cls->hInstance,NULL);
    CHECK(h);return h;
}
static bool fixture_focus(void *ctx,SpWindow w) {
    (void)ctx;requested_focus=(HWND)w.handle;return true;
}
static bool fixture_tile(void *ctx,SpWindow w,SpRect rect) {
    /* Separately claimed popup and owner needn't satisfy two concurrent tile
       Z-order constraints here. Exercise native visibility without moving them. */
    (void)ctx;(void)rect;
    if (!IsWindow((HWND)w.handle)) return false;
    ShowWindow((HWND)w.handle,SW_SHOWNOACTIVATE);
    return IsWindowVisible((HWND)w.handle)!=0;
}
static void track_member(HWND h) {
    SpState *s=&app.scratch;
    CHECK(s->count<SP_CAPACITY && s->ops.claim(s->ctx,(uintptr_t)h,&s->windows[s->count]));
    ++s->count;
}
static void signal_event(DWORD event,HWND h) {
    size_t i=event_slot(event);
    CHECK(i<HOOK_COUNT && hooks[i].callback);
    hooks[i].callback(FIXTURE_HOOK(i),event,h,OBJID_WINDOW,CHILDID_SELF,0,0);
}
static void signal_foreground(HWND h) {signal_event(EVENT_SYSTEM_FOREGROUND,h);}
static bool dispatch_foreground(void) {
    MSG msg;
    if (!PeekMessageW(&msg,app.window,SP_FOREGROUND_MESSAGE,SP_FOREGROUND_MESSAGE,PM_REMOVE)) return false;
    DispatchMessageW(&msg);return true;
}
static void expect_refresh(HWND expected,unsigned reads) {
    unsigned before=foreground_reads;
    CHECK(dispatch_foreground() && app.focused_member==expected);
    CHECK(foreground_reads==before+reads && !dispatch_foreground());
}
static void focus_is(HWND observed,HWND expected) {
    unsigned reads=foreground_reads;
    HWND previous=app.focused_member;
    actual_foreground=observed;signal_foreground(observed);
    CHECK(foreground_reads==reads && app.focused_member==previous);
    expect_refresh(expected,1);
}
static void check_alpha(HWND h,BYTE alpha) {
    bool layered=(GetWindowLongPtrW(h,GWL_EXSTYLE)&WS_EX_LAYERED)!=0;
    if (!alpha) {CHECK(!layered);return;}
    BYTE observed=0;DWORD flags=0;
    CHECK(layered && GetLayeredWindowAttributes(h,NULL,&observed,&flags) && flags==LWA_ALPHA && observed==alpha);
}
static void check_keyed(HWND h,COLORREF key,BYTE alpha) {
    BYTE observed=0;COLORREF observed_key=0;DWORD flags=0;
    CHECK(GetLayeredWindowAttributes(h,&observed_key,&observed,&flags) &&
        observed_key==key && flags==(LWA_ALPHA|LWA_COLORKEY) && observed==alpha);
}
static void bind(size_t index,HWND h) {SendMessageW(app.window,SP_BIND_MESSAGE,index,(LPARAM)h);}
static void tray_toggle(void) {SendMessageW(app.window,SP_TRAY_MESSAGE,0,WM_LBUTTONDBLCLK);}
static void restore_all(void) {
    CHECK(SendMessageW(app.window,app.control,SP_CONTROL_RESTORE,0)==1);
    CHECK(!app.scratch.count && !app.scratch.visible && !app.focused_member);
}

/* Border attribute replies are simulated; routing, queues, claims, window
   visibility and parent/owner chains are real. Reject any access to a dialog. */
static HWND border_windows[2];
static COLORREF border_colors[2];
static unsigned border_reads,border_writes;
static bool unreadable_hidden_border;
static size_t border_index(HWND h,DWORD attr,DWORD size) {
    CHECK(h && attr==DWMWA_BORDER_COLOR && size==sizeof(COLORREF));
    CHECK(h==border_windows[0] || h==border_windows[1]);return h==border_windows[0]?0u:1u;
}
static HRESULT WINAPI fixture_border_get(HWND h,DWORD attr,PVOID out,DWORD size) {
    size_t i=border_index(h,attr,size);++border_reads;
    if (unreadable_hidden_border && !IsWindowVisible(h)) return E_FAIL;
    *(COLORREF *)out=border_colors[i];return S_OK;
}
static HRESULT WINAPI fixture_border_set(HWND h,DWORD attr,LPCVOID in,DWORD size) {
    CHECK(GetPropW(h,app.api.tag));
    border_colors[border_index(h,attr,size)]=*(const COLORREF *)in;++border_writes;return S_OK;
}
static void test_border_routing(SpProfile *profile,HWND a,HWND b,HWND grandchild,HWND dialog_child) {
    const COLORREF original_a=RGB(94,82,17),original_b=DWMWA_COLOR_DEFAULT;
    border_windows[0]=a;border_windows[1]=b;border_colors[0]=original_a;border_colors[1]=original_b;
    app.api.get_border_attribute=fixture_border_get;app.api.set_border_attribute=fixture_border_set;

    CHECK(sp_appearance_set(&profile->appearance,SP_APPEARANCE_ACTIVE_BORDER,L"000000"));
    start_member_appearance(&app);
    CHECK(app.active_border.set && app.active_border.value==0 && !app.focused_alpha && !app.inactive_alpha);
    CHECK(registrations==HOOK_COUNT && hooks[0].callback);
    bind(1,a);bind(1,b);actual_foreground=dialog_child;bind(0,NULL);
    CHECK(app.scratch.visible && requested_focus==b && app.appearance_queued && !border_writes);
    expect_refresh(a,1);CHECK(border_colors[0]==RGB(0,0,0) && border_colors[1]==original_b);
    check_alpha(a,0);check_alpha(b,0);

    unsigned writes=border_writes;
    signal_foreground(a);actual_foreground=b;signal_foreground(a);
    CHECK(border_writes==writes && app.focused_member==a);
    expect_refresh(b,1);CHECK(border_colors[0]==original_a && border_colors[1]==RGB(0,0,0));

    /* Hide can make the attribute temporarily unreadable. A member that
       shows itself again must lose our override without losing its claim. */
    unreadable_hidden_border=true;bind(0,NULL);
    CHECK(!app.scratch.visible && !IsWindowVisible(b));
    expect_refresh(NULL,0);CHECK(border_colors[1]==RGB(0,0,0));
    ShowWindow(b,SW_SHOWNOACTIVATE);signal_event(EVENT_OBJECT_SHOW,b);
    expect_refresh(NULL,0);CHECK(border_colors[1]==original_b && GetPropW(b,app.api.tag));
    unreadable_hidden_border=false;
    actual_foreground=grandchild;tray_toggle();expect_refresh(a,1);CHECK(border_colors[0]==RGB(0,0,0));
    focus_is(NULL,NULL);CHECK(border_colors[0]==original_a);
    focus_is(b,b);CHECK(border_colors[1]==RGB(0,0,0));
    bind(1,b);CHECK(border_colors[1]==original_b && !GetPropW(b,app.api.tag));
    expect_refresh(NULL,1);
    restore_all();stop_member_appearance(&app);
    CHECK(unhooks==HOOK_COUNT);

    /* Equal nonzero alphas do not suppress foreground resolution for borders. */
    CHECK(sp_appearance_set(&profile->appearance,SP_APPEARANCE_MEMBER_OPACITY,L"0.85"));
    CHECK(sp_appearance_set(&profile->appearance,SP_APPEARANCE_ACTIVE_BORDER,L"2a7Bd4"));
    start_member_appearance(&app);
    CHECK(app.focused_alpha==app.inactive_alpha && app.focused_alpha==217);
    CHECK(registrations==2*HOOK_COUNT && hooks[0].callback);
    bind(1,a);bind(1,b);actual_foreground=b;bind(0,NULL);expect_refresh(b,1);
    CHECK(border_colors[0]==original_a && border_colors[1]==RGB(0x2a,0x7b,0xd4));
    focus_is(dialog_child,a);
    CHECK(border_colors[0]==RGB(0x2a,0x7b,0xd4) && border_colors[1]==original_b);
    check_alpha(a,217);check_alpha(b,217);
    restore_all();stop_member_appearance(&app);
    CHECK(border_colors[0]==original_a && border_colors[1]==original_b && unhooks==2*HOOK_COUNT);
    check_alpha(a,0);check_alpha(b,0);
    profile->appearance=(SpAppearance){0};registrations=unhooks=foreground_reads=0;
}
int main(void) {
    sp_win32_dpi();
    WNDCLASSW host_class={.lpfnWndProc=host_proc,.hInstance=GetModuleHandleW(NULL),.lpszClassName=L"CloakSpace.Foreground.Host"};
    WNDCLASSW window_class={.lpfnWndProc=DefWindowProcW,.hInstance=host_class.hInstance,.lpszClassName=L"CloakSpace.Foreground.Member"};
    CHECK(RegisterClassW(&host_class) && RegisterClassW(&window_class));
    app.window=CreateWindowExW(0,host_class.lpszClassName,L"",0,0,0,0,0,HWND_MESSAGE,NULL,host_class.hInstance,NULL);
    app.control=RegisterWindowMessageW(SP_CONTROL_NAME);
    app.cancel=CreateEventW(NULL,TRUE,FALSE,NULL);
    CHECK(app.window && app.control && app.cancel && sp_win32_init(&app.api,true));
    SpWindowOps ops=sp_win32_ops();ops.focus=fixture_focus;ops.tile=fixture_tile;
    CHECK(sp_init(&app.scratch,ops,&app.api));
    static SpProfile profile;
    profile.bindings[0].action=L"scratch.toggle";
    profile.bindings[1].action=L"scratch.member";
    profile.binding_count=2;app.profile=&profile;

    HWND a=make_window(&window_class,WS_POPUP,NULL), b=make_window(&window_class,WS_POPUP,NULL);
    HWND dialog=make_window(&window_class,WS_POPUP,a), nested=make_window(&window_class,WS_POPUP,dialog);
    HWND child=make_window(&window_class,WS_CHILD,a), grandchild=make_window(&window_class,WS_CHILD,child);
    HWND dialog_child=make_window(&window_class,WS_CHILD,nested), other=make_window(&window_class,WS_POPUP,NULL);
    test_border_routing(&profile,a,b,grandchild,dialog_child);
    unsigned reads_before=border_reads,writes_before=border_writes;
    track_member(a);track_member(b);
    SpWindow transient;
    CHECK(ops.claim(&app.api,(uintptr_t)other,&transient));

    /* No default observers, foreground sampling or style modifications. */
    start_member_appearance(&app);CHECK(!registrations);
    bind(0,NULL);CHECK(app.scratch.visible && foreground_reads==0 && !dispatch_foreground());
    check_alpha(a,0);check_alpha(b,0);
    bind(0,NULL);CHECK(!app.scratch.visible);

    CHECK(sp_appearance_set(&profile.appearance,SP_APPEARANCE_MEMBER_OPACITY,L"0.85"));
    start_member_appearance(&app);CHECK(!hooks[0].callback && registrations==HOOK_COUNT-1);
    tray_toggle();CHECK(app.scratch.visible && app.appearance_queued);
    check_alpha(a,0);check_alpha(b,0); /* A host command only queued work. */
    expect_refresh(NULL,0);check_alpha(a,217);check_alpha(b,217);check_alpha(other,0);
    restore_all();check_alpha(a,0);check_alpha(b,0);
    stop_member_appearance(&app);CHECK(unhooks==HOOK_COUNT-1);

    CHECK(sp_appearance_set(&profile.appearance,SP_APPEARANCE_INACTIVE_OPACITY,L"0.4"));
    start_member_appearance(&app);CHECK(hooks[0].callback && registrations==2*HOOK_COUNT-1);
    bind(1,a);bind(1,b);CHECK(!app.scratch.visible && !IsWindowVisible(a) && !IsWindowVisible(b));
    expect_refresh(NULL,0);
    signal_foreground(a);CHECK(!dispatch_foreground() && !foreground_reads);
    actual_foreground=a;bind(0,NULL);
    CHECK(app.scratch.visible && requested_focus==b && IsWindowVisible(a) && IsWindowVisible(b));
    expect_refresh(a,1);check_alpha(a,217);check_alpha(b,102);

    hooks[0].callback(FIXTURE_HOOK(0),EVENT_OBJECT_FOCUS,a,OBJID_WINDOW,CHILDID_SELF,0,0);
    hooks[0].callback(FIXTURE_HOOK(0),EVENT_SYSTEM_FOREGROUND,a,OBJID_CLIENT,CHILDID_SELF,0,0);
    hooks[0].callback(FIXTURE_HOOK(0),EVENT_SYSTEM_FOREGROUND,a,OBJID_WINDOW,1,0,0);
    signal_event(EVENT_OBJECT_SHOW,other); /* A transient cookie isn't membership. */
    signal_event(EVENT_OBJECT_SHOW,dialog);
    CHECK(!dispatch_foreground() && foreground_reads==1);

    signal_foreground(a);
    actual_foreground=b;signal_foreground(a); /* Both notification HWNDs are obsolete. */
    CHECK(foreground_reads==1 && app.focused_member==a);
    check_alpha(a,217);check_alpha(b,102); /* Hook never styles a window. */
    expect_refresh(b,1);check_alpha(a,102);check_alpha(b,217);
    focus_is(a,a);focus_is(grandchild,a);focus_is(dialog,a);focus_is(dialog_child,a);
    check_alpha(a,217);check_alpha(b,102);check_alpha(dialog,0);check_alpha(nested,0);check_alpha(grandchild,0);
    focus_is(other,NULL);check_alpha(a,102);check_alpha(b,102);check_alpha(other,0);
    focus_is(NULL,NULL);

    track_member(dialog);
    focus_is(dialog,dialog);focus_is(dialog_child,dialog); /* Take the nearest valid member. */
    check_alpha(dialog,217);check_alpha(a,102);check_alpha(nested,0);
    SpWindow identity=app.scratch.windows[2];
    ++app.scratch.windows[2].cookie;focus_is(dialog_child,a);check_alpha(dialog,217);
    app.scratch.windows[2]=identity;
    ++app.scratch.windows[2].process_id;focus_is(dialog_child,a);
    app.scratch.windows[2]=identity;
    ++app.scratch.windows[2].thread_id;focus_is(dialog_child,a);
    app.scratch.windows[2]=identity;
    /* Visibility must invalidate without a foreground notification. */
    focus_is(b,b);check_alpha(b,217);
    ShowWindow(b,SW_HIDE);CHECK(!IsWindowVisible(b));signal_event(EVENT_OBJECT_HIDE,b);
    expect_refresh(NULL,1);check_alpha(b,217); /* Hidden members aren't restyled. */
    ShowWindow(b,SW_SHOWMINNOACTIVE);CHECK(IsIconic(b));signal_event(EVENT_SYSTEM_MINIMIZESTART,b);
    expect_refresh(NULL,1);check_alpha(b,217);
    actual_foreground=a;
    signal_event(EVENT_SYSTEM_MINIMIZEEND,b); /* Dispatch can beat the actual restore. */
    expect_refresh(a,1);check_alpha(b,217);
    ShowWindow(b,SW_SHOWNOACTIVATE);CHECK(!IsIconic(b));signal_event(EVENT_OBJECT_LOCATIONCHANGE,b);
    expect_refresh(a,1);check_alpha(b,102);
    ShowWindow(b,SW_HIDE);signal_event(EVENT_OBJECT_HIDE,b);expect_refresh(a,1);
    actual_foreground=b;
    ShowWindow(b,SW_SHOWNOACTIVATE);signal_event(EVENT_OBJECT_SHOW,b);
    expect_refresh(b,1);check_alpha(b,217);check_alpha(a,102);

    BOOL cloak=TRUE;
    if (SUCCEEDED(DwmSetWindowAttribute(b,DWMWA_CLOAK,&cloak,sizeof(cloak)))) {
        signal_event(EVENT_OBJECT_CLOAKED,b);expect_refresh(NULL,1);check_alpha(b,217);
        actual_foreground=a;cloak=FALSE;
        CHECK(SUCCEEDED(DwmSetWindowAttribute(b,DWMWA_CLOAK,&cloak,sizeof(cloak))));
        signal_event(EVENT_OBJECT_UNCLOAKED,b);expect_refresh(a,1);check_alpha(b,102);
    }
    CHECK(DestroyWindow(other));focus_is(other,NULL);

    /* A queued event cannot restyle hidden windows. Show samples the current
       dialog even if Windows sends no new foreground event. */
    focus_is(b,b);signal_foreground(b);actual_foreground=dialog_child;
    bind(0,NULL);CHECK(!app.scratch.visible && !IsWindowVisible(b));
    expect_refresh(NULL,0);check_alpha(b,217);
    ShowWindow(b,SW_SHOWNOACTIVATE);signal_event(EVENT_OBJECT_SHOW,b);
    expect_refresh(NULL,0);check_alpha(b,0); /* A member can reappear without reopening the workspace. */
    CHECK(app.scratch.count==3 && GetPropW(b,app.api.tag));
    tray_toggle();CHECK(app.scratch.visible && IsWindowVisible(b));
    expect_refresh(dialog,1);check_alpha(dialog,217);check_alpha(b,102);

    signal_foreground(dialog);actual_foreground=b;
    bind(1,b);CHECK(app.scratch.count==2 && IsWindowVisible(b));check_alpha(b,0);
    expect_refresh(NULL,1);check_alpha(b,0);check_alpha(a,102);
    signal_foreground(a);restore_all();expect_refresh(NULL,0);
    check_alpha(a,0);check_alpha(dialog,0);check_alpha(b,0);
    stop_member_appearance(&app);CHECK(unhooks==2*HOOK_COUNT-1);

    /* With only an inactive override, focus restores native alpha and key.
       Observer refusal cannot suppress the explicit host refresh. */
    profile.appearance.member_opacity=(SpAppearanceValue){0};
    const COLORREF key=RGB(17,40,90);
    CHECK(SetWindowLongPtrW(b,GWL_EXSTYLE,GetWindowLongPtrW(b,GWL_EXSTYLE)|WS_EX_LAYERED));
    CHECK(SetLayeredWindowAttributes(b,key,166,LWA_ALPHA|LWA_COLORKEY));
    refuse_hooks=true;start_member_appearance(&app);CHECK(registrations==3*HOOK_COUNT-1 && !hooks[0].callback);
    bind(1,a);bind(1,b);actual_foreground=a;bind(0,NULL);
    expect_refresh(a,1);check_alpha(a,0);check_keyed(b,key,102);
    bind(0,NULL);actual_foreground=b;tray_toggle();expect_refresh(b,1);
    check_alpha(a,102);check_keyed(b,key,166);
    stop_member_appearance(&app);refuse_hooks=false;start_member_appearance(&app);
    focus_is(grandchild,a);check_alpha(a,0);check_keyed(b,key,102);

    signal_foreground(b);CHECK(app.appearance_queued);
    CHECK(SendMessageW(app.window,app.control,SP_CONTROL_QUIT,0)==1);
    CHECK(!IsWindow(app.window) && app.closing && !app.appearance_queued && !app.focused_member && unhooks==3*HOOK_COUNT-1);
    CHECK(IsWindowVisible(a) && IsWindowVisible(b) && !GetPropW(b,app.api.tag));
    check_alpha(a,0);check_keyed(b,key,166);
    CHECK(border_reads==reads_before && border_writes==writes_before); /* Omission never touches DWM border. */
    sp_win32_dispose(&app.api);CloseHandle(app.cancel);
    CHECK(DestroyWindow(a) && !IsWindow(grandchild) && !IsWindow(dialog_child));
    CHECK(DestroyWindow(b));
    CHECK(UnregisterClassW(window_class.lpszClassName,window_class.hInstance));
    CHECK(UnregisterClassW(host_class.lpszClassName,host_class.hInstance));
    printf("foreground: %u checks passed (native window families, opacity and host dispatch; replayed WinEvents, simulated border attribute)\n",checks);
    return 0;
}
