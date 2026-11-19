#include "host.h"
#include "overlay.h"
#include "process.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
static unsigned checks;
static PROCESS_INFORMATION host;
static HWND windows[2];
static HANDLE windows_thread,windows_ready;
static DWORD windows_thread_id;
static wchar_t fixture[32768],bad_fixture[32768];
#define CHECK(x) do { ++checks; if (!(x)) { \
    fprintf(stderr,"%s:%d: %s\n",__FILE__,__LINE__,#x); exit(1); } } while(0)
static void cleanup(void) {
    if(host.hProcess) {
        if(WaitForSingleObject(host.hProcess,0)!=WAIT_OBJECT_0) {TerminateProcess(host.hProcess,99);WaitForSingleObject(host.hProcess,2000);}
        CloseHandle(host.hProcess);if(host.hThread)CloseHandle(host.hThread);ZeroMemory(&host,sizeof(host));
    }
    if(windows_thread) {PostThreadMessageW(windows_thread_id,WM_QUIT,0,0);WaitForSingleObject(windows_thread,2000);CloseHandle(windows_thread);windows_thread=NULL;}
    if(windows_ready) {CloseHandle(windows_ready);windows_ready=NULL;}
    if(fixture[0])DeleteFileW(fixture);if(bad_fixture[0])DeleteFileW(bad_fixture);
}
static LRESULT CALLBACK disposable_proc(HWND h,UINT msg,WPARAM wp,LPARAM lp) {return DefWindowProcW(h,msg,wp,lp);}
static DWORD WINAPI create_windows(void *unused) {
    (void)unused;WNDCLASSW wc={0};wc.lpfnWndProc=disposable_proc;wc.hInstance=GetModuleHandleW(NULL);wc.lpszClassName=L"CloakSpace.HostDisposable";
    if(RegisterClassW(&wc)) for(int i=0;i<2;++i)
        windows[i]=CreateWindowExW(WS_EX_TOOLWINDOW,wc.lpszClassName,L"Disposable host integration window",
            WS_POPUP|WS_VISIBLE,100+i*320,100,300,200,NULL,NULL,wc.hInstance,NULL);
    SetEvent(windows_ready);MSG m;
    while(GetMessageW(&m,NULL,0,0)>0){TranslateMessage(&m);DispatchMessageW(&m);}
    for(int i=0;i<2;++i)if(windows[i])DestroyWindow(windows[i]);
    UnregisterClassW(wc.lpszClassName,wc.hInstance);return 0;
}
static void write_fixture(const wchar_t *path,const char *text) {
    HANDLE f=CreateFileW(path,GENERIC_WRITE,0,NULL,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,NULL);CHECK(f!=INVALID_HANDLE_VALUE);
    DWORD written=0;CHECK(WriteFile(f,text,(DWORD)strlen(text),&written,NULL) && written==strlen(text));CloseHandle(f);
}
static DWORD_PTR send(HWND h,UINT msg,WPARAM wp,LPARAM lp) {
    DWORD_PTR value=0;CHECK(SendMessageTimeoutW(h,msg,wp,lp,SMTO_BLOCK|SMTO_ABORTIFHUNG,5000,&value));return value;
}
static BOOL CALLBACK find_backdrop(HWND h,LPARAM data) {
    DWORD pid=0;wchar_t cls[96];GetWindowThreadProcessId(h,&pid);
    if(pid==host.dwProcessId && GetClassNameW(h,cls,96) && !wcscmp(cls,SP_OVERLAY_CLASS)) {
        *(HWND *)data=h;return FALSE;
    }
    return TRUE;
}
static HWND backdrop(void) {HWND h=NULL;EnumWindows(find_backdrop,(LPARAM)&h);CHECK(h);return h;}
static void check_alpha(HWND h,BYTE expected) {
    COLORREF color=CLR_INVALID;BYTE alpha=0;DWORD flags=0;
    CHECK(GetLayeredWindowAttributes(h,&color,&alpha,&flags));CHECK(alpha==expected && flags==LWA_ALPHA);
}
/* Keep GUI-child failures in the wrapper log, including legitimate startup
   input-gate refusals. Whitelist only these three inherited standard handles. */
static bool create_host(const wchar_t *exe,wchar_t *command) {
    HANDLE handles[3]={0};SECURITY_ATTRIBUTES sa={sizeof(sa),NULL,TRUE};
    handles[0]=CreateFileW(L"NUL",GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE,&sa,OPEN_EXISTING,0,NULL);
    if(handles[0]==INVALID_HANDLE_VALUE)return false;
    HANDLE self=GetCurrentProcess();bool ok=false;
    if(!DuplicateHandle(self,GetStdHandle(STD_OUTPUT_HANDLE),self,&handles[1],0,TRUE,DUPLICATE_SAME_ACCESS) ||
       !DuplicateHandle(self,GetStdHandle(STD_ERROR_HANDLE),self,&handles[2],0,TRUE,DUPLICATE_SAME_ACCESS))goto done;
    SIZE_T size=0;InitializeProcThreadAttributeList(NULL,1,0,&size);
    LPPROC_THREAD_ATTRIBUTE_LIST attributes=malloc(size);if(!attributes)goto done;
    if(InitializeProcThreadAttributeList(attributes,1,0,&size)) {
        if(UpdateProcThreadAttribute(attributes,0,PROC_THREAD_ATTRIBUTE_HANDLE_LIST,handles,sizeof(handles),NULL,NULL)) {
            STARTUPINFOEXW info={0};info.StartupInfo.cb=sizeof(info);info.StartupInfo.dwFlags=STARTF_USESTDHANDLES;
            info.StartupInfo.hStdInput=handles[0];info.StartupInfo.hStdOutput=handles[1];info.StartupInfo.hStdError=handles[2];info.lpAttributeList=attributes;
            ok=CreateProcessW(exe,command,NULL,NULL,TRUE,CREATE_NO_WINDOW|EXTENDED_STARTUPINFO_PRESENT,NULL,NULL,&info.StartupInfo,&host)!=0;
        }
        DeleteProcThreadAttributeList(attributes);
    }
    free(attributes);
done:
    for(unsigned i=0;i<3;++i)if(handles[i])CloseHandle(handles[i]);return ok;
}
static HWND start_host(const wchar_t *exe,const wchar_t *instance,bool tray,const wchar_t *profile) {
    const wchar_t *args[]={exe,L"--config",fixture,L"--instance",instance,L"--profile",profile,L"--no-hotkeys",L"--no-tray"};
    wchar_t command[SP_COMMAND_CAP];CHECK(sp_commandline(command,SP_COMMAND_CAP,args,sizeof(args)/sizeof(*args)-(tray?1u:0u)));
    CHECK(create_host(exe,command));
    wchar_t cls[120];swprintf(cls,120,L"%ls%ls",SP_HOST_PREFIX,instance);
    HWND h=NULL;ULONGLONG start=GetTickCount64();
    while(GetTickCount64()-start<5000 && WaitForSingleObject(host.hProcess,0)!=WAIT_OBJECT_0) {
        h=FindWindowW(cls,NULL);if(h)break;Sleep(5);
    }
    CHECK(h);DWORD pid=0;GetWindowThreadProcessId(h,&pid);CHECK(pid==host.dwProcessId);
    (void)WaitForInputIdle(host.hProcess,3000);return h;
}
static void wait_exit(void) {
    CHECK(WaitForSingleObject(host.hProcess,5000)==WAIT_OBJECT_0);
    DWORD code=1;CHECK(GetExitCodeProcess(host.hProcess,&code)&&code==0);
    CloseHandle(host.hProcess);CloseHandle(host.hThread);ZeroMemory(&host,sizeof(host));
}
int wmain(int argc,wchar_t **argv) {
    CHECK(argc==2);atexit(cleanup);(void)SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    wchar_t exe[32768],root[32768];DWORD n=GetFullPathNameW(argv[1],32768,exe,NULL);CHECK(n&&n<32768);
    n=GetCurrentDirectoryW(32768,root);CHECK(n&&n<32000);
    swprintf(fixture,32768,L"%ls\\build\\host-%lu.conf",root,(unsigned long)GetCurrentProcessId());
    swprintf(bad_fixture,32768,L"%ls\\build\\host-bad-%lu.conf",root,(unsigned long)GetCurrentProcessId());
    write_fixture(fixture,"profile default\nlayout gap 12\nbind Win+S scratch.toggle\nbind Win+Shift+S scratch.member\nbind Win+Alt+S scratch.promote\nbind Win+Ctrl+Shift+S scratch.restore\nend\n");
    write_fixture(bad_fixture,"profile default\nbind Win+S nonexistent.module\nend\n");
    SpProcessResult result;const wchar_t *version[]={exe,L"--version"};
    CHECK(sp_capture(exe,version,2,5000,NULL,&result)&&result.exit_code==0);
    char expected_version[256];
    CHECK(WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,L"cloakspace " SP_VERSION L" (C17 / Win32)\r\n",
        -1,expected_version,sizeof(expected_version),NULL,NULL)>0);
    CHECK(result.out_size==strlen(expected_version) && !strcmp(result.out,expected_version));
    CHECK(!result.err_size);
    const wchar_t *help[]={exe,L"--help"};
    CHECK(sp_capture(exe,help,2,5000,NULL,&result)&&result.exit_code==0);
    CHECK(strstr(result.out,"cloakspace [--config PATH]")==result.out);
    CHECK(strstr(result.out,"APPDATA/cloakspace/scratchpad.conf, then executable directory, then built-in defaults."));
    CHECK(!result.err_size);
    const wchar_t *valid[]={exe,L"--config",fixture,L"--check-config"};
    CHECK(sp_capture(exe,valid,4,5000,NULL,&result)&&result.exit_code==0);CHECK(strstr(result.out,"Configuration OK"));
    const wchar_t *invalid[]={exe,L"--config",bad_fixture,L"--check-config"};
    CHECK(sp_capture(exe,invalid,4,5000,NULL,&result)&&result.exit_code==2);CHECK(strstr(result.err,"unknown action"));
    write_fixture(bad_fixture,"profile default\nbind Win+S scratch.toggle\nend\n\xff");
    CHECK(sp_capture(exe,invalid,4,5000,NULL,&result)&&result.exit_code==2);CHECK(strstr(result.err,"UTF-8"));
    const char *bad_launch[]={
        "profile default\nbind Ctrl+Alt+U terminal.quake \"\"\nend\n",
        "profile default\nbind Ctrl+Alt+U process.exec \"\"\nend\n",
        "profile default\nterminal-profile \"Dev;Tools\"\nbind Ctrl+Alt+U scratch.toggle\nend\n",
        "profile default\nquake-window \"One;Two\"\nbind Ctrl+Alt+U scratch.toggle\nend\n",
        "profile default\ncontainer-profile \"Dev;Tools\"\nbind Ctrl+Alt+U scratch.toggle\nend\n",
        "profile default\nbind Ctrl+Alt+U terminal.quake Dev tool.exe \"a;b\"\nend\n",
        "profile default\nbind Ctrl+Alt+U terminal.quake Dev tool.exe \"\"\nend\n",
        "profile default\nbind Ctrl+Alt+U terminal.quake Dev tool.exe \"a\\\"b\"\nend\n",
        "profile default\ncontainer-directory .\nbind Ctrl+Alt+U scratch.toggle\nend\n",
        "profile default\ncontainer-directory ..\nbind Ctrl+Alt+U scratch.toggle\nend\n",
        "profile default\ncontainer-directory app\nbind Ctrl+Alt+U scratch.toggle\nend\n",
        "profile default\ncontainer-directory ../app\nbind Ctrl+Alt+U scratch.toggle\nend\n",
        "profile default\ncontainer-directory C:/app\nbind Ctrl+Alt+U scratch.toggle\nend\n",
        "profile default\ncontainer-directory \"C:\\\\app\"\nbind Ctrl+Alt+U scratch.toggle\nend\n",
        "profile default\ncontainer-directory \"/app;other\"\nbind Ctrl+Alt+U scratch.toggle\nend\n",
        "profile default\nbind Ctrl+Alt+U terminal.container \"\"\nend\n",
        "profile default\nbind Ctrl+Alt+U terminal.container /bin/sh \"a;b\"\nend\n"};
    for (size_t i=0;i<sizeof(bad_launch)/sizeof(bad_launch[0]);++i) {
        write_fixture(bad_fixture,bad_launch[i]);
        CHECK(sp_capture(exe,invalid,4,5000,NULL,&result)&&result.exit_code==2);
        CHECK(result.err_size && !result.out_size);
    }
    write_fixture(bad_fixture,"profile default\nterminal-profile \"Dev \\\"Quoted\\\"\"\nbind Ctrl+Alt+U process.exec tool.exe \"\" \"a;b\"\nend\n");
    CHECK(sp_capture(exe,invalid,4,5000,NULL,&result)&&result.exit_code==0);
    CHECK(strstr(result.out,"Configuration OK"));
    write_fixture(bad_fixture,"profile default\nbind Ctrl+Alt+T terminal.open\nbind Ctrl+Alt+K terminal.container\nend\n");
    CHECK(sp_capture(exe,invalid,4,5000,NULL,&result)&&result.exit_code==0);
    CHECK(strstr(result.out,"Configuration OK")); /* No installed Terminal/container required. */
    write_fixture(bad_fixture,"profile default\ncontainer-directory \"/app/shared \xe9\x9b\xaa files\"\nbind Ctrl+Alt+K terminal.container\nend\n");
    CHECK(sp_capture(exe,invalid,4,5000,NULL,&result)&&result.exit_code==0);
    CHECK(strstr(result.out,"Configuration OK")); /* Spaces/UTF-8; existence is Podman's check. */
    /* Inactive profiles are validated too, before any host or integration starts. */
    write_fixture(bad_fixture,"profile default\nbind Ctrl+Alt+S scratch.toggle\nend\n"
        "profile other\ncontainer-directory relative/path\nbind Ctrl+Alt+K scratch.toggle\nend\n");
    CHECK(sp_capture(exe,invalid,4,5000,NULL,&result)&&result.exit_code==2);
    CHECK(!result.out_size && strstr(result.err,"Profile 'other'") && strstr(result.err,"absolute POSIX path"));
    write_fixture(bad_fixture,"profile default\nbind Ctrl+Alt+S scratch.toggle\nend\n"
        "profile other\ncontainer-directory /app\nbind Ctrl+Alt+K scratch.toggle\nend\n");
    CHECK(sp_capture(exe,invalid,4,5000,NULL,&result)&&result.exit_code==0);
    CHECK(strstr(result.out,"2 profiles; selected 'default'"));
    wchar_t instance[64];swprintf(instance,64,L"test-%lu",(unsigned long)GetCurrentProcessId());
    windows_ready=CreateEventW(NULL,TRUE,FALSE,NULL);CHECK(windows_ready);
    windows_thread=CreateThread(NULL,0,create_windows,NULL,0,&windows_thread_id);CHECK(windows_thread);
    CHECK(WaitForSingleObject(windows_ready,3000)==WAIT_OBJECT_0 && windows[0] && windows[1]);
    UINT ctl=RegisterWindowMessageW(SP_CONTROL_NAME);CHECK(ctl);
    HWND h=start_host(exe,instance,false,L"default");CHECK(send(h,ctl,SP_CONTROL_STATUS,0)==1);
    HWND shade=backdrop();CHECK(!IsWindowVisible(shade));check_alpha(shade,36);
    (void)send(h,SP_BIND_MESSAGE,0,0);CHECK(send(h,ctl,SP_CONTROL_STATUS,0)==0x101);
    CHECK(IsWindowVisible(shade));CHECK(IsWindowEnabled(windows[0])&&IsWindowEnabled(windows[1]));
    (void)send(h,SP_BIND_MESSAGE,0,0);CHECK(send(h,ctl,SP_CONTROL_STATUS,0)==1);CHECK(!IsWindowVisible(shade));
    /* Empty Restore all must also remove the active surface. */
    (void)send(h,SP_BIND_MESSAGE,0,0);CHECK(IsWindowVisible(shade));
    CHECK(send(h,ctl,SP_CONTROL_RESTORE,0)==1);CHECK(!IsWindowVisible(shade));
    const wchar_t *duplicate[]={exe,L"--config",fixture,L"--instance",instance,L"--no-hotkeys",L"--no-tray"};
    CHECK(sp_capture(exe,duplicate,7,5000,NULL,&result)&&result.exit_code==2);
    CHECK(WaitForSingleObject(host.hProcess,0)==WAIT_TIMEOUT);
    (void)send(h,SP_BIND_MESSAGE,1,(LPARAM)windows[0]);CHECK(!IsWindowVisible(windows[0]));
    CHECK((send(h,ctl,SP_CONTROL_STATUS,0)&0xff)==2);
    (void)send(h,SP_BIND_MESSAGE,1,(LPARAM)windows[1]);CHECK(!IsWindowVisible(windows[1]));
    (void)send(h,SP_BIND_MESSAGE,0,0);CHECK(IsWindowVisible(windows[0])&&IsWindowVisible(windows[1]));
    CHECK(IsWindowVisible(shade));
    CHECK(GetWindowLongPtrW(windows[0],GWL_EXSTYLE)&WS_EX_TOPMOST);
    CHECK((send(h,ctl,SP_CONTROL_STATUS,0)&0x1ff)==0x103);
    (void)send(h,SP_BIND_MESSAGE,2,(LPARAM)windows[1]);
    (void)send(h,SP_BIND_MESSAGE,0,0);CHECK(!IsWindowVisible(windows[0])&&!IsWindowVisible(windows[1]));
    CHECK(!IsWindowVisible(shade));
    const wchar_t *restore[]={exe,L"--instance",instance,L"--restore"};
    CHECK(sp_capture(exe,restore,4,5000,NULL,&result)&&result.exit_code==0);CHECK(strstr(result.out,"members=0"));
    CHECK(IsWindowVisible(windows[0])&&IsWindowVisible(windows[1]));CHECK(!(GetWindowLongPtrW(windows[0],GWL_EXSTYLE)&WS_EX_TOPMOST));
    CHECK(send(h,ctl,SP_CONTROL_STATUS,0)==1);
    (void)send(h,SP_BIND_MESSAGE,1,(LPARAM)windows[0]);CHECK(!IsWindowVisible(windows[0]));
    const wchar_t *quit[]={exe,L"--instance",instance,L"--quit"};
    CHECK(sp_capture(exe,quit,4,10000,NULL,&result)&&result.exit_code==0);CHECK(strstr(result.out,"restored and exited"));
    wait_exit();CHECK(IsWindowVisible(windows[0]));CHECK(!IsWindow(shade));
    /* Also exercise the real notification-area add/remove and normal WM_CLOSE route. */
    h=start_host(exe,instance,true,L"default");
    shade=backdrop();
    (void)send(h,SP_BIND_MESSAGE,1,(LPARAM)windows[1]);CHECK(!IsWindowVisible(windows[1]));
    (void)send(h,SP_BIND_MESSAGE,0,0);CHECK(IsWindowVisible(shade));
    CHECK(PostMessageW(h,WM_CLOSE,0,0));wait_exit();CHECK(IsWindowVisible(windows[1]));CHECK(!IsWindow(shade));
    /* Exit while an empty workspace is active still removes its hooks/window. */
    h=start_host(exe,instance,false,L"default");shade=backdrop();
    (void)send(h,SP_BIND_MESSAGE,0,0);CHECK(IsWindowVisible(shade));
    CHECK(PostMessageW(h,WM_CLOSE,0,0));wait_exit();CHECK(!IsWindow(shade));
    /* Actual startup wiring: selected-profile local wins; late global inherits.
       These cases only inspect hidden surfaces, never request foreground input. */
    static const struct {const char *text;const wchar_t *profile;BYTE alpha;} tints[]={
        {"tint-opacity 0.25\nprofile default\nbind Win+S scratch.toggle\nend\n",L"default",64},
        {"profile default\ntint-opacity .5\nbind Win+S scratch.toggle\nend\ntint-opacity 1\n",L"default",128},
        {"profile default\ntint-opacity .5\nbind Win+S scratch.toggle\nend\nprofile other\nbind Win+S scratch.toggle\nend\ntint-opacity 1.0\n",L"other",255},
        {"tint-opacity 1\nprofile default\ntint-opacity 0.0\nbind Win+S scratch.toggle\nend\n",L"default",1}
    };
    for(size_t i=0;i<sizeof(tints)/sizeof(*tints);++i) {
        write_fixture(fixture,tints[i].text);h=start_host(exe,instance,false,tints[i].profile);shade=backdrop();
        CHECK(!IsWindowVisible(shade));check_alpha(shade,tints[i].alpha);
        CHECK(send(h,ctl,SP_CONTROL_QUIT,0)==1);wait_exit();CHECK(!IsWindow(shade));
    }
    cleanup();printf("host: %u checks passed (real executable, control/exit/tray; disposable windows only)\n",checks);return 0;
}
