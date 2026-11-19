#include "win32.h"
#include "modules.h"
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>
static unsigned checks;
#define CHECK(x) do { ++checks; if (!(x)) { \
    fprintf(stderr,"%s:%d: %s\n",__FILE__,__LINE__,#x); exit(1); } } while(0)
typedef struct {HWND h;HANDLE ready,blocked,resume,drained;volatile LONG reshow,clamp;} Target;
static Target target;
static LRESULT CALLBACK proc(HWND h,UINT msg,WPARAM wp,LPARAM lp) {
    if(msg==WM_APP+1) {SetEvent(target.blocked);WaitForSingleObject(target.resume,5000);return 0;}
    if(msg==WM_APP+2) {SetEvent(target.drained);return 0;}
    if(msg==WM_WINDOWPOSCHANGING && InterlockedCompareExchange(&target.clamp,0,0)) {
        WINDOWPOS *p=(WINDOWPOS *)lp;if(!(p->flags&SWP_NOSIZE)){p->cx=111;p->cy=112;}
    }
    LRESULT r=DefWindowProcW(h,msg,wp,lp);
    if(msg==WM_WINDOWPOSCHANGED && (((WINDOWPOS *)lp)->flags&SWP_HIDEWINDOW) &&
        InterlockedCompareExchange(&target.reshow,0,0)) ShowWindow(h,SW_SHOWNOACTIVATE);
    if(msg==WM_DESTROY)PostQuitMessage(0);
    return r;
}
static DWORD WINAPI thread(void *unused) {
    (void)unused;WNDCLASSW c={0};c.lpfnWndProc=proc;c.hInstance=GetModuleHandleW(NULL);c.lpszClassName=L"CloakSpace.AsyncDisposable";
    if(RegisterClassW(&c))target.h=CreateWindowExW(WS_EX_TOOLWINDOW,c.lpszClassName,L"Disposable async scratchpad test",
        WS_POPUP|WS_VISIBLE,100,100,200,200,NULL,NULL,c.hInstance,NULL);
    SetEvent(target.ready);
    MSG m;while(target.h&&GetMessageW(&m,NULL,0,0)>0){TranslateMessage(&m);DispatchMessageW(&m);}
    UnregisterClassW(c.lpszClassName,c.hInstance);return 0;
}
static void block_target(void) {
    ResetEvent(target.blocked);ResetEvent(target.resume);CHECK(PostMessageW(target.h,WM_APP+1,0,0));
    CHECK(WaitForSingleObject(target.blocked,2000)==WAIT_OBJECT_0);
}
static unsigned notices;
static wchar_t last_notice[256];
static void notify(void *ctx,const wchar_t *message) {
    (void)ctx;++notices;swprintf(last_notice,256,L"%ls",message);
}
static bool topmost(void) {return (GetWindowLongPtrW(target.h,GWL_EXSTYLE)&WS_EX_TOPMOST)!=0;}
static void test_topmost(SpWin32 *api) {
    SpState scratch={0};SpProfile profile={0};profile.layout=sp_default_layout();
    SpModuleContext context={0};context.scratch=&scratch;context.windows=api;
    context.profile=&profile;context.target=target.h;context.notify=notify;
    SpBinding binding={0};binding.action=L"window.toggle-topmost";
    for (unsigned initial=0;initial<2;++initial) {
        if (topmost()!=!!initial) CHECK(sp_modules_run(&context,&binding));
        CHECK(topmost()==!!initial);
        unsigned before=notices;
        block_target();ULONGLONG start=GetTickCount64();
        bool first=sp_modules_run(&context,&binding);
        bool second=sp_modules_run(&context,&binding);
        ULONGLONG elapsed=GetTickCount64()-start;
        ResetEvent(target.drained);
        bool posted=PostMessageW(target.h,WM_APP+2,0,0)!=0;
        SetEvent(target.resume);
        CHECK(posted && WaitForSingleObject(target.drained,2500)==WAIT_OBJECT_0);
        CHECK(!first && !second && notices==before+2);
        CHECK(elapsed>=700 && wcsstr(last_notice,L"may still"));
        start=GetTickCount64();
        while (topmost()==!!initial && GetTickCount64()-start<2500) Sleep(5);
        CHECK(topmost()!=!!initial); /* Timed-out async requests may later apply. */
        bool settled=topmost();
        CHECK(sp_modules_run(&context,&binding));CHECK(topmost()!=settled);
        CHECK(sp_modules_run(&context,&binding));CHECK(topmost()==settled);
        CHECK(notices==before+2);
    }
}
int main(void) {
    sp_win32_dpi();target.ready=CreateEventW(NULL,TRUE,FALSE,NULL);target.blocked=CreateEventW(NULL,TRUE,FALSE,NULL);
    target.resume=CreateEventW(NULL,TRUE,FALSE,NULL);target.drained=CreateEventW(NULL,TRUE,FALSE,NULL);
    CHECK(target.ready&&target.blocked&&target.resume&&target.drained);
    HANDLE t=CreateThread(NULL,0,thread,NULL,0,NULL);CHECK(t);CHECK(WaitForSingleObject(target.ready,2000)==WAIT_OBJECT_0 && target.h);
    SpWin32 api;CHECK(sp_win32_init(&api,true));SpWindowOps ops=sp_win32_ops();SpWindow w;
    CHECK(ops.claim(&api,(uintptr_t)target.h,&w));
    InterlockedExchange(&target.reshow,1);block_target();
    CHECK(!ops.hide(&api,w));CHECK(ops.alive(&api,w));CHECK(!ops.release(&api,w));
    SetEvent(target.resume);
    ULONGLONG start=GetTickCount64();
    while(GetPropW(target.h,api.pending_tag)&&GetTickCount64()-start<2500)Sleep(5);
    CHECK(!GetPropW(target.h,api.pending_tag));
    while(!IsWindowVisible(target.h)&&GetTickCount64()-start<2500)Sleep(5);
    CHECK(IsWindowVisible(target.h));CHECK(ops.release(&api,w));
    InterlockedExchange(&target.reshow,0);CHECK(ops.claim(&api,(uintptr_t)target.h,&w));
    CHECK(ops.tile(&api,w,(SpRect){100,100,200,200}));
    block_target();start=GetTickCount64();
    CHECK(!ops.tile(&api,w,(SpRect){200,200,300,300}));CHECK(GetTickCount64()-start>=350);
    SetEvent(target.resume);Sleep(100);
    InterlockedExchange(&target.clamp,1);
    CHECK(!ops.tile(&api,w,(SpRect){120,120,400,400}));
    RECT r;CHECK(GetWindowRect(target.h,&r));CHECK(r.right-r.left==111 && r.bottom-r.top==112);
    CHECK(ops.release(&api,w));
    test_topmost(&api);
    CHECK(PostMessageW(target.h,WM_CLOSE,0,0));CHECK(WaitForSingleObject(t,2000)==WAIT_OBJECT_0);
    sp_win32_dispose(&api);CloseHandle(t);CloseHandle(target.ready);CloseHandle(target.blocked);CloseHandle(target.resume);CloseHandle(target.drained);
    printf("async windows: %u checks passed (delayed hide/auto-show, hung move/topmost, clamped geometry)\n",checks);return 0;
}
