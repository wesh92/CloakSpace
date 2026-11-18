#include "modules.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Monitor bounds are invariant under a 180-degree flip. Model the four Windows
   orientations without changing the operator's display, cursor, focus or keys. */
typedef struct {
    const char *name;
    DWORD rotation;
    RECT monitor,work;
    bool vertical;
} Screen;
static const Screen screens[]={
    {"vertical",DMDO_90,{-1080,-240,0,1680},{-1080,-240,0,1640},true},
    {"vertical-flipped",DMDO_270,{1920,0,3000,1920},{1920,40,3000,1920},true},
    {"horizontal",DMDO_DEFAULT,{0,0,1920,1080},{0,0,1920,1040},false},
    {"horizontal-flipped",DMDO_180,{-1920,-100,0,980},{-1880,-100,0,980},false},
    /* Classification must use the screen, not taskbars or configured fractions. */
    {"horizontal with tall work area",DMDO_DEFAULT,{0,0,1920,1080},{0,0,800,1040},false},
    {"vertical with wide work area",DMDO_90,{0,0,1080,1920},{0,0,1040,900},true},
    {"square",DMDO_DEFAULT,{0,0,1200,1200},{0,0,1200,1160},false}
};
static const Screen *screen;
static unsigned checks,route,anchor;
static SpSplit configured_split;
static DWORD monitor_error;
static bool missing_monitor;
#define CHECK(x) do {++checks;if(!(x)){ \
    fprintf(stderr,"%s:%d: %s (screen=%s rotation=%lu route=%u split=%d anchor=%u)\n", \
        __FILE__,__LINE__,#x,screen?screen->name:"none", \
        screen?(unsigned long)screen->rotation:0,route,(int)configured_split,anchor);exit(1);}}while(0)

static BOOL WINAPI fixture_cursor(LPPOINT p){
    *p=(POINT){screen->monitor.left+10,screen->monitor.top+10};return TRUE;
}
static HMONITOR WINAPI fixture_monitor(POINT p,DWORD flags){
    CHECK(p.x==screen->monitor.left+10&&p.y==screen->monitor.top+10);
    CHECK(flags==MONITOR_DEFAULTTOPRIMARY);return missing_monitor?NULL:(HMONITOR)(UINT_PTR)1;
}
static BOOL WINAPI fixture_info(HMONITOR monitor,LPMONITORINFO info){
    CHECK(monitor==(missing_monitor?NULL:(HMONITOR)(UINT_PTR)1));CHECK(info->cbSize==sizeof(*info));
    if(monitor_error){SetLastError(monitor_error);return FALSE;}
    info->rcMonitor=screen->monitor;info->rcWork=screen->work;return TRUE;
}
static BOOL WINAPI fixture_foreground(HWND h){(void)h;return TRUE;}
static BOOL WINAPI fixture_popup(HMENU menu,UINT flags,int x,int y,int reserved,HWND owner,const RECT *exclude){
    (void)x;(void)y;(void)reserved;(void)owner;(void)exclude;
    CHECK(menu);CHECK(flags&TPM_RETURNCMD);return 1; /* ID_TOGGLE */
}
#define GetCursorPos fixture_cursor
#define MonitorFromPoint fixture_monitor
#define GetMonitorInfoW fixture_info
#define SetForegroundWindow fixture_foreground
#define TrackPopupMenu fixture_popup
/* Exercise the real host entry points, monitor selection, module dispatch and
   core state machine. Only display metadata and window operations are fixtures. */
#include "../src/main.c"
#include "../src/win32.c"
#undef GetCursorPos
#undef MonitorFromPoint
#undef GetMonitorInfoW
#undef SetForegroundWindow
#undef TrackPopupMenu

typedef struct {bool claimed,shown;uintptr_t cookie;SpRect rect;unsigned tiles;} Window;
static struct {Window windows[5];bool surface;} fake;
static SpProfile profile;
static bool fixture_claim(void *ctx,uintptr_t handle,SpWindow *out){
    (void)ctx;CHECK(handle>0&&handle<5);Window *w=&fake.windows[handle];
    w->claimed=true;++w->cookie;*out=(SpWindow){handle,w->cookie,1,1};return true;
}
static bool fixture_alive(void *ctx,SpWindow window){
    (void)ctx;Window *w=&fake.windows[window.handle];return w->claimed&&w->cookie==window.cookie;
}
static bool fixture_hide(void *ctx,SpWindow window){(void)ctx;fake.windows[window.handle].shown=false;return true;}
static bool fixture_tile(void *ctx,SpWindow window,SpRect rect){
    (void)ctx;Window *w=&fake.windows[window.handle];w->rect=rect;w->shown=true;++w->tiles;return true;
}
static bool fixture_release(void *ctx,SpWindow window){
    (void)ctx;Window *w=&fake.windows[window.handle];w->claimed=false;w->shown=true;return true;
}
static bool fixture_focus(void *ctx,SpWindow window){return fixture_alive(ctx,window);}
static bool fixture_surface(void *ctx,const SpState *state,bool active,SpRect work){
    (void)ctx;(void)state;(void)work;fake.surface=active;return true;
}
static SpRect workarea(void){
    RECT r=screen->work;return (SpRect){r.left,r.top,r.right-r.left,r.bottom-r.top};
}
static void rect_equal(SpRect a,SpRect b){CHECK(a.x==b.x);CHECK(a.y==b.y);CHECK(a.w==b.w);CHECK(a.h==b.h);}
static void layout_equal(SpLayout a,SpLayout b){
    CHECK(a.gap==b.gap);CHECK(a.single_w==b.single_w);CHECK(a.single_h==b.single_h);
    CHECK(a.multi_w==b.multi_w);CHECK(a.multi_h==b.multi_h);CHECK(a.split==b.split);
    CHECK(a.mirror_x==b.mirror_x);CHECK(a.mirror_y==b.mirror_y);
}
static SpLayout configured(void){
    return (SpLayout){19,65,80,83,91,configured_split,(anchor&1)!=0,(anchor&2)!=0};
}
static void setup(void){
    memset(&app,0,sizeof(app));memset(&fake,0,sizeof(fake));memset(&profile,0,sizeof(profile));
    profile.name=L"orientation fixture";profile.layout=configured();profile.binding_count=3;
    profile.bindings[0].action=L"scratch.toggle";profile.bindings[1].action=L"scratch.member";
    profile.bindings[2].action=L"scratch.promote";app.profile=&profile;
    SpWindowOps ops={fixture_claim,fixture_alive,fixture_hide,fixture_tile,fixture_release,fixture_focus};
    CHECK(sp_init(&app.scratch,ops,NULL));app.scratch.layout=profile.layout;
    app.scratch.workspace=fixture_surface;
    for(uintptr_t i=1;i<=3;++i)(void)host_proc(NULL,SP_BIND_MESSAGE,1,(LPARAM)i);
    CHECK(app.scratch.count==3&&!app.scratch.visible&&!fake.surface);
    for(size_t i=1;i<=3;++i)CHECK(!fake.windows[i].shown&&!fake.windows[i].tiles);
}
static void toggle(void){
    if(route==0)(void)host_proc(NULL,SP_BIND_MESSAGE,0,0);
    else if(route==1)menu(&app);
    else (void)host_proc(NULL,SP_TRAY_MESSAGE,0,WM_LBUTTONDBLCLK);
}
static void placed(void){
    CHECK(app.scratch.visible&&fake.surface);
    SpLayout effective=configured();if(screen->vertical)effective.split=SP_SPLIT_VERTICAL;
    SpRect expected[SP_CAPACITY];
    CHECK(sp_layout_with(workarea(),app.scratch.count,expected,SP_CAPACITY,effective)==SP_OK);
    for(size_t i=0;i<app.scratch.count;++i){
        Window *w=&fake.windows[app.scratch.windows[i].handle];
        CHECK(w->shown&&w->tiles);rect_equal(w->rect,expected[i]);
    }
    /* Neither the loaded profile nor the core's configured layout may be mutated. */
    layout_equal(profile.layout,configured());layout_equal(app.scratch.layout,configured());
}
static void activation(void){
    const Screen *original=screen;setup();toggle();placed();
    (void)host_proc(NULL,SP_BIND_MESSAGE,2,3);CHECK(app.scratch.windows[0].handle==3);placed();
    (void)host_proc(NULL,SP_BIND_MESSAGE,1,4);CHECK(app.scratch.count==4);placed();
    (void)host_proc(NULL,SP_BIND_MESSAGE,1,4);CHECK(app.scratch.count==3);placed();
    toggle();CHECK(!app.scratch.visible&&!fake.surface);
    /* Re-activation must re-read the screen, not latch the first override. */
    for(size_t i=0;i<sizeof(screens)/sizeof(screens[0]);++i){
        screen=&screens[i];toggle();placed();toggle();CHECK(!app.scratch.visible&&!fake.surface);
    }
    screen=original;CHECK(sp_release_all(&app.scratch).status==SP_OK);CHECK(!app.scratch.count);
}
static void lookup_failure(void){
    for(unsigned missing=0;missing<2;++missing){
        screen=&screens[0];setup();
        monitor_error=missing?ERROR_INVALID_MONITOR_HANDLE:ERROR_ACCESS_DENIED;missing_monitor=missing!=0;
        SpRect work={7,11,13,17};bool vertical=app.scratch.vertical_screen;
        CHECK(!sp_win32_scratch_workarea(&app.api,&app.scratch,&work));
        rect_equal(work,(SpRect){7,11,13,17});CHECK(app.scratch.vertical_screen==vertical);
        wchar_t expected[96];
        CHECK(swprintf(expected,96,L"Cannot read monitor work area (Windows error %lu).",(unsigned long)monitor_error)>0);
        CHECK(!wcscmp(app.api.error,expected));
        toggle();CHECK(!app.scratch.visible&&!fake.surface&&app.scratch.count==3);
        CHECK(!wcscmp(app.api.error,expected));
        for(size_t i=1;i<=3;++i)CHECK(fake.windows[i].claimed&&!fake.windows[i].shown&&!fake.windows[i].tiles);
        layout_equal(app.scratch.layout,configured());layout_equal(profile.layout,configured());
        monitor_error=0;missing_monitor=false;
        screen=&screens[3];toggle();placed();
        CHECK(sp_release_all(&app.scratch).status==SP_OK);
    }
}
static void preflight_cases(void){
    static const Screen portrait_thin={"vertical, only vertical fits",DMDO_90,
        {0,0,1080,1920},{0,0,40,1000},true};
    static const Screen portrait_shallow={"vertical, only horizontal fits",DMDO_90,
        {0,0,1080,1920},{0,0,1000,40},true};
    static const Screen horizontal_shallow={"horizontal shallow work area",DMDO_DEFAULT,
        {0,0,1920,1080},{0,0,1000,40},false};
    screen=&portrait_thin;setup();toggle();placed();
    /* Configured horizontal cannot fit: add/promote must preflight the override. */
    (void)host_proc(NULL,SP_BIND_MESSAGE,1,4);CHECK(app.scratch.count==4);placed();
    (void)host_proc(NULL,SP_BIND_MESSAGE,2,4);CHECK(app.scratch.windows[0].handle==4);placed();
    toggle();CHECK(!app.scratch.visible&&!fake.surface);
    SpLayout invalid=configured();invalid.split=(SpSplit)99;
    app.scratch.layout=profile.layout=invalid;unsigned tiles=fake.windows[1].tiles;
    toggle();CHECK(!app.scratch.visible&&!fake.surface);CHECK(fake.windows[1].tiles==tiles);
    layout_equal(app.scratch.layout,invalid);layout_equal(profile.layout,invalid);
    app.scratch.layout=profile.layout=configured();toggle();placed();
    CHECK(sp_release_all(&app.scratch).status==SP_OK);

    screen=&portrait_shallow;setup();toggle();CHECK(!app.scratch.visible&&!fake.surface);
    for(size_t i=1;i<=3;++i)CHECK(!fake.windows[i].tiles);
    screen=&horizontal_shallow;toggle();placed();
    screen=&portrait_shallow;tiles=fake.windows[1].tiles;
    (void)host_proc(NULL,SP_BIND_MESSAGE,2,3);CHECK(app.scratch.windows[0].handle==1);
    (void)host_proc(NULL,SP_BIND_MESSAGE,1,4);CHECK(app.scratch.count==3&&!fake.windows[4].claimed);
    CHECK(fake.windows[1].tiles==tiles); /* No partial reorder, claim or placement. */
    layout_equal(app.scratch.layout,configured());layout_equal(profile.layout,configured());
    CHECK(sp_release_all(&app.scratch).status==SP_OK);
}
int main(void){
    for(route=0;route<3;++route)
        for(configured_split=SP_SPLIT_LONGEST;configured_split<=SP_SPLIT_ALTERNATE;++configured_split)
            for(anchor=0;anchor<4;++anchor)
                for(size_t i=0;i<sizeof(screens)/sizeof(screens[0]);++i){screen=&screens[i];activation();}
    configured_split=SP_SPLIT_HORIZONTAL;anchor=0;
    for(route=0;route<3;++route){lookup_failure();preflight_cases();}
    printf("screen orientation: %u checks passed (host activation/retile; injected monitors, no display or user-window changes)\n",checks);
    return 0;
}
