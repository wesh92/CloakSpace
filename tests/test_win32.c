#include "win32.h"
#include "modules.h"
#include "../examples/window_report.h"
#include <dwmapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>

static unsigned checks;
#define CHECK(x) do { ++checks; if (!(x)) { \
    fprintf(stderr,"%s:%d: %s\n",__FILE__,__LINE__,#x); exit(1); } } while(0)
static HWND refuse_layered_removal, transparent_on_layering, restore_on_layering;
static SpState *restore_during_style;
static LRESULT CALLBACK proc(HWND h,UINT m,WPARAM w,LPARAM l) {
    if (m==WM_STYLECHANGING && (INT_PTR)w==GWL_EXSTYLE) {
        STYLESTRUCT *styles=(STYLESTRUCT *)l;
        if (h==refuse_layered_removal) styles->styleNew|=WS_EX_LAYERED;
        if (!(styles->styleOld&WS_EX_LAYERED) && (styles->styleNew&WS_EX_LAYERED)) {
            if (h==transparent_on_layering) styles->styleNew|=WS_EX_TRANSPARENT;
            if (h==restore_on_layering) CHECK(sp_release_all(restore_during_style).status==SP_WINDOW_ERROR);
        }
    }
    return DefWindowProcW(h,m,w,l);
}
static unsigned notices,launches;
static void notice(void *ctx,const wchar_t *text) {(void)ctx;CHECK(text&&*text);++notices;}
static void reported(void *ctx,SpResult r) {(void)ctx;CHECK(r.status==SP_OK||r.status==SP_NOOP);}
static bool queued(void *ctx,const SpBinding *b) {(void)ctx;CHECK(b);++launches;return true;}
static HWND opacity_window(const WNDCLASSW *cls,DWORD exstyle) {
    HWND h=CreateWindowExW(exstyle|WS_EX_TOOLWINDOW|WS_EX_NOACTIVATE,cls->lpszClassName,L"Disposable opacity fixture",
        WS_POPUP|WS_VISIBLE,100,100,240,180,NULL,NULL,cls->hInstance,NULL);
    CHECK(h);return h;
}
static bool layered(HWND h) {return (GetWindowLongPtrW(h,GWL_EXSTYLE)&WS_EX_LAYERED)!=0;}
static void set_per_pixel_layer(HWND h) {
    HDC dc=CreateCompatibleDC(NULL);CHECK(dc);
    BITMAPINFO info={.bmiHeader={.biSize=sizeof(BITMAPINFOHEADER),.biWidth=16,.biHeight=-16,
        .biPlanes=1,.biBitCount=32,.biCompression=BI_RGB}};
    void *pixels=NULL;HBITMAP bitmap=CreateDIBSection(dc,&info,DIB_RGB_COLORS,&pixels,NULL,0);
    CHECK(bitmap&&pixels);HGDIOBJ old=SelectObject(dc,bitmap);CHECK(old&&old!=HGDI_ERROR);
    for (size_t i=0;i<16u*16u;++i) ((DWORD *)pixels)[i]=0xff286e88;
    SIZE size={16,16};POINT source={0,0};BLENDFUNCTION blend={AC_SRC_OVER,0,255,AC_SRC_ALPHA};
    CHECK(UpdateLayeredWindow(h,NULL,NULL,&size,dc,&source,0,&blend,ULW_ALPHA));
    CHECK(SelectObject(dc,old)==bitmap);CHECK(DeleteObject(bitmap));CHECK(DeleteDC(dc));
}
static void check_unreadable_layer(HWND h) {
    DWORD flags;
    CHECK(layered(h));CHECK(!GetLayeredWindowAttributes(h,NULL,NULL,&flags));
}
static void check_opacity(HWND h,DWORD expected_flags,COLORREF expected_key,BYTE expected_alpha) {
    COLORREF key=0;BYTE alpha=0;DWORD flags=0;
    CHECK(layered(h));CHECK(GetLayeredWindowAttributes(h,&key,&alpha,&flags));CHECK(flags==expected_flags);
    if (flags&LWA_ALPHA) {
        if (alpha!=expected_alpha) fprintf(stderr,"opacity check %u: expected %u, got %u (flags %lu)\n",
            checks,(unsigned)expected_alpha,(unsigned)alpha,(unsigned long)flags);
        CHECK(alpha==expected_alpha);
    }
    if (flags&LWA_COLORKEY) CHECK(key==expected_key);
}
static void test_member_opacity(SpWin32 *api,SpWindowOps ops,const WNDCLASSW *cls,SpRect area) {
    const COLORREF key=RGB(12,34,56);
    HWND plain=opacity_window(cls,0),keyed=opacity_window(cls,WS_EX_LAYERED),
        unreadable=opacity_window(cls,WS_EX_LAYERED),other=opacity_window(cls,0);
    CHECK(SetLayeredWindowAttributes(keyed,RGB(1,2,3),0,LWA_COLORKEY));
    set_per_pixel_layer(unreadable);check_unreadable_layer(unreadable);
    SpState s;CHECK(sp_init(&s,ops,api));
    CHECK(sp_toggle_member(&s,(uintptr_t)plain,area).status==SP_OK);
    CHECK(sp_toggle_member(&s,(uintptr_t)keyed,area).status==SP_OK);
    CHECK(sp_toggle_member(&s,(uintptr_t)unreadable,area).status==SP_OK);
    SpWindow transient;CHECK(ops.claim(api,(uintptr_t)other,&transient));
    SpResult shown=sp_toggle(&s,area);CHECK(shown.status==SP_OK||shown.status==SP_WINDOW_ERROR);
    CHECK(s.visible&&s.count==3);
    CHECK(sp_win32_member_visible(api,s.windows[0]));
    CHECK(sp_win32_member_visible(api,s.windows[1]));
    sp_win32_apply_member_opacity(api,&s,keyed,255,0); /* Native already; do not snapshot yet. */
    CHECK(SetLayeredWindowAttributes(keyed,key,0,LWA_COLORKEY));
    sp_win32_apply_member_opacity(api,&s,plain,0,0);
    CHECK(!layered(plain));check_opacity(keyed,LWA_COLORKEY,key,0);

    ShowWindow(plain,SW_HIDE);
    sp_win32_apply_member_opacity(api,&s,keyed,220,90);
    CHECK(!sp_win32_member_visible(api,s.windows[0])&&!layered(plain));
    CHECK(!layered(other));check_opacity(keyed,LWA_ALPHA|LWA_COLORKEY,key,220);
    check_unreadable_layer(unreadable);
    ShowWindow(plain,SW_SHOWMINNOACTIVE);
    CHECK(IsIconic(plain));
    sp_win32_apply_member_opacity(api,&s,keyed,220,90);
    CHECK(!sp_win32_member_visible(api,s.windows[0])&&!layered(plain));
    ShowWindow(plain,SW_SHOWNOACTIVATE);
    BOOL cloak=TRUE;
    if (SUCCEEDED(DwmSetWindowAttribute(plain,DWMWA_CLOAK,&cloak,sizeof(cloak)))) {
        CHECK(IsWindowVisible(plain)&&!sp_win32_member_visible(api,s.windows[0]));
        sp_win32_apply_member_opacity(api,&s,keyed,220,90);CHECK(!layered(plain));
        cloak=FALSE;CHECK(SUCCEEDED(DwmSetWindowAttribute(plain,DWMWA_CLOAK,&cloak,sizeof(cloak))));
    }
    ShowWindow(keyed,SW_HIDE);
    sp_win32_apply_member_opacity(api,&s,plain,220,90);
    check_opacity(plain,LWA_ALPHA,0,220);check_opacity(keyed,LWA_ALPHA|LWA_COLORKEY,key,220);
    ShowWindow(keyed,SW_SHOWNOACTIVATE);
    sp_win32_apply_member_opacity(api,&s,plain,220,90);
    check_opacity(plain,LWA_ALPHA,0,220);check_opacity(keyed,LWA_ALPHA|LWA_COLORKEY,key,90);
    sp_win32_apply_member_opacity(api,&s,keyed,220,90);
    check_opacity(plain,LWA_ALPHA,0,90);check_opacity(keyed,LWA_ALPHA|LWA_COLORKEY,key,220);
    sp_win32_apply_member_opacity(api,&s,keyed,0,90);
    check_opacity(keyed,LWA_COLORKEY,key,0);
    sp_win32_apply_member_opacity(api,&s,plain,0,90);
    CHECK(!layered(plain));check_opacity(keyed,LWA_ALPHA|LWA_COLORKEY,key,90);
    LONG_PTR native_style=GetWindowLongPtrW(plain,GWL_EXSTYLE);
    CHECK(SetWindowLongPtrW(plain,GWL_EXSTYLE,native_style|WS_EX_TRANSPARENT));
    sp_win32_apply_member_opacity(api,&s,NULL,220,90);
    CHECK(!layered(plain));
    CHECK(SetWindowLongPtrW(plain,GWL_EXSTYLE,native_style));
    transparent_on_layering=plain;
    sp_win32_apply_member_opacity(api,&s,NULL,220,90);
    transparent_on_layering=NULL;
    CHECK(!layered(plain)&&(GetWindowLongPtrW(plain,GWL_EXSTYLE)&WS_EX_TRANSPARENT));
    CHECK(SetWindowLongPtrW(plain,GWL_EXSTYLE,native_style));
    sp_win32_apply_member_opacity(api,&s,NULL,220,90);
    check_opacity(plain,LWA_ALPHA,0,90);check_opacity(keyed,LWA_ALPHA|LWA_COLORKEY,key,90);

    CHECK(sp_toggle(&s,area).status==SP_OK);CHECK(!s.visible&&!IsWindowVisible(plain));
    sp_win32_apply_member_opacity(api,&s,plain,220,90);check_opacity(plain,LWA_ALPHA,0,90);
    shown=sp_toggle(&s,area);CHECK(shown.status==SP_OK||shown.status==SP_WINDOW_ERROR);
    sp_win32_apply_member_opacity(api,&s,plain,220,90);check_opacity(plain,LWA_ALPHA,0,220);
    CHECK(SetWindowLongPtrW(plain,GWL_EXSTYLE,GetWindowLongPtrW(plain,GWL_EXSTYLE)|WS_EX_ACCEPTFILES));
    CHECK(ops.alive(api,transient)&&!layered(other));RemovePropW(other,api->tag);
    SpResult released=sp_toggle_member(&s,(uintptr_t)keyed,area);
    CHECK(released.status==SP_OK||released.status==SP_WINDOW_ERROR);CHECK(s.count==2);
    check_opacity(keyed,LWA_COLORKEY,key,0);

    CHECK(sp_toggle(&s,area).status==SP_OK);CHECK(!IsWindowVisible(plain));
    refuse_layered_removal=plain;
    CHECK(sp_release_all(&s).status==SP_WINDOW_ERROR);
    CHECK(s.count==1&&ops.alive(api,s.windows[0]));check_opacity(plain,LWA_ALPHA,0,220);
    check_unreadable_layer(unreadable);set_per_pixel_layer(unreadable);
    refuse_layered_removal=NULL;
    CHECK(sp_release_all(&s).status==SP_OK);CHECK(!s.count&&!layered(plain)&&IsWindowVisible(plain));
    CHECK(GetWindowLongPtrW(plain,GWL_EXSTYLE)&WS_EX_ACCEPTFILES);

    CHECK(sp_toggle_member(&s,(uintptr_t)plain,area).status==SP_OK);
    shown=sp_toggle(&s,area);CHECK(shown.status==SP_OK||shown.status==SP_WINDOW_ERROR);
    sp_win32_apply_member_opacity(api,&s,plain,220,90);check_opacity(plain,LWA_ALPHA,0,220);
    SpWindow stale=s.windows[0];CHECK(SetPropW(plain,api->tag,(HANDLE)(stale.cookie+1)));
    sp_win32_apply_member_opacity(api,&s,NULL,220,90);check_opacity(plain,LWA_ALPHA,0,220);
    CHECK(!ops.release(api,stale));CHECK(sp_release_all(&s).status==SP_OK);
    check_opacity(plain,LWA_ALPHA,0,220);CHECK(!s.count);

    /* A later claim has a new snapshot, including any preexisting native alpha. */
    CHECK(SetLayeredWindowAttributes(keyed,key,150,LWA_ALPHA|LWA_COLORKEY));
    CHECK(sp_toggle_member(&s,(uintptr_t)keyed,area).status==SP_OK);
    shown=sp_toggle(&s,area);CHECK(shown.status==SP_OK||shown.status==SP_WINDOW_ERROR);
    sp_win32_apply_member_opacity(api,&s,NULL,0,90);check_opacity(keyed,LWA_ALPHA|LWA_COLORKEY,key,90);
    sp_win32_apply_member_opacity(api,&s,keyed,0,90);check_opacity(keyed,LWA_ALPHA|LWA_COLORKEY,key,150);
    sp_win32_apply_member_opacity(api,&s,NULL,0,90);check_opacity(keyed,LWA_ALPHA|LWA_COLORKEY,key,90);
    /* Do not replace the application's new layering state with an old snapshot. */
    COLORREF new_key=RGB(65,43,21);
    CHECK(SetLayeredWindowAttributes(keyed,new_key,165,LWA_ALPHA|LWA_COLORKEY));
    sp_win32_apply_member_opacity(api,&s,keyed,220,90);
    CHECK(sp_release_all(&s).status==SP_OK&&!s.count);
    check_opacity(keyed,LWA_ALPHA|LWA_COLORKEY,new_key,165);

    CHECK(sp_toggle_member(&s,(uintptr_t)keyed,area).status==SP_OK);
    shown=sp_toggle(&s,area);CHECK(shown.status==SP_OK||shown.status==SP_WINDOW_ERROR);
    sp_win32_apply_member_opacity(api,&s,NULL,0,90);check_opacity(keyed,LWA_ALPHA|LWA_COLORKEY,new_key,90);
    LONG_PTR style=GetWindowLongPtrW(keyed,GWL_EXSTYLE);
    CHECK(SetWindowLongPtrW(keyed,GWL_EXSTYLE,style&~(LONG_PTR)WS_EX_LAYERED));
    CHECK(SetWindowLongPtrW(keyed,GWL_EXSTYLE,style));
    set_per_pixel_layer(keyed);check_unreadable_layer(keyed);
    CHECK(sp_release_all(&s).status==SP_OK&&!s.count);
    check_unreadable_layer(keyed);set_per_pixel_layer(keyed);

    CHECK(sp_toggle_member(&s,(uintptr_t)other,area).status==SP_OK);
    shown=sp_toggle(&s,area);CHECK(shown.status==SP_OK||shown.status==SP_WINDOW_ERROR);
    restore_on_layering=other;restore_during_style=&s;
    sp_win32_apply_member_opacity(api,&s,other,220,90);
    restore_on_layering=NULL;restore_during_style=NULL;
    CHECK(!s.visible&&s.count==1&&!layered(other));
    CHECK(sp_release_all(&s).status==SP_OK&&!s.count);
    CHECK(DestroyWindow(plain));CHECK(DestroyWindow(keyed));CHECK(DestroyWindow(unreadable));CHECK(DestroyWindow(other));
}
int main(void) {
    sp_win32_dpi();
    WNDCLASSW cls={0}; cls.lpfnWndProc=proc; cls.hInstance=GetModuleHandleW(NULL);
    cls.lpszClassName=L"CloakSpace.DisposableTest"; CHECK(RegisterClassW(&cls));
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
    CHECK(DestroyWindow(a)); test_member_opacity(&api,ops,&cls,area);
    sp_win32_dispose(&api); sp_win32_dispose(&reject);
    CHECK(UnregisterClassW(cls.lpszClassName,cls.hInstance));
    printf("win32: %u checks passed (only disposable test-owned windows)\n",checks); return 0;
}
