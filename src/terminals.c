#include "terminals.h"
#include "launch_args.h"
#include "elevation.h"
#include <stdio.h>
#include <string.h>
#include <wchar.h>

static bool resolve(const wchar_t *name,wchar_t *out) {
    if ((wcslen(name)>2 && name[1]==L':' && (name[2]==L'\\' || name[2]==L'/')) ||
        (name[0]==L'\\' && name[1]==L'\\')) {
        DWORD n=GetFullPathNameW(name,SP_COMMAND_CAP,out,NULL);
        DWORD attr=n&&n<SP_COMMAND_CAP?GetFileAttributesW(out):INVALID_FILE_ATTRIBUTES;
        return attr!=INVALID_FILE_ATTRIBUTES && !(attr&FILE_ATTRIBUTE_DIRECTORY);
    }
    if (sp_find_executable(name,out,SP_COMMAND_CAP)) return true;
    if (!wcschr(name,L'.') && !wcspbrk(name,L"\\/:")) {
        wchar_t with_exe[512];
        if (wcslen(name)+5>512) return false;
        swprintf(with_exe,512,L"%ls.exe",name); return sp_find_executable(with_exe,out,SP_COMMAND_CAP);
    }
    return false;
}
bool sp_terminal_run(const SpProfile *p,const SpBinding *b,HANDLE cancel,wchar_t *message,size_t cap) {
    message[0]=0;
    if (cancel && WaitForSingleObject(cancel,0)==WAIT_OBJECT_0) return false;
    const wchar_t *problem=sp_launch_binding_error(b);
    if (problem) { swprintf(message,cap,L"%ls",problem); return false; }
    wchar_t exe[SP_COMMAND_CAP]; DWORD error=0;
    if (!wcscmp(b->action,L"process.exec")) {
        /* Check the decoded request before resolving or starting any broker. */
        if (!sp_elevation_guard(p,b->args[0],b->args,b->argc,message,cap)) return false;
        if (!resolve(b->args[0],exe)) { swprintf(message,cap,L"Executable not found on absolute PATH or by absolute path: %ls",b->args[0]); return false; }
        const wchar_t *argv[SP_MAX_ARGS]; argv[0]=exe;
        for(size_t i=1;i<b->argc;++i) argv[i]=b->args[i];
        if (!sp_elevation_guard(p,exe,argv,b->argc,message,cap)) return false;
        if (!sp_spawn(exe,argv,b->argc,&error)) {
            if (error==ERROR_ELEVATION_REQUIRED)
                swprintf(message,cap,L"Executable requires elevation; no elevation retry was attempted. Use a non-elevated alternative and check never-elevate policy.");
            else swprintf(message,cap,L"Cannot launch executable (Windows error %lu).",(unsigned long)error);
            return false;
        }
        return true;
    }
    bool is_container=!wcscmp(b->action,L"terminal.container");
    const wchar_t *terminal[]={L"wt.exe"},*podman[]={L"podman.exe"};
    if (!sp_elevation_guard(p,terminal[0],terminal,1,message,cap)) return false;
    if (is_container &&
        !sp_elevation_guard(p,podman[0],podman,1,message,cap)) return false;
    problem=sp_terminal_settings_error(p);
    if (problem) { swprintf(message,cap,L"%ls",problem); return false; }
    const wchar_t *selected_profile=is_container && p->container_profile?p->container_profile:p->terminal_profile;
    if (!wcscmp(b->action,L"terminal.quake")) selected_profile=b->args[0];
    const wchar_t *argv[32]={terminal[0],L"-w",p->quake_window,L"nt"}; size_t argc=4;
    if (selected_profile) { argv[argc++]=L"-p"; argv[argc++]=selected_profile; }
    if (!wcscmp(b->action,L"terminal.quake")) {
        if (b->argc>1) argv[argc++]=L"--";
        for(size_t i=1;i<b->argc;++i) argv[argc++]=b->args[i];
        /* Preflight the exact raw Terminal vector, whose child CWD is unknown,
           before even discovering wt.exe. Only argv[0] is resolved below. */
        if (!sp_elevation_guard(p,argv[0],argv,argc,message,cap)) return false;
    }
    wchar_t wt[SP_COMMAND_CAP];
    if (!sp_find_executable(L"wt.exe",wt,SP_COMMAND_CAP)) {
        swprintf(message,cap,L"Windows Terminal (wt.exe) was not found on PATH. Install it and enable its app execution alias."); return false;
    }
    const wchar_t *resolved_terminal[]={wt};
    if (!sp_elevation_guard(p,wt,resolved_terminal,1,message,cap)) return false;
    argv[0]=wt;
    wchar_t container[SP_CONTAINER_CAP],title[SP_CONTAINER_CAP+16];
    if (is_container) {
        if (!p->container_filter || !*p->container_filter) {
            swprintf(message,cap,L"container-filter must be configured before container discovery."); return false;
        }
        if (!sp_find_executable(L"podman.exe",exe,SP_COMMAND_CAP)) {
            swprintf(message,cap,L"Podman CLI (podman.exe) was not found on PATH."); return false;
        }
        problem=sp_terminal_value_error(exe,true);
        if (problem) { swprintf(message,cap,L"%ls",problem); return false; }
        const wchar_t *query[]={exe,L"ps",L"--filter",p->container_filter,L"--format",L"{{.Names}}"};
        if (!sp_elevation_guard(p,exe,query,6,message,cap)) return false;
        SpProcessResult result;
        bool ok=sp_capture(exe,query,6,8000,cancel,&result);
        if (result.cancelled) return false;
        if (!ok || result.exit_code!=0) {
            wchar_t detail[400]={0};
            MultiByteToWideChar(CP_UTF8,0,result.err,(int)(result.err_size<350?result.err_size:350),detail,399);
            if (result.timed_out) swprintf(message,cap,L"Podman discovery timed out after 8 seconds; its process tree was stopped.");
            else if(result.truncated) swprintf(message,cap,L"Podman discovery exceeded the bounded output buffer; no container was launched.");
            else swprintf(message,cap,L"Podman discovery failed (exit %lu, Windows error %lu). %ls",
                (unsigned long)result.exit_code,(unsigned long)result.error,detail);
            return false;
        }
        char first[SP_CONTAINER_CAP]; size_t count=0;
        if (!sp_parse_containers(result.out,result.out_size,first,&count)) {
            swprintf(message,cap,L"Podman returned an invalid container-name list; no command was launched."); return false;
        }
        if (!count) { swprintf(message,cap,L"No running container matches container-filter. Start a matching container or change the filter."); return false; }
        MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,first,-1,container,SP_CONTAINER_CAP);
        swprintf(title,SP_CONTAINER_CAP+16,L"container: %ls",container);
        argv[argc++]=L"--title"; argv[argc++]=title;
        argv[argc++]=L"--"; argv[argc++]=exe; argv[argc++]=L"exec"; argv[argc++]=L"-it";
        if (p->container_directory) { argv[argc++]=L"-w"; argv[argc++]=p->container_directory; }
        argv[argc++]=container;
        if (b->argc) for (size_t i=0;i<b->argc;++i) argv[argc++]=b->args[i];
        else argv[argc++]=L"/bin/sh";
        if (count>1) swprintf(message,cap,L"Found %zu matching containers; opening %ls. Narrow container-filter to select another.",count,container);
    }
    /* Include argv[0]: Terminal scans even its own path for command delimiters. */
    for (size_t i=0;i<argc;++i) {
        problem=sp_terminal_value_error(argv[i],false);
        if (problem) { swprintf(message,cap,L"%ls",problem); return false; }
    }
    if (cancel && WaitForSingleObject(cancel,0)==WAIT_OBJECT_0) return false;
    if (!sp_elevation_guard(p,wt,argv,argc,message,cap)) return false;
    if (!sp_spawn(wt,argv,argc,&error)) {
        swprintf(message,cap,L"Cannot launch Windows Terminal (Windows error %lu).",(unsigned long)error); return false;
    }
    return true;
}
