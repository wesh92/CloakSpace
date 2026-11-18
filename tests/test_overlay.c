#include "overlay.h"
#include "shield.h"
#include "config.h"
#include <dwmapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned checks;
static LONG installs,uninstalls,fail_hook,fail_position,slow_position,busy_buttons,held_key,gui_flags;
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
#define SetWindowsHookExW test_hook
#define UnhookWindowsHookEx test_unhook
#define CallNextHookEx test_next
#define GetForegroundWindow test_foreground
#define SetForegroundWindow test_focus
#define GetAsyncKeyState test_async
#define GetGUIThreadInfo test_gui
#define SetWindowPos test_position
#include "../src/overlay.c"
#undef SetWindowsHookExW
#undef UnhookWindowsHookEx
#undef CallNextHookEx
#undef GetForegroundWindow
#undef SetForegroundWindow
#undef GetAsyncKeyState
#undef GetGUIThreadInfo
#undef SetWindowPos

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
    configured_alpha(area);
    cleanup();CHECK(UnregisterClassW(cls.lpszClassName,cls.hInstance));
    printf("overlay: %u checks passed (native tint/Z-order, callback replay; no synthesized user input)\n",checks);return 0;
}
