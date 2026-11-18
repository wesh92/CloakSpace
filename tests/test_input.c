#include "hotkeys.h"
#include "modules.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static unsigned checks;
#define CHECK(x) do { ++checks; if (!(x)) { \
    fprintf(stderr,"%s:%d: %s\n",__FILE__,__LINE__,#x); exit(1); } } while(0)
static SpConfig cfg;
static unsigned semantic_notices,semantic_jobs;
static void semantic_notice(void *host,const wchar_t *message) {(void)host;CHECK(message&&*message);++semantic_notices;}
static bool semantic_background(void *host,const SpBinding *binding) {(void)host;CHECK(binding);++semantic_jobs;return true;}
static void test_launch_validation(void) {
    static SpConfig semantic;
    const wchar_t *bad[]={
        L"bind Ctrl+Alt+U terminal.quake \"\"",
        L"bind Ctrl+Alt+U process.exec \"\"",
        L"bind Ctrl+Alt+U terminal.quake \"Dev;Tools\"",
        L"bind Ctrl+Alt+U terminal.quake Dev tool.exe \"a;b\"",
        L"bind Ctrl+Alt+U terminal.quake Dev tool.exe \"\"",
        L"bind Ctrl+Alt+U terminal.quake Dev \"\"",
        L"bind Ctrl+Alt+U terminal.quake Dev tool.exe \"a\\\"b\"",
        L"terminal-profile \"Dev;Tools\"\nbind Ctrl+Alt+U scratch.toggle",
        L"quake-window \"One;Two\"\nbind Ctrl+Alt+U scratch.toggle",
        L"container-profile \"Dev;Tools\"\nbind Ctrl+Alt+U scratch.toggle",
        L"container-directory .\nbind Ctrl+Alt+U scratch.toggle",
        L"container-directory ..\nbind Ctrl+Alt+U scratch.toggle",
        L"container-directory app\nbind Ctrl+Alt+U scratch.toggle",
        L"container-directory ../app\nbind Ctrl+Alt+U scratch.toggle",
        L"container-directory C:/app\nbind Ctrl+Alt+U scratch.toggle",
        L"container-directory \"C:\\\\app\"\nbind Ctrl+Alt+U scratch.toggle",
        L"container-directory \"/app;other\"\nbind Ctrl+Alt+U scratch.toggle",
        L"container-directory \"/app\\\"quote\"\nbind Ctrl+Alt+U scratch.toggle",
        L"bind Ctrl+Alt+U terminal.container \"\"",
        L"bind Ctrl+Alt+U terminal.container /bin/sh \"a;b\"",
        L"bind Ctrl+Alt+U terminal.container /bin/sh \"\""};
    wchar_t text[1024],error[256];
    for (size_t i=0;i<sizeof(bad)/sizeof(bad[0]);++i) {
        swprintf(text,1024,L"profile default\n%ls\nend\n",bad[i]);
        CHECK(sp_config_parse(&semantic,text,error,256));
        CHECK(!sp_modules_validate(&semantic.profiles[0],error,256));CHECK(*error);
    }
    const wchar_t *directories[]={L"/",L"/app/shared files",L"/app/snow-\u96ea files"};
    for (size_t i=0;i<sizeof(directories)/sizeof(directories[0]);++i) {
        swprintf(text,1024,L"profile default\ncontainer-directory \"%ls\"\nbind Ctrl+Alt+U terminal.container\nend\n",directories[i]);
        CHECK(sp_config_parse(&semantic,text,error,256));
        CHECK(!wcscmp(semantic.profiles[0].container_directory,directories[i]));
        CHECK(sp_modules_validate(&semantic.profiles[0],error,256));
    }
    CHECK(sp_config_parse(&semantic,L"profile default\nbind Ctrl+Alt+U process.exec tool.exe \"\" \"a;b\" \"a\\\"b\"\nend\n",error,256));
    CHECK(sp_modules_validate(&semantic.profiles[0],error,256));
    SpModuleContext context={0};context.profile=&semantic.profiles[0];
    context.notify=semantic_notice;context.background=semantic_background;
    SpBinding binding={0};binding.action=L"terminal.quake";binding.argc=1;binding.args[0]=L"";
    CHECK(!sp_modules_run(&context,&binding));CHECK(semantic_jobs==0 && semantic_notices==1);
    binding.action=L"process.exec";
    CHECK(!sp_modules_run(&context,&binding));CHECK(semantic_jobs==0 && semantic_notices==2);
    binding.action=L"terminal.container";
    CHECK(!sp_modules_run(&context,&binding));CHECK(semantic_jobs==0 && semantic_notices==3);
    CHECK(sp_modules_run(&context,&semantic.profiles[0].bindings[0]));CHECK(semantic_jobs==1 && semantic_notices==3);
    binding.argc=0;CHECK(sp_modules_run(&context,&binding));CHECK(semantic_jobs==2);
    binding.argc=SP_MAX_ARGS;
    for (size_t i=0;i<binding.argc;++i) binding.args[i]=i?L"arg":L"/bin/sh";
    CHECK(sp_modules_run(&context,&binding));CHECK(semantic_jobs==3);
    binding.argc=SP_MAX_ARGS+1;CHECK(!sp_modules_run(&context,&binding));CHECK(semantic_jobs==3 && semantic_notices==4);
}

/* Delay the worker before its message queue exists. No OS hook/input is used. */
static SpKeyboard delayed_keyboard;
static HANDLE startup_gate, observed_thread;
static LPTHREAD_START_ROUTINE delayed_entry;
static void *delayed_context;
static bool finite_join;
static unsigned startup_hook_installs;
static bool neutral_startup, neutral_hook_installed;
static HOOKPROC neutral_proc;
static unsigned neutral_posts, neutral_consumed, neutral_preinstall_reads;
static unsigned neutral_scenario, neutral_waits;
static void neutral_key(unsigned vk,unsigned scan,bool down);
static DWORD WINAPI delayed_worker(void *unused) {
    (void)unused;
    WaitForSingleObject(startup_gate,INFINITE);
    return delayed_entry(delayed_context);
}
static HANDLE WINAPI delayed_create_thread(LPSECURITY_ATTRIBUTES sa,SIZE_T stack,
        LPTHREAD_START_ROUTINE entry,void *ctx,DWORD flags,LPDWORD id) {
    delayed_entry=entry; delayed_context=ctx;
    HANDLE h=CreateThread(sa,stack,delayed_worker,NULL,flags,id);
    CHECK(h && DuplicateHandle(GetCurrentProcess(),h,GetCurrentProcess(),&observed_thread,
        0,FALSE,DUPLICATE_SAME_ACCESS));
    return h;
}
static DWORD WINAPI startup_wait(HANDLE h,DWORD ms) {
    if (h==delayed_keyboard.ready && ms==3000) return WAIT_TIMEOUT;
    if (h==delayed_keyboard.thread) {
        finite_join=ms!=INFINITE;
        SetEvent(startup_gate);
        /* Always join in this harness so even the broken version cannot outlive storage. */
        return WaitForSingleObject(h,INFINITE);
    }
    return WaitForSingleObject(h,ms);
}
static HHOOK WINAPI no_hook_install(int id,HOOKPROC proc,HINSTANCE module,DWORD thread) {
    (void)id; (void)module; (void)thread;
    if (neutral_startup) { neutral_hook_installed=true; neutral_proc=proc; }
    ++startup_hook_installs; return (HHOOK)(UINT_PTR)1;
}
static BOOL WINAPI no_hook_uninstall(HHOOK hook) { (void)hook; return TRUE; }
BOOL WINAPI no_hook_messages(LPMSG msg,HWND hwnd,UINT first,UINT last) {
    (void)msg; (void)hwnd; (void)first; (void)last; return 0;
}
static SHORT WINAPI no_async_state(int key) {
    if (!neutral_startup) return 0;
    /* Win was held before installation, then released without a hook event.
       The low bit is deliberately set after release; only the high bit counts. */
    if (!neutral_hook_installed) {
        ++neutral_preinstall_reads;
        return key==VK_LWIN?(SHORT)0x8000:0;
    }
    if (key==1 && neutral_waits==0) {
        /* Model reentrant hook delivery while the first async poll is running. */
        if (neutral_scenario==3) {
            neutral_key(VK_LWIN,0x15b,true);
            neutral_key('S',0x1f,true);
            neutral_key('S',0x1f,false);
            neutral_key(VK_LWIN,0x15b,false);
        } else if (neutral_scenario==4) neutral_key('S',0x1f,true);
    }
    return neutral_scenario==2 && neutral_waits==0 && key==VK_LWIN?(SHORT)0x8000:1;
}
static BOOL WINAPI neutral_peek(LPMSG msg,HWND hwnd,UINT first,UINT last,UINT remove) {
    if (neutral_startup) return FALSE;
    return PeekMessageW(msg,hwnd,first,last,remove);
}
static void neutral_key(unsigned vk,unsigned scan,bool down) {
    KBDLLHOOKSTRUCT key={0}; key.vkCode=vk; key.scanCode=scan&0xff;
    key.flags=scan&0x100?LLKHF_EXTENDED:0;
    /* Direct callback replay only: never post or synthesize OS keyboard input. */
    neutral_consumed+=neutral_proc(HC_ACTION,down?WM_KEYDOWN:WM_KEYUP,(LPARAM)&key)!=0;
}
static DWORD WINAPI neutral_wait(DWORD count,const HANDLE *handles,BOOL all,DWORD ms,DWORD wake) {
    if (!neutral_startup) return MsgWaitForMultipleObjects(count,handles,all,ms,wake);
    ++neutral_waits;
    if (neutral_scenario!=1 && neutral_waits==1) {
        CHECK(ms==25);
        if (neutral_scenario==2) {
            neutral_key('S',0x1f,true);
            neutral_key(VK_LWIN,0x15b,false);
            neutral_key('S',0x1f,false);
        }
        CHECK(neutral_posts==0 && neutral_consumed==0);
        return WAIT_TIMEOUT;
    }
    if (neutral_scenario==4 && neutral_waits==2) {
        /* A second all-up poll has unchanged epoch but an observed held slot. */
        CHECK(ms==25);
        neutral_key('S',0x1f,false);
        CHECK(neutral_posts==0 && neutral_consumed==0);
        return WAIT_OBJECT_0+1;
    }
    CHECK(ms==INFINITE);
    if (neutral_scenario!=1) neutral_key(VK_LWIN,0x15b,true);
    neutral_key('S',0x1f,true);
    neutral_key('S',0x1f,false);
    if (neutral_scenario!=1) neutral_key(VK_LWIN,0x15b,false);
    return WAIT_OBJECT_0;
}
static LRESULT WINAPI neutral_next(HHOOK hook,int code,WPARAM wp,LPARAM lp) {
    (void)hook; (void)code; (void)wp; (void)lp; return 0;
}
static BOOL WINAPI neutral_post(HWND hwnd,UINT msg,WPARAM wp,LPARAM lp) {
    (void)hwnd; (void)msg; (void)wp; (void)lp; ++neutral_posts; return TRUE;
}
static HWND WINAPI neutral_foreground(void) { return NULL; }
static UINT WINAPI neutral_send(UINT count,LPINPUT input,int size) {
    (void)input; (void)size; return count;
}
void delayed_keyboard_stop(SpKeyboard *keyboard);
#define sp_keyboard_start delayed_keyboard_start
#define sp_keyboard_stop delayed_keyboard_stop
#define CreateThread delayed_create_thread
#define WaitForSingleObject startup_wait
#define SetWindowsHookExW no_hook_install
#define UnhookWindowsHookEx no_hook_uninstall
#define GetMessageW no_hook_messages
#define GetAsyncKeyState no_async_state
#define PeekMessageW neutral_peek
#define MsgWaitForMultipleObjects neutral_wait
#define CallNextHookEx neutral_next
#define PostMessageW neutral_post
#define GetForegroundWindow neutral_foreground
#define SendInput neutral_send
#include "../src/hotkeys.c"
#undef sp_keyboard_start
#undef sp_keyboard_stop
#undef CreateThread
#undef WaitForSingleObject
#undef SetWindowsHookExW
#undef UnhookWindowsHookEx
#undef GetMessageW
#undef GetAsyncKeyState
#undef PeekMessageW
#undef MsgWaitForMultipleObjects
#undef CallNextHookEx
#undef PostMessageW
#undef GetForegroundWindow
#undef SendInput

static void test_neutral_thread(const SpProfile *p,unsigned scenario) {
    SpKeyboard keyboard={0}; keyboard.profile=p;
    keyboard.ready=CreateEventW(NULL,TRUE,FALSE,NULL);
    keyboard.cancel=CreateEventW(NULL,TRUE,FALSE,NULL);
    CHECK(keyboard.ready && keyboard.cancel);
    neutral_startup=true; neutral_scenario=scenario; neutral_hook_installed=false;
    neutral_posts=neutral_consumed=neutral_preinstall_reads=neutral_waits=0;
    CHECK(keyboard_thread(&keyboard)==0);
    neutral_startup=false;
    CHECK(neutral_posts==(scenario==1?0u:1u) && neutral_consumed==(scenario==1?0u:2u));
    CHECK(neutral_preinstall_reads==0 && keyboard.error==0);
    CHECK(keyboard.gate.armed && neutral_waits==(scenario==1?1u:scenario==4?3u:2u));
    delayed_keyboard_stop(&keyboard);
}

static void gate_pass(SpKeyGate *g,SpKeyState *k,const SpProfile *p,
        unsigned vk,unsigned scan,bool down) {
    SpKeyDecision d=sp_key_gate_event(g,k,p,vk,scan,down,false);
    CHECK(!d.suppress && d.binding==SIZE_MAX);
}
static void gate_fresh_chord(SpKeyGate *g,SpKeyState *k,const SpProfile *p) {
    CHECK(g->armed);
    gate_pass(g,k,p,VK_LWIN,0x15b,true);
    SpKeyDecision d=sp_key_gate_event(g,k,p,'S',0x1f,true,false);
    CHECK(d.suppress && d.binding==0);
    d=sp_key_gate_event(g,k,p,'S',0x1f,true,false);
    CHECK(d.suppress && d.binding==SIZE_MAX);
    d=sp_key_gate_event(g,k,p,'S',0x1f,false,false);
    CHECK(d.suppress && d.binding==SIZE_MAX);
    gate_pass(g,k,p,VK_LWIN,0x15b,false);
}
static void test_default_keymap(void) {
    static SpConfig defaults;sp_config_defaults(&defaults);
    const SpProfile *p=&defaults.profiles[0];
    static const struct {unsigned vk,scan,mods;const wchar_t *action;} cases[]={
        {'S',0x1f,0,L"scratch.toggle"},
        {'S',0x1f,SP_MOD_SHIFT,L"scratch.member"},
        {'P',0x19,0,L"scratch.promote"},
        {'R',0x13,0,L"scratch.restore"},
        {0xde,0x29,0,L"terminal.open"}};
    const unsigned extras[]={0,SP_MOD_WIN,SP_MOD_SHIFT,SP_MOD_WIN|SP_MOD_SHIFT};
    CHECK(p->binding_count==sizeof(cases)/sizeof(cases[0]));
    for (size_t i=0;i<sizeof(cases)/sizeof(cases[0]);++i) {
        CHECK(!wcscmp(p->bindings[i].action,cases[i].action) && !p->bindings[i].argc);
        for (size_t e=0;e<sizeof(extras)/sizeof(extras[0]);++e) {
            if (extras[e]&cases[i].mods) continue; /* Only add absent modifiers. */
            SpKeyState k={0};unsigned mods=cases[i].mods|extras[e];
            CHECK(!sp_key_event(&k,p,VK_LCONTROL,0x1d,true,false).suppress);
            CHECK(!sp_key_event(&k,p,VK_LMENU,0x38,true,false).suppress);
            if (mods&SP_MOD_SHIFT) CHECK(!sp_key_event(&k,p,VK_LSHIFT,0x2a,true,false).suppress);
            if (mods&SP_MOD_WIN) CHECK(!sp_key_event(&k,p,VK_LWIN,0x15b,true,false).suppress);
            CHECK(sp_key_modifiers(&k)==(SP_MOD_CTRL|SP_MOD_ALT|mods));
            /* Shift+S is deliberately another default action; other extras miss. */
            size_t expected=!extras[e]?i:extras[e]==SP_MOD_SHIFT && i==0?1:SIZE_MAX;
            bool suppress=expected!=SIZE_MAX;
            SpKeyDecision d=sp_key_event(&k,p,cases[i].vk,cases[i].scan,true,false);
            CHECK(d.suppress==suppress && d.binding==expected);
            d=sp_key_event(&k,p,cases[i].vk,cases[i].scan,true,false);
            CHECK(d.suppress==suppress && d.binding==SIZE_MAX);
            d=sp_key_event(&k,p,cases[i].vk,cases[i].scan,false,false);
            CHECK(d.suppress==suppress && d.binding==SIZE_MAX);
        }
    }
}
static void test_startup_modifiers(const SpProfile *p) {
    const unsigned held[]={VK_LWIN,VK_RWIN,VK_LCONTROL,VK_RCONTROL,
        VK_LSHIFT,VK_RSHIFT,VK_LMENU,VK_RMENU};
    const unsigned scans[]={0x15b,0x15c,0x1d,0x11d,0x2a,0x36,0x38,0x138};
    const SpKeyState zero={0};
    for (size_t i=0;i<sizeof(held)/sizeof(held[0]);++i) {
        SpKeyState k={0}; SpKeyGate g={0}; uint64_t epoch=g.epoch;
        /* The modifier down predated the hook; async polling sees it held. */
        CHECK(!sp_key_gate_poll(&g,&k,epoch,false));
        CHECK(!g.armed && !memcmp(&k,&zero,sizeof(k)));
        gate_pass(&g,&k,p,'S',0x1f,true);
        gate_pass(&g,&k,p,'S',0x1f,false);
        gate_pass(&g,&k,p,held[i],scans[i],false);
        CHECK(!sp_key_gate_poll(&g,&k,epoch,true));
        CHECK(sp_key_gate_poll(&g,&k,g.epoch,true));
        CHECK(!memcmp(&k,&zero,sizeof(k)) && sp_key_modifiers(&k)==0);
        /* No startup modifier can remain latched after its release. */
        gate_pass(&g,&k,p,'S',0x1f,true);
        gate_pass(&g,&k,p,'S',0x1f,false);
        gate_fresh_chord(&g,&k,p);
    }
}
static void test_startup_scan_alias(const SpProfile *p) {
    SpKeyState k={0}; SpKeyGate g={0}; const SpKeyState zero={0};
    /* Scan029's initial VK_C0 down happened before installation, so no VK
       snapshot exists. Its first observed repeat can already be VK_DE. */
    CHECK(!sp_key_gate_poll(&g,&k,g.epoch,false));
    CHECK(!k.down[0xc0] && !k.pressed[512+0xc0]);
    gate_pass(&g,&k,p,VK_LWIN,0x15b,true);
    gate_pass(&g,&k,p,VK_LSHIFT,0x2a,true);
    gate_pass(&g,&k,p,0xde,0x29,true);
    CHECK(k.pressed[0x29] && !sp_key_gate_poll(&g,&k,g.epoch,true));
    gate_pass(&g,&k,p,0xc0,0x29,true);
    gate_pass(&g,&k,p,0xc0,0x29,false);
    gate_pass(&g,&k,p,VK_LSHIFT,0x2a,false);
    gate_pass(&g,&k,p,VK_LWIN,0x15b,false);
    CHECK(sp_key_gate_poll(&g,&k,g.epoch,true));
    CHECK(!memcmp(&k,&zero,sizeof(k)));
    gate_pass(&g,&k,p,VK_LWIN,0x15b,true);
    gate_pass(&g,&k,p,VK_LSHIFT,0x2a,true);
    SpKeyDecision d=sp_key_gate_event(&g,&k,p,0xde,0x29,true,false);
    CHECK(d.suppress && d.binding==4);
    d=sp_key_gate_event(&g,&k,p,0xc0,0x29,false,false);
    CHECK(d.suppress && d.binding==SIZE_MAX);
}
static void test_neutral_poll_interleaving(const SpProfile *p) {
    SpKeyState k={0}; SpKeyGate g={0}; uint64_t epoch=g.epoch;
    /* A down/up pair can finish during the outside-callback async poll. */
    gate_pass(&g,&k,p,'S',0x1f,true);
    gate_pass(&g,&k,p,'S',0x1f,false);
    CHECK(g.epoch==epoch+2 && !k.pressed[0x1f]);
    CHECK(!sp_key_gate_poll(&g,&k,epoch,true) && !g.armed);
    CHECK(sp_key_gate_poll(&g,&k,g.epoch,true));
    gate_fresh_chord(&g,&k,p);

    k=(SpKeyState){0}; g=(SpKeyGate){0};
    gate_pass(&g,&k,p,'S',0x1f,true); epoch=g.epoch;
    gate_pass(&g,&k,p,'S',0x1f,false);
    CHECK(!sp_key_gate_poll(&g,&k,epoch,true)); /* Up-only interleaving. */
    CHECK(!sp_key_gate_poll(&g,&k,g.epoch,false));
    CHECK(sp_key_gate_poll(&g,&k,g.epoch,true));

    k=(SpKeyState){0}; g=(SpKeyGate){0}; epoch=g.epoch;
    SpKeyDecision d=sp_key_gate_event(&g,&k,p,'S',0x1f,true,true);
    CHECK(!d.suppress && d.binding==SIZE_MAX && g.epoch==epoch && !k.pressed[0x1f]);
    CHECK(sp_key_gate_poll(&g,&k,epoch,true)); /* Injected input cannot hold the gate. */

    k=(SpKeyState){0}; g=(SpKeyGate){0};
    k.down[VK_LWIN]=true; k.swallowed[0x1f]=true;
    CHECK(sp_key_gate_poll(&g,&k,g.epoch,true));
    const SpKeyState zero={0}; CHECK(!memcmp(&k,&zero,sizeof(k)));
    gate_fresh_chord(&g,&k,p); /* Arming clears the entire reducer, not just pressed. */
}
static void test_stale_all_up_poll(const SpProfile *p) {
    SpKeyState k={0}; SpKeyGate g={0};
    /* Callback down passed before this poll began; its async high bit is still
       stale/all-up. Matching epoch alone must NOT allow the gate to arm. */
    gate_pass(&g,&k,p,'S',0x1f,true);
    uint64_t epoch=g.epoch;
    CHECK(!sp_key_gate_poll(&g,&k,epoch,true) && !g.armed && k.pressed[0x1f]);
    gate_pass(&g,&k,p,VK_LWIN,0x15b,true);
    gate_pass(&g,&k,p,'S',0x1f,true);
    CHECK(!sp_key_gate_poll(&g,&k,g.epoch,true));
    gate_pass(&g,&k,p,'S',0x1f,false);
    CHECK(!sp_key_gate_poll(&g,&k,g.epoch,true)); /* Win remains observed down. */
    gate_pass(&g,&k,p,VK_LWIN,0x15b,false);
    CHECK(sp_key_gate_poll(&g,&k,g.epoch,true));
    gate_fresh_chord(&g,&k,p);

    /* Scanless events use a bounded VK slot, with the same all-up guard. */
    k=(SpKeyState){0}; g=(SpKeyGate){0};
    gate_pass(&g,&k,p,'S',0,true);
    CHECK(k.pressed[512+'S'] && !sp_key_gate_poll(&g,&k,g.epoch,true));
    gate_pass(&g,&k,p,'S',0,false);
    CHECK(sp_key_gate_poll(&g,&k,g.epoch,true));
}

int main(void) {
    test_default_keymap();
    test_launch_validation();
    /* Explicit Win-key fixtures exercise the reducer/mask independently of the
       public default keymap. The real OS hook below still uses an empty map. */
    wchar_t input_error[256];
    CHECK(sp_config_parse(&cfg,L"profile default\nbind Win+S scratch.toggle\n"
        L"bind Win+Shift+S scratch.member\nbind Win+Alt+S scratch.promote\n"
        L"bind Win+Ctrl+Shift+S scratch.restore\nbind Win+Shift+Scan029 terminal.open\nend\n",input_error,256));
    const SpProfile *p=&cfg.profiles[0]; SpKeyState k={0};
    test_startup_modifiers(p);
    test_startup_scan_alias(p);
    test_neutral_poll_interleaving(p);
    test_stale_all_up_poll(p);
    /* An app-visible initial down must never turn into a swallowed repeat/up. */
    CHECK(!sp_key_event(&k,p,'S',0x1f,true,false).suppress);
    sp_key_event(&k,p,0x5b,0x15b,true,false);
    SpKeyDecision repeat=sp_key_event(&k,p,'S',0x1f,true,false);
    CHECK(!repeat.suppress && repeat.binding==SIZE_MAX);
    CHECK(!sp_key_event(&k,p,'S',0x1f,false,false).suppress);
    CHECK(sp_key_event(&k,p,'S',0x1f,true,false).suppress);
    CHECK(sp_key_event(&k,p,'S',0x1f,false,false).suppress);
    memset(&k,0,sizeof(k));
    /* A layout/modifier change may alter the VK while a physical scan stays held. */
    CHECK(!sp_key_event(&k,p,0xc0,0x29,true,false).suppress);
    sp_key_event(&k,p,0x5b,0x15b,true,false);
    sp_key_event(&k,p,0xa0,0x2a,true,false);
    repeat=sp_key_event(&k,p,0xde,0x29,true,false);
    CHECK(!repeat.suppress && repeat.binding==SIZE_MAX);
    CHECK(!sp_key_event(&k,p,0xde,0x29,false,false).suppress);
    memset(&k,0,sizeof(k));

    CHECK(!sp_key_event(&k,p,0x5b,0x15b,true,false).suppress);
    SpKeyDecision d=sp_key_event(&k,p,'S',0x1f,true,false);
    CHECK(d.suppress && d.binding==0);
    d=sp_key_event(&k,p,'S',0x1f,true,false); CHECK(d.suppress && d.binding==SIZE_MAX);
    CHECK(!sp_key_event(&k,p,0x5b,0x15b,false,false).suppress);
    CHECK(sp_key_event(&k,p,'S',0x1f,false,false).suppress);
    CHECK(!sp_key_event(&k,p,'S',0x1f,true,false).suppress);
    sp_key_event(&k,p,'S',0x1f,false,false);
    sp_key_event(&k,p,0x5b,0x15b,true,false); sp_key_event(&k,p,0xa0,0x2a,true,false);
    d=sp_key_event(&k,p,0xde,0x29,true,false); CHECK(d.suppress && d.binding==4);
    CHECK(sp_key_event(&k,p,0xde,0x29,false,false).suppress);
    CHECK(!sp_key_event(&k,p,'S',0x1f,true,true).suppress); /* Ignore injected input. */
    CHECK(!k.down['S']);
    wchar_t error[256]; CHECK(sp_modules_validate(p,error,256));
    SpProfile bad=*p; bad.bindings[0].action=L"missing.module"; CHECK(!sp_modules_validate(&bad,error,256));
    bad=*p; bad.bindings[0].argc=1; CHECK(!sp_modules_validate(&bad,error,256));
    size_t n=0; CHECK(sp_actions(&n) && n>=11);
    /* Install the real hook with an empty keymap: it cannot intercept user shortcuts. */
    static const SpProfile empty={0}; SpKeyboard keyboard;
    startup_gate=CreateEventW(NULL,TRUE,FALSE,NULL); CHECK(startup_gate);
    CHECK(!delayed_keyboard_start(&delayed_keyboard,NULL,WM_APP+40,&empty));
    CHECK(WaitForSingleObject(observed_thread,0)==WAIT_OBJECT_0);
    CloseHandle(observed_thread); CloseHandle(startup_gate);
    CHECK(!finite_join && startup_hook_installs==0);
    CHECK(delayed_keyboard.error==ERROR_TIMEOUT);
    CHECK(!delayed_keyboard.thread && !delayed_keyboard.ready && !delayed_keyboard.cancel);
    delayed_keyboard_stop(&delayed_keyboard);
    test_neutral_thread(p,1); /* Held modifier released between old snapshot and install. */
    test_neutral_thread(p,2); /* Held at install: timeout poll, release, then fresh chord. */
    test_neutral_thread(p,3); /* Down/up reentry during polling invalidates the epoch. */
    test_neutral_thread(p,4); /* Stale async all-up cannot override an observed down. */
    CHECK(sp_keyboard_start(&keyboard,NULL,WM_APP+40,&empty));
    sp_keyboard_stop(&keyboard); CHECK(!keyboard.thread && !keyboard.ready && !keyboard.cancel);
    printf("input/modules: %u checks passed (real hook installed with empty keymap)\n",checks);return 0;
}
