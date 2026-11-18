#include "win32.h"
#include "modules.h"
#include "../examples/window_report.h"
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>

static unsigned checks;
#define CHECK(x) do { ++checks; if (!(x)) { \
    fprintf(stderr,"%s:%d: %s\n",__FILE__,__LINE__,#x); exit(1); } } while(0)
static LRESULT CALLBACK proc(HWND h,UINT m,WPARAM w,LPARAM l) {
    return DefWindowProcW(h,m,w,l);
}
static unsigned notices,launches;
static void notice(void *ctx,const wchar_t *text) {(void)ctx;CHECK(text&&*text);++notices;}
static void reported(void *ctx,SpResult r) {(void)ctx;CHECK(r.status==SP_OK||r.status==SP_NOOP);}
static bool queued(void *ctx,const SpBinding *b) {(void)ctx;CHECK(b);++launches;return true;}
int main(void) {
    sp_win32_dpi();
    WNDCLASSW cls={0}; cls.lpfnWndProc=proc; cls.hInstance=GetModuleHandleW(NULL);
    cls.lpszClassName=L"WinScratchpad.DisposableTest"; CHECK(RegisterClassW(&cls));
    HWND a=CreateWindowExW(WS_EX_TOOLWINDOW,cls.lpszClassName,L"Disposable scratchpad test A",
        WS_POPUP|WS_VISIBLE,100,100,300,200,NULL,NULL,cls.hInstance,NULL);
    HWND b=CreateWindowExW(WS_EX_TOOLWINDOW,cls.lpszClassName,L"Disposable scratchpad test B",
        WS_POPUP|WS_VISIBLE,420,100,300,200,NULL,NULL,cls.hInstance,NULL);
    CHECK(a && b);
    SpWin32 api, reject; CHECK(sp_win32_init(&api,true)); CHECK(sp_win32_init(&reject,false));
    SpWindowOps ops=sp_win32_ops(); SpWindow w;
    CHECK(!ops.claim(&reject,(uintptr_t)a,&w));
    CHECK(!ops.claim(&api,(uintptr_t)GetDesktopWindow(),&w));
    CHECK(ops.claim(&api,(uintptr_t)a,&w)); CHECK(ops.alive(&api,w));
    CHECK(ops.hide(&api,w)); CHECK(!IsWindowVisible(a));
    CHECK(ops.tile(&api,w,(SpRect){120,130,320,240}));
    RECT r; CHECK(GetWindowRect(a,&r));
    CHECK(r.left==120 && r.top==130 && r.right-r.left==320 && r.bottom-r.top==240);
    CHECK(IsWindowVisible(a)); CHECK(GetWindowLongPtrW(a,GWL_EXSTYLE)&WS_EX_TOPMOST);
    CHECK(ops.release(&api,w)); CHECK(IsWindowVisible(a));
    CHECK(!(GetWindowLongPtrW(a,GWL_EXSTYLE)&WS_EX_TOPMOST)); CHECK(!ops.alive(&api,w));
    CHECK(ops.claim(&api,(uintptr_t)a,&w));
    CHECK(SetPropW(a,api.pending_tag,(HANDLE)w.cookie));
    CHECK(!ops.release(&api,w)); CHECK(ops.alive(&api,w));
    /* Model a delayed asynchronous hide: retain ownership until it is observed. */
    ShowWindow(a,SW_HIDE); CHECK(ops.release(&api,w)); CHECK(IsWindowVisible(a));
    CHECK(ops.claim(&api,(uintptr_t)a,&w));
    CHECK(SetPropW(a,api.tag,(HANDLE)(w.cookie+1))); CHECK(!ops.alive(&api,w));
    RemovePropW(a,api.tag);
    SpState s; CHECK(sp_init(&s,ops,&api)); SpRect area; CHECK(sp_win32_workarea(&api,&area));
    CHECK(area.w>0 && area.h>0);
    SpProfile profile={0};profile.layout=sp_default_layout();profile.name=L"test";
    SpModuleContext module={&s,&api,&profile,a,area,NULL,reported,notice,queued};SpBinding binding={0};
    binding.action=L"window.center";CHECK(sp_modules_run(&module,&binding));
    SpRect expected;CHECK(sp_layout_with(area,1,&expected,1,profile.layout)==SP_OK);
    CHECK(GetWindowRect(a,&r));CHECK(r.left==expected.x&&r.top==expected.y&&r.right-r.left==expected.w);
    binding.action=L"window.toggle-topmost";CHECK(sp_modules_run(&module,&binding));CHECK(GetWindowLongPtrW(a,GWL_EXSTYLE)&WS_EX_TOPMOST);
    CHECK(sp_modules_run(&module,&binding));CHECK(!(GetWindowLongPtrW(a,GWL_EXSTYLE)&WS_EX_TOPMOST));
    binding.action=L"window.minimize";CHECK(sp_modules_run(&module,&binding));
    ULONGLONG deadline=GetTickCount64()+1000;MSG pending;
    while(!IsIconic(a)&&GetTickCount64()<deadline){while(PeekMessageW(&pending,NULL,0,0,PM_REMOVE)){TranslateMessage(&pending);DispatchMessageW(&pending);}Sleep(1);}
    CHECK(IsIconic(a));ShowWindow(a,SW_RESTORE);
    CHECK(example_window_report(&module,&binding));CHECK(notices==1);
    binding.action=L"terminal.open";CHECK(sp_modules_run(&module,&binding));CHECK(launches==1);
    CHECK(sp_toggle_member(&s,(uintptr_t)a,area).status==SP_OK);
    binding.action=L"window.center";CHECK(!sp_modules_run(&module,&binding));CHECK(s.count==1&&notices==2);
    CHECK(sp_toggle_member(&s,(uintptr_t)b,area).status==SP_OK);
    CHECK(!IsWindowVisible(a) && !IsWindowVisible(b));
    SpResult result=sp_toggle(&s,area); CHECK(result.status==SP_OK || result.status==SP_WINDOW_ERROR);
    CHECK(IsWindowVisible(a) && IsWindowVisible(b)); CHECK(s.visible && s.count==2);
    CHECK(sp_toggle(&s,area).status==SP_OK);
    CHECK(DestroyWindow(b)); CHECK(sp_prune(&s)==1 && s.count==1);
    CHECK(sp_release_all(&s).status==SP_OK); CHECK(IsWindowVisible(a) && s.count==0);
    CHECK(DestroyWindow(a)); sp_win32_dispose(&api); sp_win32_dispose(&reject);
    CHECK(UnregisterClassW(cls.lpszClassName,cls.hInstance));
    printf("win32: %u checks passed (only disposable test-owned windows)\n",checks); return 0;
}
