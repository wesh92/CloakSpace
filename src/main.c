#include "host.h"
#include "config_file.h"
#include "hotkeys.h"
#include "modules.h"
#include "overlay.h"
#include "terminals.h"
#include <shellapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>

#define ID_TOGGLE 1
#define ID_RESTORE 2
#define ID_EXIT 3

#define SP_HOST_JOB_TIMER 1u
#define SP_HOST_HIDE_TIMER 2u
#define SP_HOST_HIDE_POLL_MS 25u
/* Fade is at most 500 ms; allow headroom for the overlay thread to acknowledge. */
#define SP_HOST_HIDE_TIMEOUT_MS 2000u

typedef struct {
    HWND window;
    HANDLE mutex,cancel,worker;
    UINT control,taskbar_created;
    NOTIFYICONDATAW tray;
    SpConfig config;
    const SpProfile *profile;
    SpState scratch;
    SpWin32 api;
    SpOverlay *overlay;
    ULONGLONG hide_deadline;
    SpKeyboard keyboard;
    const SpBinding *job_binding;
    wchar_t job_message[1024];
    bool job_ok,closing,has_tray;
} App;
static App app;

static void stop_hide_watchdog(App *a) {
    if (!a->hide_deadline) return;
    a->hide_deadline=0;
    (void)KillTimer(a->window,SP_HOST_HIDE_TIMER);
}
static bool workspace_failure(App *a) {
    swprintf(a->api.error,256,L"%ls",sp_overlay_error(a->overlay));return false;
}
static bool workspace(void *ctx,const SpState *state,bool active,SpRect work) {
    App *a=ctx;
    stop_hide_watchdog(a);
    return sp_overlay_workspace(a->overlay,state,active,work) || workspace_failure(a);
}
static bool hide_workspace(void *ctx,const SpState *state,uintptr_t *request) {
    App *a=ctx;
    /* Arm before requesting a fade so timer failure cannot supersede its result. */
    if (!SetTimer(a->window,SP_HOST_HIDE_TIMER,SP_HOST_HIDE_POLL_MS,NULL)) {
        *request=0;
        return workspace(a,state,false,(SpRect){0});
    }
    a->hide_deadline=GetTickCount64()+SP_HOST_HIDE_TIMEOUT_MS;
    SpOverlayHideResult result=sp_overlay_hide(a->overlay,state,request);
    if (result==SP_OVERLAY_HIDE_PENDING) return true;
    stop_hide_watchdog(a);
    return result==SP_OVERLAY_HIDE_DONE || workspace_failure(a);
}

static void output(const wchar_t *text,bool error) {
    DWORD which=error?STD_ERROR_HANDLE:STD_OUTPUT_HANDLE;
    HANDLE h=GetStdHandle(which);
    if (!h || h==INVALID_HANDLE_VALUE) { AttachConsole(ATTACH_PARENT_PROCESS); h=GetStdHandle(which); }
    if (!h||h==INVALID_HANDLE_VALUE) return;
    DWORD written=0,mode;
    if (GetConsoleMode(h,&mode)) { WriteConsoleW(h,text,(DWORD)wcslen(text),&written,NULL); return; }
    int n=WideCharToMultiByte(CP_UTF8,0,text,-1,NULL,0,NULL,NULL);
    char *utf8=n>0?malloc((size_t)n):NULL;
    if (utf8) {
        WideCharToMultiByte(CP_UTF8,0,text,-1,utf8,n,NULL,NULL);
        WriteFile(h,utf8,(DWORD)n-1,&written,NULL); free(utf8);
    }
}
static void notify(void *ctx,const wchar_t *message) {
    App *a=ctx; OutputDebugStringW(message);
    if (!a->has_tray) { output(message,true); output(L"\r\n",true); return; }
    a->tray.uFlags=NIF_INFO; a->tray.dwInfoFlags=NIIF_WARNING;
    wcsncpy(a->tray.szInfo,message,sizeof(a->tray.szInfo)/sizeof(wchar_t)-1);
    a->tray.szInfo[sizeof(a->tray.szInfo)/sizeof(wchar_t)-1]=0;
    wcscpy(a->tray.szInfoTitle,L"CloakSpace");
    Shell_NotifyIconW(NIM_MODIFY,&a->tray);
}
static void report(void *ctx,SpResult r) {
    App *a=ctx; wchar_t message[512];
    if (r.status==SP_OK||r.status==SP_NOOP) return;
    if (r.status==SP_TOO_SMALL) notify(a,L"This layout cannot fit positive-sized tiles. Release members, reduce the gap, or change layout settings. No partial geometry was applied.");
    else if (r.status==SP_TOO_MANY) notify(a,L"The scratchpad is limited to 64 members. Release some before adding more.");
    else if(r.status==SP_BAD_ARGUMENT) notify(a,L"Invalid layout/work area. Restore all is still available from the tray or --restore.");
    else {
        swprintf(message,512,L"%zu operation(s) failed. %ls Failed releases remain owned; retry Restore all.",r.failed,a->api.error);
        notify(a,message);
    }
}
static void check_hide(App *a,uintptr_t request) {
    if (!request || request!=a->scratch.hide_request) return;
    bool removed;
    if (sp_overlay_hide_completion(a->overlay,request,&removed)) {
        if (!removed) (void)workspace_failure(a);
    } else {
        /* Queued ticks use the current deadline. On expiry, leave the native
           request alone and finish as failed so late results cannot hide members. */
        if (!a->hide_deadline || GetTickCount64()<a->hide_deadline) return;
        wcscpy(a->api.error,L"Workspace deactivation was not acknowledged; the input surface may still be active.");
        removed=false;
    }
    stop_hide_watchdog(a);
    report(a,sp_finish_hide(&a->scratch,request,removed));
}
static DWORD WINAPI job_thread(void *ctx) {
    App *a=ctx;
    a->job_ok=sp_terminal_run(a->profile,a->job_binding,a->cancel,a->job_message,1024);
    PostMessageW(a->window,SP_JOB_MESSAGE,0,0); return 0;
}
static bool background(void *ctx,const SpBinding *binding) {
    App *a=ctx;
    if (a->worker) {
        notify(a,L"A launch/discovery operation is already running; try again after it finishes."); return false;
    }
    ResetEvent(a->cancel); a->job_binding=binding; a->job_message[0]=0;
    a->worker=CreateThread(NULL,0,job_thread,a,0,NULL);
    if (!a->worker) { notify(a,L"Cannot create the launch worker thread."); return false; }
    return true;
}
static void finish_job(App *a) {
    if (!a->worker || WaitForSingleObject(a->worker,0)!=WAIT_OBJECT_0) return;
    CloseHandle(a->worker); a->worker=NULL;
    if (!a->closing && a->job_message[0]) notify(a,a->job_message);
}
static bool begin_close(App *a) {
    if (a->closing) return true;
    SpResult restored=sp_release_all(&a->scratch);report(a,restored);
    if ((restored.status!=SP_OK && restored.status!=SP_NOOP) || a->scratch.count || a->scratch.visible) {
        notify(a,L"Exit cancelled: workspace removal or window release was not confirmed. Let them respond, then retry Restore all / Exit."); return false;
    }
    a->closing=true; SetEvent(a->cancel); sp_keyboard_stop(&a->keyboard);
    finish_job(a);
    if (!a->worker) DestroyWindow(a->window);
    else SetTimer(a->window,SP_HOST_JOB_TIMER,25,NULL);
    return true;
}
static LRESULT status(const App *a) {
    return (LRESULT)(a->scratch.count+1)|(a->scratch.visible?0x100:0)|(a->closing?0x200:0);
}
static void dispatch(App *a,size_t index,HWND target) {
    if (a->closing || index>=a->profile->binding_count) return;
    const SpBinding *binding=&a->profile->bindings[index]; SpRect work={0};
    bool needs_work=!wcscmp(binding->action,L"scratch.toggle")||!wcscmp(binding->action,L"scratch.member")||
        !wcscmp(binding->action,L"scratch.promote")||!wcscmp(binding->action,L"window.center");
    if (needs_work && !sp_win32_scratch_workarea(&a->api,&a->scratch,&work)) { notify(a,a->api.error); return; }
    SpModuleContext c={&a->scratch,&a->api,a->profile,target,work,a,report,notify,background};
    (void)sp_modules_run(&c,binding);
}
static bool add_tray(App *a) {
    ZeroMemory(&a->tray,sizeof(a->tray)); a->tray.cbSize=sizeof(a->tray);
    a->tray.hWnd=a->window; a->tray.uID=1; a->tray.uFlags=NIF_ICON|NIF_MESSAGE|NIF_TIP;
    a->tray.uCallbackMessage=SP_TRAY_MESSAGE; a->tray.hIcon=LoadIconW(NULL,IDI_APPLICATION);
    swprintf(a->tray.szTip,128,L"CloakSpace - %.80ls",a->profile->name);
    a->has_tray=Shell_NotifyIconW(NIM_ADD,&a->tray)!=0; return a->has_tray;
}
static void menu(App *a) {
    HMENU m=CreatePopupMenu(); if (!m) return;
    AppendMenuW(m,MF_STRING,ID_TOGGLE,L"Show / hide scratchpad");
    AppendMenuW(m,MF_STRING,ID_RESTORE,L"Restore all windows");
    AppendMenuW(m,MF_SEPARATOR,0,NULL); AppendMenuW(m,MF_STRING,ID_EXIT,L"Restore all and exit");
    POINT p; GetCursorPos(&p); SetForegroundWindow(a->window);
    UINT chosen=TrackPopupMenu(m,TPM_RETURNCMD|TPM_NONOTIFY|TPM_RIGHTBUTTON,p.x,p.y,0,a->window,NULL);
    PostMessageW(a->window,WM_NULL,0,0); DestroyMenu(m);
    if (a->closing) return; /* Exit can run while the tray menu is tracking. */
    if (chosen==ID_EXIT) begin_close(a);
    else if(chosen==ID_RESTORE) report(a,sp_release_all(&a->scratch));
    else if(chosen==ID_TOGGLE) {
        SpRect work; if(sp_win32_scratch_workarea(&a->api,&a->scratch,&work)) report(a,sp_toggle(&a->scratch,work)); else notify(a,a->api.error);
    }
}
static LRESULT CALLBACK host_proc(HWND h,UINT msg,WPARAM wp,LPARAM lp) {
    App *a=&app;
    if (msg==a->control && a->control) {
        if (wp==SP_CONTROL_STATUS) return status(a);
        if (wp==SP_CONTROL_RESTORE) {
            SpResult restored=sp_release_all(&a->scratch);report(a,restored);
            return status(a)|((restored.status==SP_OK || restored.status==SP_NOOP)?0:SP_CONTROL_FAILED);
        }
        if (wp==SP_CONTROL_QUIT) return begin_close(a)?1:0;
        return 0;
    }
    if (msg==a->taskbar_created && a->taskbar_created) { (void)add_tray(a); return 0; }
    switch(msg) {
    case SP_BIND_MESSAGE: dispatch(a,(size_t)wp,(HWND)lp); return 0;
    case SP_TRAY_MESSAGE:
        if(a->closing) return 0;
        if (lp==WM_RBUTTONUP||lp==WM_CONTEXTMENU) menu(a);
        else if(lp==WM_LBUTTONDBLCLK) {
            SpRect r; if(sp_win32_scratch_workarea(&a->api,&a->scratch,&r)) report(a,sp_toggle(&a->scratch,r));
        }
        return 0;
    case SP_JOB_MESSAGE:
        finish_job(a); if(a->worker) SetTimer(h,SP_HOST_JOB_TIMER,25,NULL); else if(a->closing) DestroyWindow(h); return 0;
    case SP_OVERLAY_HIDE_COMPLETE: check_hide(a,(uintptr_t)wp); return 0;
    case WM_TIMER:
        if(wp==SP_HOST_JOB_TIMER) { finish_job(a); if(!a->worker) {KillTimer(h,SP_HOST_JOB_TIMER);if(a->closing)DestroyWindow(h);} }
        else if(wp==SP_HOST_HIDE_TIMER) check_hide(a,a->scratch.hide_request);
        return 0;
    case WM_CLOSE: begin_close(a); return 0;
    case WM_QUERYENDSESSION: return TRUE;
    case WM_ENDSESSION:
        if(wp) { SetEvent(a->cancel); (void)sp_release_all(&a->scratch); sp_keyboard_stop(&a->keyboard); DestroyWindow(h); }
        return 0;
    case WM_DESTROY:
        a->closing=true;
        stop_hide_watchdog(a);a->scratch.hide_request=0;
        a->scratch.workspace=NULL;a->scratch.workspace_hide=NULL;a->scratch.workspace_ctx=NULL;
        sp_overlay_destroy(a->overlay);a->overlay=NULL;
        if(a->has_tray) { Shell_NotifyIconW(NIM_DELETE,&a->tray); a->has_tray=false; }
        PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(h,msg,wp,lp);
}
static int control(const wchar_t *cls,unsigned action) {
    HWND h=FindWindowW(cls,NULL); if(!h) {output(L"No matching scratchpad instance is running.\r\n",true);return 1;}
    DWORD pid=0; GetWindowThreadProcessId(h,&pid);
    HANDLE process=OpenProcess(SYNCHRONIZE,FALSE,pid);
    DWORD_PTR response=0;
    UINT message=RegisterWindowMessageW(SP_CONTROL_NAME);
    if (!SendMessageTimeoutW(h,message,action,0,SMTO_ABORTIFHUNG|SMTO_BLOCK,30000,&response)) {
        output(L"Scratchpad control timed out or was denied. No success is assumed.\r\n",true);
        if(process)CloseHandle(process);return 1;
    }
    if(action==SP_CONTROL_QUIT) {
        bool ok=response==1 && process && WaitForSingleObject(process,5000)==WAIT_OBJECT_0;
        if(process)CloseHandle(process);
        output(ok?L"Scratchpad restored and exited.\r\n":L"Exit was refused or not confirmed; retry Restore all.\r\n",!ok);
        return ok?0:1;
    }
    if(process)CloseHandle(process);
    unsigned encoded_count=(unsigned)(response&0xff);
    if (!encoded_count || encoded_count>SP_CAPACITY+1) {
        output(L"Scratchpad returned an invalid status. No success is assumed.\r\n",true);return 1;
    }
    wchar_t line[160];unsigned remaining=encoded_count-1;bool failed=(response&SP_CONTROL_FAILED)!=0;
    swprintf(line,160,L"members=%u visible=%u closing=%u failed=%u\r\n",remaining,!!(response&0x100),!!(response&0x200),failed);output(line,false);
    if (action==SP_CONTROL_RESTORE && (failed || remaining || (response&(0x100|0x200)))) {
        output(L"Restore was not confirmed; the workspace or windows may remain active. Retry Restore all.\r\n",true);return 1;
    }
    return 0;
}
static bool valid_instance(const wchar_t *s) {
    size_t n=wcslen(s); if(!n||n>48)return false;
    for(;*s;++s) if(!((*s>=L'A'&&*s<=L'Z')||(*s>=L'a'&&*s<=L'z')||(*s>=L'0'&&*s<=L'9')||*s==L'-'||*s==L'_'))return false;
    return true;
}
static int run(int argc,wchar_t **argv) {
    const wchar_t *config=NULL,*profile=L"default",*instance=L"default";
    bool check=false,no_hotkeys=false,no_tray=false,list=false;
    unsigned command=0;
    for(int i=1;i<argc;++i) {
        if(!wcscmp(argv[i],L"--version")) {output(L"cloakspace " SP_VERSION L" (C17 / Win32)\r\n",false);return 0;}
        if(!wcscmp(argv[i],L"--help")) {
            output(L"cloakspace [--config PATH] [--profile NAME] [--instance NAME]\r\n"
                   L"  --check-config  Validate configuration and module arguments without starting\r\n"
                   L"  --list-actions  List registered C module actions\r\n"
                   L"  --status | --restore | --quit  Control the selected running instance\r\n"
                   L"  --no-hotkeys --no-tray  Diagnostic modes; keep control commands available\r\n"
                   L"Configuration: APPDATA/cloakspace/scratchpad.conf, then executable directory, then built-in defaults.\r\n",false);return 0;
        }
        if(!wcscmp(argv[i],L"--config")||!wcscmp(argv[i],L"--profile")||!wcscmp(argv[i],L"--instance")) {
            const wchar_t *option=argv[i]; if(++i==argc) {output(L"Missing option value.\r\n",true);return 2;}
            if(!wcscmp(option,L"--config")) config=argv[i]; else if(!wcscmp(option,L"--profile"))profile=argv[i];else instance=argv[i];
        } else if(!wcscmp(argv[i],L"--check-config"))check=true;
        else if(!wcscmp(argv[i],L"--list-actions"))list=true;
        else if(!wcscmp(argv[i],L"--no-hotkeys"))no_hotkeys=true;
        else if(!wcscmp(argv[i],L"--no-tray"))no_tray=true;
        else if(!wcscmp(argv[i],L"--status")||!wcscmp(argv[i],L"--restore")||!wcscmp(argv[i],L"--quit")) {
            if(command) {output(L"Choose one control command.\r\n",true);return 2;}
            command=!wcscmp(argv[i],L"--status")?SP_CONTROL_STATUS:!wcscmp(argv[i],L"--restore")?SP_CONTROL_RESTORE:SP_CONTROL_QUIT;
        } else {output(L"Unknown argument; use --help.\r\n",true);return 2;}
    }
    if(!valid_instance(instance)) {output(L"Instance name must contain 1..48 ASCII letters/digits/hyphens/underscores.\r\n",true);return 2;}
    wchar_t cls[100],mutex[100]; swprintf(cls,100,L"%ls%ls",SP_HOST_PREFIX,instance);
    swprintf(mutex,100,L"Local\\CloakSpace.v1.%ls",instance);
    if(command)return control(cls,command);
    if(list) {
        size_t count=0;const SpAction *actions=sp_actions(&count);wchar_t line[512];
        for(size_t i=0;i<count;++i) {swprintf(line,512,L"%ls (%zu..%zu args): %ls\r\n",actions[i].name,actions[i].min_args,actions[i].max_args,actions[i].description);output(line,false);}
        return 0;
    }
    wchar_t error[512];
    if(!sp_config_load(&app.config,config,error,512)) {output(error,true);output(L"\r\n",true);return 2;}
    for(size_t i=0;i<app.config.count;++i) if(!sp_modules_validate(&app.config.profiles[i],error,512)) {output(error,true);output(L"\r\n",true);return 2;}
    app.profile=sp_config_profile(&app.config,profile);
    if(!app.profile) {output(L"Selected profile was not found.\r\n",true);return 2;}
    if(check) {swprintf(error,512,L"Configuration OK: %zu profiles; selected '%ls' (%zu bindings).\r\n",app.config.count,app.profile->name,app.profile->binding_count);output(error,false);return 0;}
    app.mutex=CreateMutexW(NULL,FALSE,mutex);DWORD mutex_error=GetLastError();
    if(!app.mutex||mutex_error==ERROR_ALREADY_EXISTS) {if(app.mutex)CloseHandle(app.mutex);app.mutex=NULL;output(L"Instance already running, or cannot create its mutex. No process was replaced.\r\n",true);return 2;}
    if(!sp_win32_init(&app.api,false)||!sp_init(&app.scratch,sp_win32_ops(),&app.api)) {output(L"Cannot initialize window API.\r\n",true);return 1;}
    app.scratch.layout=app.profile->layout;
    app.cancel=CreateEventW(NULL,TRUE,FALSE,NULL); if(!app.cancel) {output(L"Cannot create cancellation event.\r\n",true);return 1;}
    app.control=RegisterWindowMessageW(SP_CONTROL_NAME);app.taskbar_created=RegisterWindowMessageW(L"TaskbarCreated");
    WNDCLASSW wc={0};wc.hInstance=GetModuleHandleW(NULL);wc.lpfnWndProc=host_proc;wc.lpszClassName=cls;
    if(!app.control||!RegisterClassW(&wc)) {output(L"Cannot register host window.\r\n",true);return 1;}
    app.window=CreateWindowExW(WS_EX_TOOLWINDOW,cls,L"CloakSpace",WS_OVERLAPPED,0,0,0,0,NULL,NULL,wc.hInstance,NULL);
    if(!app.window) {output(L"Cannot create host window.\r\n",true);return 1;}
    UINT fade_ms=sp_appearance_resolve(app.profile).tint_fade_ms.value;
    app.overlay=sp_overlay_create_with_fade(app.window,app.api.tag,(BYTE)sp_tint_alpha(app.profile),fade_ms,error,512);
    if(!app.overlay) {output(error,true);DestroyWindow(app.window);return 1;}
    app.scratch.workspace=workspace;app.scratch.workspace_hide=hide_workspace;app.scratch.workspace_ctx=&app;
    if(!no_tray && !add_tray(&app)) {output(L"Cannot add recovery tray icon; startup cancelled.\r\n",true);DestroyWindow(app.window);return 1;}
    if(!no_hotkeys && !sp_keyboard_start(&app.keyboard,app.window,SP_BIND_MESSAGE,app.profile)) {
        output(L"Cannot install the keyboard hook; startup cancelled.\r\n",true);DestroyWindow(app.window);return 1;
    }
    MSG msg;int got;while((got=GetMessageW(&msg,NULL,0,0))>0) {TranslateMessage(&msg);DispatchMessageW(&msg);}
    (void)sp_release_all(&app.scratch);SetEvent(app.cancel);sp_keyboard_stop(&app.keyboard);
    if(app.overlay) DestroyWindow(app.window); /* Also join the overlay if the loop stopped without WM_DESTROY. */
    if(app.worker) {WaitForSingleObject(app.worker,10000);CloseHandle(app.worker);}
    sp_win32_dispose(&app.api);CloseHandle(app.cancel);CloseHandle(app.mutex);UnregisterClassW(cls,wc.hInstance);
    return got<0?1:0;
}
int WINAPI wWinMain(HINSTANCE instance,HINSTANCE previous,LPWSTR command,int show) {
    (void)instance;(void)previous;(void)command;(void)show;sp_win32_dpi();
    int argc=0;LPWSTR *argv=CommandLineToArgvW(GetCommandLineW(),&argc);
    if(!argv)return 2;int result=run(argc,argv);LocalFree(argv);return result;
}
