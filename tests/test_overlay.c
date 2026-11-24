#include "overlay.h"
#include "shield.h"
#include "config.h"
#include <dwmapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned checks;
static LONG installs,uninstalls,fail_hook,fail_position,slow_position,busy_buttons,held_key,gui_flags;
static LONG manual_animation,zero_visible_alpha;
static LONG64 fade_clock,last_animation_timer;
static PVOID volatile foreground_window,capture_window;
static LRESULT next_result;
static SpOverlay *surface;
static SpWin32 api;
static SpState scratch;
static HWND owner,background,tile,dialog,child;
#define CHECK(x) do {++checks;if(!(x)){fprintf(stderr,"%s:%d: %s\n",__FILE__,__LINE__,#x);exit(1);}}while(0)

/* Production callback replay, not synthesized OS input. Real hook installation
   and teardown are independently exercised by the actual executable host suite. */
static HHOOK WINAPI test_hook(int id,HOOKPROC proc,HINSTANCE instance,DWORD thread) {
    (void)proc;(void)instance;(void)thread;
    if (id==InterlockedCompareExchange(&fail_hook,0,0)) {SetLastError(ERROR_ACCESS_DENIED);return NULL;}
    InterlockedIncrement(&installs);return (HHOOK)(UINT_PTR)id;
}
static BOOL WINAPI test_unhook(HHOOK hook){(void)hook;InterlockedIncrement(&uninstalls);return TRUE;}
static LRESULT WINAPI test_next(HHOOK hook,int code,WPARAM wp,LPARAM lp){(void)hook;(void)code;(void)wp;(void)lp;return next_result;}
static HWND WINAPI test_foreground(void){return (HWND)InterlockedCompareExchangePointer(&foreground_window,NULL,NULL);}
static BOOL WINAPI test_focus(HWND h){(void)h;return TRUE;}
static SHORT WINAPI test_async(int vk){
    return (vk=='A' && InterlockedCompareExchange(&held_key,0,0)) ||
        ((vk==VK_LBUTTON || vk==VK_RBUTTON) && InterlockedCompareExchange(&busy_buttons,0,0))?(SHORT)0x8000:0;
}
static BOOL WINAPI test_gui(DWORD thread,PGUITHREADINFO gui){
    (void)thread;gui->hwndActive=test_foreground();gui->hwndCapture=(HWND)InterlockedCompareExchangePointer(&capture_window,NULL,NULL);
    gui->flags=(DWORD)InterlockedCompareExchange(&gui_flags,0,0);return TRUE;
}
static BOOL WINAPI test_position(HWND h,HWND after,int x,int y,int w,int height,UINT flags){
    if (InterlockedCompareExchange(&fail_position,0,0)) {SetLastError(ERROR_ACCESS_DENIED);return FALSE;}
    if (InterlockedExchange(&slow_position,0)) Sleep(1750);
    return SetWindowPos(h,after,x,y,w,height,flags);
}
static ULONGLONG WINAPI test_clock(void){return (ULONGLONG)InterlockedCompareExchange64(&fade_clock,0,0);}
static BOOL WINAPI test_alpha(HWND h,COLORREF key,BYTE alpha,DWORD flags){
    if(!alpha && IsWindowVisible(h))InterlockedIncrement(&zero_visible_alpha);
    return SetLayeredWindowAttributes(h,key,alpha,flags);
}
static UINT_PTR WINAPI test_timer(HWND h,UINT_PTR id,UINT elapsed,TIMERPROC proc);
#define SetWindowsHookExW test_hook
#define UnhookWindowsHookEx test_unhook
#define CallNextHookEx test_next
#define GetForegroundWindow test_foreground
#define SetForegroundWindow test_focus
#define GetAsyncKeyState test_async
#define GetGUIThreadInfo test_gui
#define SetWindowPos test_position
#define GetTickCount64 test_clock
#define SetLayeredWindowAttributes test_alpha
#define SetTimer test_timer
#include "../src/overlay.c"
#undef SetWindowsHookExW
#undef UnhookWindowsHookEx
#undef CallNextHookEx
#undef GetForegroundWindow
#undef SetForegroundWindow
#undef GetAsyncKeyState
#undef GetGUIThreadInfo
#undef SetWindowPos
#undef GetTickCount64
#undef SetLayeredWindowAttributes
#undef SetTimer

static UINT_PTR WINAPI test_timer(HWND h,UINT_PTR id,UINT elapsed,TIMERPROC proc){
    if(id==SP_OVERLAY_ORDER_TIMER)return SetTimer(h,id,elapsed,proc);
    /* Keep the real ordering timer. Disable only automatic fade delivery when
       replaying obsolete IDs, so a valid tick cannot mask the replay's effect. */
    UINT_PTR timer=InterlockedCompareExchange(&manual_animation,0,0)?id:SetTimer(h,id,elapsed,proc);
    InterlockedExchange64(&last_animation_timer,(LONG64)timer);
    return timer;
}

static void cleanup(void){
    if(surface){scratch.workspace=NULL;sp_overlay_destroy(surface);surface=NULL;}
    (void)sp_release_all(&scratch);sp_win32_dispose(&api);
    if(child)DestroyWindow(child);if(dialog)DestroyWindow(dialog);if(tile)DestroyWindow(tile);
    if(background)DestroyWindow(background);if(owner)DestroyWindow(owner);
}
static LRESULT CALLBACK window_proc(HWND h,UINT msg,WPARAM wp,LPARAM lp){return DefWindowProcW(h,msg,wp,lp);}
static bool fixture_focus(void *ctx,SpWindow window){return sp_win32_ops().alive(ctx,window);}
static bool above(HWND a,HWND b){
    for(HWND h=GetTopWindow(NULL);h;h=GetWindow(h,GW_HWNDNEXT)){if(h==a)return true;if(h==b)return false;}return false;
}
static LRESULT key(unsigned vk,unsigned scan,bool down,DWORD flags){
    KBDLLHOOKSTRUCT event={0};event.vkCode=vk;event.scanCode=scan&0xff;
    event.flags=flags|((scan&0x100)?LLKHF_EXTENDED:0);
    return overlay_keyboard(HC_ACTION,down?WM_KEYDOWN:WM_KEYUP,(LPARAM)&event);
}
static LRESULT mouse(UINT msg,POINT p,DWORD data){
    MSLLHOOKSTRUCT event={0};event.pt=p;event.mouseData=data;
    return overlay_mouse(HC_ACTION,msg,(LPARAM)&event);
}
static void presses(void){
    SpShieldPress p={0};
    CHECK(!sp_shield_press(&p,false,true)); /* Unknown/preexisting up is not owned. */
    CHECK(!sp_shield_press(&p,true,false));
    CHECK(sp_shield_press(&p,true,true)); /* Suppress repeat, not the already-delivered release. */
    CHECK(!sp_shield_press(&p,false,true));
    CHECK(sp_shield_press(&p,true,true));
    CHECK(sp_shield_press(&p,true,false));
    CHECK(sp_shield_press(&p,false,false));
    CHECK(!sp_shield_press(&p,false,false));
    CHECK(sp_shield_key('A',0x1e)==sp_shield_key('Q',0x1e));
    CHECK(sp_shield_key('A',0)==512+'A');CHECK(sp_shield_key(256,0)==768);
    CHECK(sp_shield_key(255,511)==511);CHECK(sp_shield_key(255,512)==767);
    for(unsigned sequence=0;sequence<256;++sequence){
        SpShieldPress press={0};bool original=false;
        for(unsigned i=0;i<4;++i){
            bool down=(sequence>>(i*2))&1,block=(sequence>>(i*2+1))&1;
            bool was_down=press.pressed,was_swallowed=press.swallowed;
            bool got=sp_shield_press(&press,down,block);
            if(down && !was_down)original=block;
            CHECK(got==(down?(original||block):was_swallowed));
            if(!down)CHECK(!press.pressed&&!press.swallowed);
        }
    }
}
static void rect_equal(HWND h,SpRect r){
    RECT got;CHECK(GetWindowRect(h,&got));CHECK(got.left==r.x&&got.top==r.y&&got.right-got.left==r.w&&got.bottom-got.top==r.h);
}
/* Save only our fixture rectangle, never the surrounding user's desktop. */
static COLORREF capture(const char *path,SpRect area){
    (void)DwmFlush();Sleep(80);
    HDC screen=GetDC(NULL),memory=CreateCompatibleDC(screen);CHECK(screen&&memory);
    HBITMAP bitmap=CreateCompatibleBitmap(screen,area.w,area.h);CHECK(bitmap);
    HGDIOBJ old=SelectObject(memory,bitmap);CHECK(old);
    CHECK(BitBlt(memory,0,0,area.w,area.h,screen,area.x,area.y,SRCCOPY|CAPTUREBLT));
    COLORREF color=GetPixel(memory,area.w/2,area.h/2);CHECK(color!=CLR_INVALID);
    BITMAPINFO info={0};info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);info.bmiHeader.biWidth=area.w;
    info.bmiHeader.biHeight=-area.h;info.bmiHeader.biPlanes=1;info.bmiHeader.biBitCount=32;info.bmiHeader.biCompression=BI_RGB;
    DWORD size=(DWORD)area.w*(DWORD)area.h*4;void *pixels=malloc(size);CHECK(pixels);
    SelectObject(memory,old);CHECK(GetDIBits(memory,bitmap,0,(UINT)area.h,pixels,&info,DIB_RGB_COLORS));
    BITMAPFILEHEADER file={0};file.bfType=0x4d42;file.bfOffBits=sizeof(file)+sizeof(info.bmiHeader);file.bfSize=file.bfOffBits+size;
    FILE *out=fopen(path,"wb");CHECK(out);CHECK(fwrite(&file,sizeof(file),1,out)==1);
    CHECK(fwrite(&info.bmiHeader,sizeof(info.bmiHeader),1,out)==1);CHECK(fwrite(pixels,size,1,out)==1);CHECK(fclose(out)==0);
    free(pixels);DeleteObject(bitmap);DeleteDC(memory);ReleaseDC(NULL,screen);return color;
}
static void configured_alpha(SpRect area){
    static SpConfig config;
    static const struct {const wchar_t *text;BYTE applied;} cases[]={
        {NULL,36},
        {L"profile default\nbind Win+S scratch.toggle\nend",36},
        {L"tint-opacity 0.5\nprofile default\nbind Win+S scratch.toggle\nend",128},
        {L"profile default\ntint-opacity 1.0\nbind Win+S scratch.toggle\nend",255},
        {L"tint-opacity 1\nprofile default\ntint-opacity 0.0\nbind Win+S scratch.toggle\nend",1},
        {L"tint-opacity 0.001\nprofile default\nbind Win+S scratch.toggle\nend",1}
    };
    ShowWindow(tile,SW_HIDE);InterlockedExchangePointer(&foreground_window,background);
    POINT gap={area.x+4,area.y+4};SpState empty={0};
    for(size_t i=0;i<sizeof(cases)/sizeof(*cases);++i){
        wchar_t error[256];
        if(cases[i].text)CHECK(sp_config_parse(&config,cases[i].text,error,256));
        else sp_config_defaults(&config);
        surface=sp_overlay_create(owner,api.tag,(BYTE)sp_tint_alpha(&config.profiles[0]),error,256);CHECK(surface);
        HWND h=surface->window;COLORREF color=CLR_INVALID;BYTE alpha=0;DWORD flags=0;
        CHECK(GetLayeredWindowAttributes(h,&color,&alpha,&flags));
        CHECK(alpha==cases[i].applied && flags==LWA_ALPHA);
        CHECK(!(GetWindowLongPtrW(h,GWL_EXSTYLE)&WS_EX_TRANSPARENT));
        CHECK(sp_overlay_workspace(surface,&empty,true,area));CHECK(IsWindowVisible(h));
        (void)DwmFlush();CHECK(WindowFromPoint(gap)==h);CHECK(IsWindowEnabled(background));
        CHECK(key('Z',0x2c,true,0)==1);CHECK(key('Z',0x2c,false,0)==1);
        CHECK(mouse(WM_LBUTTONDOWN,gap,0)==1);CHECK(mouse(WM_LBUTTONUP,gap,0)==1);
        CHECK(mouse(WM_MOUSEWHEEL,gap,0)==1);
        CHECK(sp_overlay_workspace(surface,&empty,false,area));CHECK(!IsWindowVisible(h));
        CHECK(WindowFromPoint(gap)==background);
        CHECK(key('Z',0x2c,true,0)==0);CHECK(key('Z',0x2c,false,0)==0);
        CHECK(mouse(WM_LBUTTONDOWN,gap,0)==0);CHECK(mouse(WM_LBUTTONUP,gap,0)==0);
        sp_overlay_destroy(surface);surface=NULL;CHECK(!IsWindow(h));
    }
    CHECK(installs==uninstalls);
}
enum { FADE_TEST_MS=400, FADE_TEST_ALPHA=129 };

static BYTE layered_alpha(HWND h){
    BYTE alpha=0;DWORD flags=0;
    CHECK(GetLayeredWindowAttributes(h,NULL,&alpha,&flags) && flags==LWA_ALPHA && alpha>0);
    return alpha;
}
static void await_fade_alpha(HWND h,BYTE lower,BYTE upper){
    ULONGLONG start=GetTickCount64();BYTE alpha=0;bool valid;
    do {
        DWORD flags=0;
        valid=IsWindowVisible(h) && GetLayeredWindowAttributes(h,NULL,&alpha,&flags) && flags==LWA_ALPHA && alpha>0;
        if(!valid || (alpha>=lower && alpha<=upper))break;
        Sleep(5);
    } while(GetTickCount64()-start<2500);
    CHECK(valid && alpha>=lower && alpha<=upper);
}
static void expect_hide_completion(HWND h,uintptr_t request){
    ULONGLONG start=GetTickCount64();MSG notice={0};
    while(!PeekMessageW(&notice,owner,SP_OVERLAY_HIDE_COMPLETE,SP_OVERLAY_HIDE_COMPLETE,PM_REMOVE) &&
        GetTickCount64()-start<2500)Sleep(5);
    CHECK(notice.message==SP_OVERLAY_HIDE_COMPLETE && notice.wParam==request);
    bool hidden=false;
    CHECK(sp_overlay_hide_completion(surface,request,&hidden) && hidden && !IsWindowVisible(h));
}
static void advance_fade(UINT ms){InterlockedAdd64(&fade_clock,ms);}
static UINT_PTR animation_timer(void){return (UINT_PTR)InterlockedCompareExchange64(&last_animation_timer,0,0);}
static void replay_tick(HWND h,UINT_PTR timer){
    DWORD_PTR ignored;
    CHECK(timer && SendMessageTimeoutW(h,WM_TIMER,timer,0,SMTO_BLOCK|SMTO_ABORTIFHUNG,2500,&ignored));
}
static void animated_alpha(SpRect area){
    wchar_t error[256];SpState empty={0};POINT gap={area.x+4,area.y+4};
    surface=sp_overlay_create_with_fade(owner,api.tag,FADE_TEST_ALPHA,FADE_TEST_MS,error,256);CHECK(surface);
    HWND h=surface->window;uintptr_t request=0,new_request=0;bool hidden=false;

    /* The test advances time only after the API returns. In this section only
       actual HWND timer delivery can move alpha or finish the hide. */
    CHECK(sp_overlay_workspace(surface,&empty,true,area));
    CHECK(IsWindowVisible(h) && layered_alpha(h)==1 && WindowFromPoint(gap)==h);
    advance_fade(FADE_TEST_MS/2);await_fade_alpha(h,2,FADE_TEST_ALPHA-1);
    advance_fade(FADE_TEST_MS/2);await_fade_alpha(h,FADE_TEST_ALPHA,FADE_TEST_ALPHA);
    CHECK(sp_overlay_hide(surface,&empty,&request)==SP_OVERLAY_HIDE_PENDING && request);
    CHECK(!sp_overlay_hide_completion(surface,request,&hidden) && IsWindowVisible(h) && layered_alpha(h)==FADE_TEST_ALPHA);
    advance_fade(FADE_TEST_MS/2);await_fade_alpha(h,2,FADE_TEST_ALPHA-1);
    advance_fade(FADE_TEST_MS/2-1);await_fade_alpha(h,1,1);
    CHECK(WindowFromPoint(gap)==h && !sp_overlay_hide_completion(surface,request,&hidden));
    advance_fade(1);expect_hide_completion(h,request);CHECK(WindowFromPoint(gap)==background);

    /* Repeats adopt the live timer and preserve its deadline. Only the newest
       deferred Hide token can complete when that timer reaches the endpoint. */
    InterlockedExchange(&manual_animation,1);
    CHECK(sp_overlay_workspace(surface,&empty,true,area));UINT_PTR entering=animation_timer();
    advance_fade(FADE_TEST_MS/2);replay_tick(h,entering);BYTE middle=layered_alpha(h);
    CHECK(middle>1 && middle<FADE_TEST_ALPHA);
    CHECK(sp_overlay_workspace(surface,&empty,true,area) && animation_timer()==entering && layered_alpha(h)==middle);
    advance_fade(FADE_TEST_MS/2);replay_tick(h,entering);CHECK(layered_alpha(h)==FADE_TEST_ALPHA);
    CHECK(sp_overlay_hide(surface,&empty,&request)==SP_OVERLAY_HIDE_PENDING && request);UINT_PTR leaving=animation_timer();
    advance_fade(FADE_TEST_MS/2);replay_tick(h,leaving);
    CHECK(sp_overlay_hide(surface,&empty,&new_request)==SP_OVERLAY_HIDE_PENDING && new_request && new_request!=request);
    CHECK(animation_timer()==leaving && !sp_overlay_hide_completion(surface,request,&hidden));
    advance_fade(FADE_TEST_MS/2);replay_tick(h,leaving);expect_hide_completion(h,new_request);
    CHECK(!sp_overlay_hide_completion(surface,request,&hidden));

    /* An obsolete tick must not advance the replacement animation or finish
       the old Hide, even when injected after its original deadline. */
    CHECK(sp_overlay_workspace(surface,&empty,true,area));entering=animation_timer();
    advance_fade(FADE_TEST_MS/2);replay_tick(h,entering);middle=layered_alpha(h);
    CHECK(sp_overlay_hide(surface,&empty,&request)==SP_OVERLAY_HIDE_PENDING && request);
    leaving=animation_timer();CHECK(leaving!=entering && layered_alpha(h)==middle);
    advance_fade(FADE_TEST_MS/4);replay_tick(h,entering);CHECK(layered_alpha(h)==middle);
    replay_tick(h,leaving);BYTE dimmer=layered_alpha(h);CHECK(dimmer>1 && dimmer<middle);
    CHECK(sp_overlay_workspace(surface,&empty,true,area));UINT_PTR reversed=animation_timer();
    CHECK(reversed!=leaving && layered_alpha(h)==dimmer);
    advance_fade(FADE_TEST_MS);replay_tick(h,leaving);
    CHECK(IsWindowVisible(h) && layered_alpha(h)==dimmer && !sp_overlay_hide_completion(surface,request,&hidden));
    replay_tick(h,reversed);CHECK(layered_alpha(h)==FADE_TEST_ALPHA);
    CHECK(sp_overlay_workspace(surface,&empty,false,area));
    replay_tick(h,entering);replay_tick(h,leaving);replay_tick(h,reversed);
    CHECK(!IsWindowVisible(h) && WindowFromPoint(gap)==background);
    CHECK(!InterlockedCompareExchange(&zero_visible_alpha,0,0));
    sp_overlay_destroy(surface);surface=NULL;CHECK(!IsWindow(h) && installs==uninstalls);
}
int main(void){
    presses();atexit(cleanup);sp_win32_dpi();
    WNDCLASSW cls={0};cls.lpfnWndProc=window_proc;cls.hInstance=GetModuleHandleW(NULL);
    cls.lpszClassName=L"Scratchpad.OverlayFixture";cls.hbrBackground=GetStockObject(WHITE_BRUSH);CHECK(RegisterClassW(&cls));
    owner=CreateWindowExW(WS_EX_TOOLWINDOW,cls.lpszClassName,L"Overlay fixture owner",WS_POPUP,0,0,0,0,NULL,NULL,cls.hInstance,NULL);CHECK(owner);
    SpRect monitor;CHECK(sp_win32_init(&api,true));CHECK(sp_win32_workarea(&api,&monitor));
    SpRect area={monitor.x+30,monitor.y+30,400,300};
    background=CreateWindowExW(WS_EX_TOOLWINDOW|WS_EX_TOPMOST,cls.lpszClassName,L"Disposable background",WS_POPUP|WS_VISIBLE,
        area.x,area.y,area.w,area.h,NULL,NULL,cls.hInstance,NULL);CHECK(background);
    tile=CreateWindowExW(WS_EX_TOOLWINDOW,cls.lpszClassName,L"Disposable scratch member",WS_POPUP|WS_VISIBLE,
        area.x+60,area.y+60,200,150,NULL,NULL,cls.hInstance,NULL);CHECK(tile);
    SpWindowOps ops=sp_win32_ops();ops.focus=fixture_focus; /* No foreground permission assumption. */
    CHECK(sp_init(&scratch,ops,&api));
    InterlockedExchangePointer(&foreground_window,background);
    wchar_t error[256];
    InterlockedExchange(&fail_hook,WH_MOUSE_LL);
    CHECK(!sp_overlay_create(owner,api.tag,(BYTE)sp_tint_alpha(NULL),error,256));CHECK(error[0]);
    InterlockedExchange(&fail_hook,0);
    InterlockedExchange(&held_key,1);
    surface=sp_overlay_create(owner,api.tag,(BYTE)sp_tint_alpha(NULL),error,256);CHECK(surface);
    scratch.workspace=sp_overlay_workspace;scratch.workspace_ctx=surface;
    HWND backdrop=surface->window;CHECK(IsWindow(backdrop));CHECK(!IsWindowVisible(backdrop));
    CHECK(!GetWindow(backdrop,GW_OWNER)); /* Independent queues: no show/destroy deadlock. */
    LONG_PTR style=GetWindowLongPtrW(backdrop,GWL_EXSTYLE);
    CHECK((style&(WS_EX_LAYERED|WS_EX_TOOLWINDOW|WS_EX_TOPMOST))==(WS_EX_LAYERED|WS_EX_TOOLWINDOW|WS_EX_TOPMOST));
    CHECK(!(style&WS_EX_TRANSPARENT));
    COLORREF transparent;BYTE alpha;DWORD flags;
    CHECK(GetLayeredWindowAttributes(backdrop,&transparent,&alpha,&flags));CHECK(alpha==36&&flags==LWA_ALPHA&&alpha>0&&alpha<64);
    CHECK(sp_toggle_member(&scratch,(uintptr_t)tile,area).status==SP_OK);
    CHECK(sp_toggle_member(&scratch,(uintptr_t)tile,area).status==SP_OK);
    ShowWindow(tile,SW_HIDE);
    CHECK(UpdateWindow(background));COLORREF before=capture("build/overlay-inactive.bmp",area);
    CHECK(sp_toggle(&scratch,area).status==SP_WINDOW_ERROR);CHECK(!scratch.visible&&!IsWindowVisible(backdrop));
    InterlockedExchange(&held_key,0);
    InterlockedExchange(&busy_buttons,1);
    CHECK(sp_toggle(&scratch,area).status==SP_WINDOW_ERROR);CHECK(!scratch.visible&&!IsWindowVisible(backdrop));
    InterlockedExchange(&busy_buttons,0);
    InterlockedExchange(&fail_position,1);
    CHECK(sp_toggle(&scratch,area).status==SP_WINDOW_ERROR);CHECK(!scratch.visible&&!IsWindowVisible(backdrop));
    InterlockedExchange(&fail_position,0);
    CHECK(sp_toggle(&scratch,area).status==SP_OK);CHECK(scratch.visible&&IsWindowVisible(backdrop));rect_equal(backdrop,area);
    CHECK(IsWindowEnabled(background));CHECK(above(backdrop,background));
    COLORREF tinted=capture("build/overlay-active.bmp",area);
    CHECK(GetRValue(before)>GetRValue(tinted)+20&&GetGValue(before)>GetGValue(tinted)+20&&GetBValue(before)>GetBValue(tinted)+20);
    CHECK(GetRValue(tinted)>170&&GetGValue(tinted)>170&&GetBValue(tinted)>170);
    POINT gap={area.x+4,area.y+4},outside={area.x-2,area.y-2};
    CHECK(WindowFromPoint(gap)==backdrop);CHECK(underneath(surface,background));
    CHECK(mouse(WM_MOUSEMOVE,gap,0)==0);
    CHECK(key('A',0x1e,true,0)==1);CHECK(key('A',0x1e,true,0)==1);CHECK(key('A',0x1e,false,0)==1);
    CHECK(key('B',0x30,true,LLKHF_INJECTED)==1);CHECK(key('B',0x30,false,LLKHF_INJECTED)==1);
    CHECK(mouse(WM_LBUTTONDOWN,gap,0)==1);CHECK(mouse(WM_LBUTTONUP,outside,0)==1);
    CHECK(mouse(WM_RBUTTONDOWN,gap,0)==1);CHECK(mouse(WM_RBUTTONUP,gap,0)==1);
    CHECK(mouse(WM_MBUTTONDOWN,gap,0)==1);CHECK(mouse(WM_MBUTTONUP,gap,0)==1);
    CHECK(mouse(WM_XBUTTONDOWN,gap,XBUTTON1<<16)==1);CHECK(mouse(WM_XBUTTONUP,gap,XBUTTON1<<16)==1);
    CHECK(mouse(WM_XBUTTONDOWN,gap,XBUTTON2<<16)==1);CHECK(mouse(WM_XBUTTONUP,gap,XBUTTON2<<16)==1);
    CHECK(mouse(WM_MOUSEWHEEL,gap,120u<<16)==1);CHECK(mouse(WM_MOUSEHWHEEL,gap,120u<<16)==1);
    CHECK(mouse(WM_LBUTTONDOWN,outside,0)==0);CHECK(mouse(WM_LBUTTONUP,outside,0)==0);
    next_result=7;CHECK(overlay_keyboard(-1,0,0)==7);CHECK(overlay_mouse(-1,0,0)==7);
    CHECK(key('S',0x1f,true,0)==7);CHECK(key('S',0x1f,false,0)==7);next_result=0;
    ShowWindow(tile,SW_SHOW);CHECK(sp_toggle_member(&scratch,(uintptr_t)tile,area).status==SP_OK);
    CHECK(scratch.count==1&&scratch.visible&&IsWindowVisible(tile));CHECK(above(tile,backdrop));
    InterlockedExchangePointer(&foreground_window,tile);
    RECT r;CHECK(GetWindowRect(tile,&r));POINT middle={(r.left+r.right)/2,(r.top+r.bottom)/2};
    CHECK(WindowFromPoint(middle)==tile);CHECK(!underneath(surface,tile));
    CHECK(key('C',0x2e,true,0)==0);CHECK(key('C',0x2e,false,0)==0);
    CHECK(mouse(WM_LBUTTONDOWN,middle,0)==0);CHECK(mouse(WM_LBUTTONUP,middle,0)==0);CHECK(mouse(WM_MOUSEWHEEL,middle,0)==0);
    dialog=CreateWindowExW(WS_EX_TOOLWINDOW,cls.lpszClassName,L"Owned scratch dialog",WS_POPUP|WS_VISIBLE,r.left+10,r.top+10,80,80,tile,NULL,cls.hInstance,NULL);CHECK(dialog);
    child=CreateWindowExW(0,L"STATIC",L"Scratch child",WS_CHILD|WS_VISIBLE,0,0,40,40,tile,NULL,cls.hInstance,NULL);CHECK(child);
    CHECK(family(surface,dialog)&&family(surface,child));CHECK(!family(surface,background));
    InterlockedExchangePointer(&foreground_window,dialog);CHECK(key('D',0x20,true,0)==0);CHECK(key('D',0x20,false,0)==0);
    InterlockedExchangePointer(&capture_window,tile);CHECK(!pointer_blocked(surface,gap,false,false));CHECK(mouse(WM_MOUSEMOVE,gap,0)==0);
    InterlockedExchangePointer(&capture_window,background);CHECK(pointer_blocked(surface,gap,false,false));CHECK(mouse(WM_MOUSEMOVE,gap,0)==1);InterlockedExchangePointer(&capture_window,NULL);
    CHECK(DestroyWindow(dialog));dialog=NULL;CHECK(DestroyWindow(child));child=NULL;
    /* Losing the last member must preserve an active empty surface. */
    CHECK(sp_toggle_member(&scratch,(uintptr_t)tile,area).status==SP_OK);CHECK(!scratch.count&&scratch.visible&&IsWindowVisible(backdrop));
    CHECK(!family(surface,tile));InterlockedExchangePointer(&foreground_window,background);
    CHECK(key('E',0x12,true,0)==1);
    CHECK(sp_toggle(&scratch,area).status==SP_OK);CHECK(!scratch.visible&&!IsWindowVisible(backdrop));
    CHECK(key('E',0x12,false,0)==1);CHECK(key('F',0x21,true,0)==0);CHECK(key('F',0x21,false,0)==0);
    CHECK(mouse(WM_MOUSEWHEEL,gap,0)==0);CHECK(mouse(WM_LBUTTONDOWN,gap,0)==0);CHECK(mouse(WM_LBUTTONUP,gap,0)==0);
    /* A timed-out request cannot show a delayed orphan backdrop. */
    InterlockedExchange(&slow_position,1);CHECK(sp_toggle(&scratch,area).status==SP_WINDOW_ERROR);CHECK(!scratch.visible);
    Sleep(400);CHECK(!IsWindowVisible(backdrop));
    CHECK(sp_toggle(&scratch,(SpRect){-420,-300,340,240}).status==SP_OK);rect_equal(backdrop,(SpRect){-420,-300,340,240});
    CHECK(sp_release_all(&scratch).status==SP_OK);CHECK(!IsWindowVisible(backdrop));
    CHECK(sp_toggle(&scratch,area).status==SP_OK);CHECK(sp_release_all(&scratch).status==SP_OK);CHECK(!IsWindowVisible(backdrop)&&IsWindowEnabled(background));
    scratch.workspace=NULL;sp_overlay_destroy(surface);surface=NULL;CHECK(!IsWindow(backdrop));CHECK(installs==uninstalls);
    configured_alpha(area);animated_alpha(area);
    cleanup();CHECK(UnregisterClassW(cls.lpszClassName,cls.hInstance));
    printf("overlay: %u checks passed (native tint/hit testing, timer fades and stale ticks; no synthesized user input)\n",checks);return 0;
}
