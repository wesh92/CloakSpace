#include "process.h"
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

static bool put(wchar_t *out, size_t cap, size_t *used, wchar_t c) {
    if (*used+1>=cap) return false;
    out[(*used)++]=c; return true;
}
bool sp_commandline(wchar_t *out, size_t cap, const wchar_t *const *argv, size_t argc) {
    if (!out || !cap || !argv || !argc) return false;
    size_t n=0; out[0]=0;
    for (size_t i=0;i<argc;++i) {
        if (!argv[i]) goto bad;
        if (i && !put(out,cap,&n,L' ')) goto bad;
        if (!put(out,cap,&n,L'"')) goto bad;
        const wchar_t *p=argv[i];
        for (;;) {
            size_t slashes=0;
            while (*p==L'\\') { ++slashes; ++p; }
            size_t copies=slashes;
            if (*p==L'"' || !*p) copies*=2;
            for (size_t j=0;j<copies;++j) if (!put(out,cap,&n,L'\\')) goto bad;
            if (!*p) break;
            if (*p==L'"' && !put(out,cap,&n,L'\\')) goto bad;
            if (!put(out,cap,&n,*p++)) goto bad;
        }
        if (!put(out,cap,&n,L'"')) goto bad;
    }
    out[n]=0; return true;
bad:
    out[0]=0; return false;
}
static bool absolute(const wchar_t *p) {
    return (p[0] && p[1]==L':' && (p[2]==L'\\'||p[2]==L'/')) ||
           (p[0]==L'\\' && p[1]==L'\\');
}
bool sp_find_executable(const wchar_t *name, wchar_t *out, size_t cap) {
    if (!name || !*name || wcspbrk(name,L"\\/:\"") || !out || cap<2) return false;
    DWORD size=GetEnvironmentVariableW(L"PATH",NULL,0);
    if (!size) return false;
    wchar_t *path=calloc(size,sizeof(*path));
    if (!path) return false;
    DWORD got=GetEnvironmentVariableW(L"PATH",path,size);
    if (!got || got>=size) { free(path); return false; }
    bool found=false;
    for (wchar_t *p=path; *p;) {
        wchar_t *end=wcschr(p,L';'); if (!end) end=p+wcslen(p);
        wchar_t saved=*end; *end=0;
        size_t n=wcslen(p);
        if (n>=2 && p[0]==L'"' && p[n-1]==L'"') { p[n-1]=0; ++p; }
        if (absolute(p) && wcslen(p)+wcslen(name)+2<=cap) {
            size_t len=wcslen(p); memcpy(out,p,len*sizeof(*out));
            out[len++]=L'\\'; wcscpy(out+len,name);
            DWORD attr=GetFileAttributesW(out);
            if (attr!=INVALID_FILE_ATTRIBUTES && !(attr&FILE_ATTRIBUTE_DIRECTORY)) { found=true; break; }
        }
        if (!saved) break;
        p=end+1;
    }
    free(path); if (!found) out[0]=0; return found;
}
static void close_handle(HANDLE *h) {
    if (*h && *h!=INVALID_HANDLE_VALUE) CloseHandle(*h);
    *h=NULL;
}
static bool drain(HANDLE pipe, char *buf, size_t *size, bool *read_any, SpProcessResult *r) {
    DWORD avail=0,got=0; char chunk[4096];
    if (!PeekNamedPipe(pipe,NULL,0,NULL,&avail,NULL)) {
        DWORD error=GetLastError();
        if (error==ERROR_BROKEN_PIPE) return true;
        r->error=error; return false;
    }
    if (!avail) return true;
    if (!ReadFile(pipe,chunk,avail<sizeof(chunk)?avail:(DWORD)sizeof(chunk),&got,NULL)) {
        r->error=GetLastError(); return false;
    }
    *read_any=got!=0;
    size_t room=SP_OUTPUT_CAP-1-*size, copy=got<room?got:room;
    memcpy(buf+*size,chunk,copy); *size+=copy; buf[*size]=0;
    if (got>room) { r->truncated=true; r->error=ERROR_BUFFER_OVERFLOW; return false; }
    return true;
}
bool sp_capture(const wchar_t *exe, const wchar_t *const *argv, size_t argc,
                DWORD timeout_ms, HANDLE cancel, SpProcessResult *r) {
    if (!r) return false;
    ZeroMemory(r,sizeof(*r)); r->exit_code=STILL_ACTIVE;
    wchar_t command[SP_COMMAND_CAP];
    if (!exe || !absolute(exe) || !timeout_ms || !sp_commandline(command,SP_COMMAND_CAP,argv,argc)) {
        r->error=ERROR_INVALID_PARAMETER; return false;
    }
    HANDLE out_r=NULL,out_w=NULL,err_r=NULL,err_w=NULL,input=NULL,job=NULL;
    PROCESS_INFORMATION pi={0}; STARTUPINFOEXW si={0}; SIZE_T bytes=0;
    bool ok=false,started=false;
    SECURITY_ATTRIBUTES sa={sizeof(sa),NULL,TRUE};
    if (!CreatePipe(&out_r,&out_w,&sa,0) || !CreatePipe(&err_r,&err_w,&sa,0) ||
        !SetHandleInformation(out_r,HANDLE_FLAG_INHERIT,0) ||
        !SetHandleInformation(err_r,HANDLE_FLAG_INHERIT,0)) goto error;
    input=CreateFileW(L"NUL",GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE,&sa,OPEN_EXISTING,0,NULL);
    if (input==INVALID_HANDLE_VALUE) goto error;
    si.StartupInfo.cb=sizeof(si); si.StartupInfo.dwFlags=STARTF_USESTDHANDLES;
    si.StartupInfo.hStdInput=input; si.StartupInfo.hStdOutput=out_w; si.StartupInfo.hStdError=err_w;
    InitializeProcThreadAttributeList(NULL,1,0,&bytes);
    si.lpAttributeList=HeapAlloc(GetProcessHeap(),0,bytes);
    if (!si.lpAttributeList) { SetLastError(ERROR_NOT_ENOUGH_MEMORY); goto error; }
    if (!InitializeProcThreadAttributeList(si.lpAttributeList,1,0,&bytes)) {
        HeapFree(GetProcessHeap(),0,si.lpAttributeList); si.lpAttributeList=NULL; goto error;
    }
    HANDLE handles[]={input,out_w,err_w};
    if (!UpdateProcThreadAttribute(si.lpAttributeList,0,PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
        handles,sizeof(handles),NULL,NULL)) goto error;
    job=CreateJobObjectW(NULL,NULL); if (!job) goto error;
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits={0};
    limits.BasicLimitInformation.LimitFlags=JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!SetInformationJobObject(job,JobObjectExtendedLimitInformation,&limits,sizeof(limits))) goto error;
    if (!CreateProcessW(exe,command,NULL,NULL,TRUE,
        CREATE_NO_WINDOW|CREATE_SUSPENDED|EXTENDED_STARTUPINFO_PRESENT,NULL,NULL,&si.StartupInfo,&pi)) goto error;
    started=true;
    close_handle(&out_w); close_handle(&err_w); close_handle(&input);
    if (!AssignProcessToJobObject(job,pi.hProcess)) goto error;
    if (ResumeThread(pi.hThread)==(DWORD)-1) goto error;
    ULONGLONG start=GetTickCount64();
    for (;;) {
        if (cancel && WaitForSingleObject(cancel,0)==WAIT_OBJECT_0) {
            r->cancelled=true; r->error=ERROR_CANCELLED; goto done;
        }
        if (GetTickCount64()-start>=timeout_ms) {
            r->timed_out=true; r->error=ERROR_TIMEOUT; goto done;
        }
        bool a=false,b=false;
        if (!drain(out_r,r->out,&r->out_size,&a,r) || !drain(err_r,r->err,&r->err_size,&b,r)) goto done;
        DWORD wait=WaitForSingleObject(pi.hProcess,0);
        if (wait==WAIT_FAILED) goto error;
        if (wait==WAIT_OBJECT_0) {
            /* Exit may follow the peeks above but precede this wait. Read again
               after observing exit, until both pipes are empty. Output caps
               bound this even if a descendant still holds a write handle. */
            do {
                a=false; b=false;
                if (!drain(out_r,r->out,&r->out_size,&a,r) ||
                    !drain(err_r,r->err,&r->err_size,&b,r)) goto done;
            } while (a || b);
            break;
        }
        if (!a && !b) Sleep(2);
    }
    if (!GetExitCodeProcess(pi.hProcess,&r->exit_code)) goto error;
    ok=true; goto done;
error:
    r->error=GetLastError();
done:
    if (started && !ok) {
        if (job) TerminateJobObject(job,r->error);
        /* Also covers AssignProcessToJobObject failing before membership in the job. */
        TerminateProcess(pi.hProcess,r->error);
        WaitForSingleObject(pi.hProcess,1000);
        GetExitCodeProcess(pi.hProcess,&r->exit_code);
    }
    if (si.lpAttributeList) {
        DeleteProcThreadAttributeList(si.lpAttributeList);
        HeapFree(GetProcessHeap(),0,si.lpAttributeList);
    }
    close_handle(&out_r); close_handle(&out_w); close_handle(&err_r); close_handle(&err_w);
    close_handle(&input); close_handle(&pi.hThread); close_handle(&pi.hProcess); close_handle(&job);
    return ok;
}
bool sp_spawn(const wchar_t *exe, const wchar_t *const *argv, size_t argc, DWORD *error) {
    wchar_t command[SP_COMMAND_CAP]; PROCESS_INFORMATION pi={0}; STARTUPINFOW si={0};
    if (error) *error=0;
    if (!exe || !absolute(exe) || !sp_commandline(command,SP_COMMAND_CAP,argv,argc)) {
        if (error) *error=ERROR_INVALID_PARAMETER;
        return false;
    }
    si.cb=sizeof(si);
    if (!CreateProcessW(exe,command,NULL,NULL,FALSE,0,NULL,NULL,&si,&pi)) {
        if (error) *error=GetLastError();
        return false;
    }
    CloseHandle(pi.hThread); CloseHandle(pi.hProcess); return true;
}
static bool name_char(unsigned char c) {
    return (c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='_'||c=='.'||c=='-';
}
bool sp_parse_containers(const char *text, size_t length, char first[SP_CONTAINER_CAP], size_t *count) {
    if (!text || !first || !count) return false;
    *count=0; first[0]=0;
    for (size_t i=0;i<length;) {
        size_t start=i;
        while (i<length && text[i]!='\n' && text[i]!='\r') ++i;
        size_t end=i;
        while (start<end && (text[start]==' '||text[start]=='\t')) ++start;
        while (end>start && (text[end-1]==' '||text[end-1]=='\t')) --end;
        if (end>start) {
            size_t n=end-start;
            unsigned char c=(unsigned char)text[start];
            if (n>=SP_CONTAINER_CAP || !name_char(c) || c=='_'||c=='.'||c=='-') return false;
            for (size_t j=start;j<end;++j) if (!name_char((unsigned char)text[j])) return false;
            if (!*count) { memcpy(first,text+start,n); first[n]=0; }
            ++*count;
        }
        while (i<length && (text[i]=='\r'||text[i]=='\n')) ++i;
    }
    return true;
}
