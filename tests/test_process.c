#include "process.h"
#include <shellapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

static unsigned checks;
#define CHECK(x) do { ++checks; if (!(x)) { \
    fprintf(stderr,"%s:%d: %s\n",__FILE__,__LINE__,#x); exit(1); } } while(0)
/* Force output to arrive after both pre-exit pipe peeks, before the exit wait. */
static HANDLE tail_gate;
static bool release_tail;
static DWORD WINAPI tail_wait(HANDLE h,DWORD ms) {
    if (release_tail && ms==0) {
        release_tail=false;
        CHECK(SetEvent(tail_gate));
        DWORD result=WaitForSingleObject(h,5000);
        CHECK(result==WAIT_OBJECT_0);
        return result;
    }
    return WaitForSingleObject(h,ms);
}
#define sp_commandline tail_commandline
#define sp_find_executable tail_find_executable
#define sp_capture tail_capture
#define sp_spawn tail_spawn
#define sp_parse_containers tail_parse_containers
#define WaitForSingleObject tail_wait
#include "../src/process.c"
#undef sp_commandline
#undef sp_find_executable
#undef sp_capture
#undef sp_spawn
#undef sp_parse_containers
#undef WaitForSingleObject

/* Exercise the terminal resolver without ever launching an executable. */
static unsigned spawn_attempts;
static bool blocked_spawn(const wchar_t *exe,const wchar_t *const *args,size_t count,DWORD *error) {
    (void)exe; (void)args; (void)count; (void)error;
    ++spawn_attempts; return true;
}
#define sp_spawn blocked_spawn
#define sp_terminal_run checked_terminal_run
#include "../src/terminals.c"
#undef sp_spawn
#undef sp_terminal_run

static int write_tail(size_t size) {
    char out[6000],err[6000]; DWORD written;
    memset(out,'o',sizeof(out)); memset(err,'e',sizeof(err));
    if (!WriteFile(GetStdHandle(STD_OUTPUT_HANDLE),out,(DWORD)size,&written,NULL) || written!=size) return 11;
    if (!WriteFile(GetStdHandle(STD_ERROR_HANDLE),err,(DWORD)size,&written,NULL) || written!=size) return 12;
    return 0;
}
int wmain(int argc, wchar_t **argv) {
    if (argc>1) {
        if (!wcscmp(argv[1],L"--tail")) {
            HANDLE gate=OpenEventW(SYNCHRONIZE,FALSE,argv[2]);
            if (!gate || WaitForSingleObject(gate,5000)!=WAIT_OBJECT_0) return 13;
            CloseHandle(gate); return write_tail(128);
        }
        if (!wcscmp(argv[1],L"--tail-burst")) return write_tail(6000);
        if (!wcscmp(argv[1],L"--ok")) { fputs("hello\n",stdout); fputs("diagnostic\n",stderr); return 7; }
        if (!wcscmp(argv[1],L"--sleep")) { Sleep(5000); return 0; }
        if (!wcscmp(argv[1],L"--flood")) { for(int i=0;i<20000;++i) putchar('x'); return 0; }
        if (!wcscmp(argv[1],L"--args")) {
            CHECK(argc==5); CHECK(!wcscmp(argv[2],L"one & two"));
            CHECK(!wcscmp(argv[3],L"a\"b\\")); CHECK(!wcscmp(argv[4],L"")); return 0;
        }
        return 10;
    }
    const wchar_t *args[]={L"program.exe",L"",L"plain",L"a b",L"a\"b",L"x\\",L"a\\\\\"b",L"&|<>;%^!",L"\u96ea"};
    wchar_t command[2048];
    CHECK(sp_commandline(command,2048,args,sizeof(args)/sizeof(args[0])));
    int n=0; LPWSTR *parsed=CommandLineToArgvW(command,&n);
    CHECK(parsed && n==(int)(sizeof(args)/sizeof(args[0])));
    for (int i=0;i<n;++i) CHECK(!wcscmp(parsed[i],args[i]));
    LocalFree(parsed);
    CHECK(!sp_commandline(command,2,args,1)); CHECK(command[0]==0);
    CHECK(!sp_commandline(command,2048,NULL,0));
    char first[SP_CONTAINER_CAP]; size_t count;
    const char *names=" \r\n container-one\r\ncontainer.two_2\n";
    CHECK(sp_parse_containers(names,strlen(names),first,&count));
    CHECK(count==2 && !strcmp(first,"container-one"));
    CHECK(sp_parse_containers("\r\n",2,first,&count) && count==0);
    CHECK(!sp_parse_containers("bad;cmd",7,first,&count));
    CHECK(!sp_parse_containers("--option",8,first,&count));
    CHECK(!sp_parse_containers("x\0y",3,first,&count));
    CHECK(!sp_parse_containers("x y",3,first,&count));
    char long_name[SP_CONTAINER_CAP+1]; memset(long_name,'a',sizeof(long_name));
    CHECK(!sp_parse_containers(long_name,sizeof(long_name),first,&count));
    wchar_t exe[32768]; DWORD len=GetModuleFileNameW(NULL,exe,32768); CHECK(len>0 && len<32768);
    SpProcessResult r;
    wchar_t gate_name[100];
    swprintf(gate_name,100,L"Local\\win-scratchpad-tail-%lu",(unsigned long)GetCurrentProcessId());
    tail_gate=CreateEventW(NULL,TRUE,FALSE,gate_name); CHECK(tail_gate);
    const wchar_t *tail[]={exe,L"--tail",gate_name};
    release_tail=true;
    CHECK(tail_capture(exe,tail,3,5000,NULL,&r));
    CloseHandle(tail_gate); tail_gate=NULL;
    CHECK(!release_tail && r.exit_code==0);
    CHECK(r.out_size==128 && r.err_size==128);
    for (size_t i=0;i<128;++i) CHECK(r.out[i]=='o' && r.err[i]=='e');
    const wchar_t *burst[]={exe,L"--tail-burst"};
    for (unsigned iteration=0;iteration<40;++iteration) {
        CHECK(sp_capture(exe,burst,2,5000,NULL,&r) && r.exit_code==0);
        CHECK(r.out_size==6000 && r.err_size==6000);
        CHECK(r.out[5999]=='o' && r.err[5999]=='e');
    }
    /* A drive-relative executable exists, but must be rejected before spawn. */
    wchar_t cwd[32768],directory[32768],relative[32768],message[512];
    CHECK(GetCurrentDirectoryW(32768,cwd));
    wcscpy(directory,exe); wchar_t *base=wcsrchr(directory,L'\\'); CHECK(base);
    *base++=0;
    swprintf(relative,32768,L"%lc:%ls",exe[0],base);
    CHECK(SetCurrentDirectoryW(directory));
    static SpProfile profile;
    SpBinding binding={0}; binding.action=L"process.exec"; binding.argc=1; binding.args[0]=relative;
    bool relative_ok=checked_terminal_run(&profile,&binding,NULL,message,512);
    CHECK(SetCurrentDirectoryW(cwd));
    CHECK(!relative_ok && spawn_attempts==0 && message[0]);
    binding.args[0]=exe;
    CHECK(checked_terminal_run(&profile,&binding,NULL,message,512) && spawn_attempts==1);
    const wchar_t *ok[]={exe,L"--ok"};
    CHECK(sp_capture(exe,ok,2,5000,NULL,&r)); CHECK(r.exit_code==7 && r.error==0);
    CHECK(!strcmp(r.out,"hello\r\n") || !strcmp(r.out,"hello\n"));
    CHECK(strstr(r.err,"diagnostic") && r.out_size==strlen(r.out));
    const wchar_t *argtest[]={exe,L"--args",L"one & two",L"a\"b\\",L""};
    CHECK(sp_capture(exe,argtest,5,5000,NULL,&r) && r.exit_code==0);
    const wchar_t *slow[]={exe,L"--sleep"};
    ULONGLONG start=GetTickCount64();
    CHECK(!sp_capture(exe,slow,2,100,NULL,&r)); CHECK(r.timed_out && r.error==ERROR_TIMEOUT);
    CHECK(GetTickCount64()-start<2500);
    HANDLE cancel=CreateEventW(NULL,TRUE,TRUE,NULL); CHECK(cancel);
    CHECK(!sp_capture(exe,slow,2,5000,cancel,&r) && r.cancelled);
    CloseHandle(cancel);
    const wchar_t *flood[]={exe,L"--flood"};
    CHECK(!sp_capture(exe,flood,2,5000,NULL,&r) && r.truncated);
    CHECK(r.out_size==SP_OUTPUT_CAP-1);
    CHECK(!sp_capture(L"Z:\\missing\\podman.exe",ok,2,5000,NULL,&r));
    CHECK(r.error==ERROR_FILE_NOT_FOUND || r.error==ERROR_PATH_NOT_FOUND || r.error==ERROR_INVALID_DRIVE);
    /* PATH discovery must reject a bare relative search directory. */
    DWORD path_size=GetEnvironmentVariableW(L"PATH",NULL,0);
    wchar_t *saved=calloc(path_size,sizeof(*saved)); CHECK(saved);
    CHECK(GetEnvironmentVariableW(L"PATH",saved,path_size));
    CHECK(SetEnvironmentVariableW(L"PATH",L".;relative;\"relative path\""));
    CHECK(!sp_find_executable(L"test_process.exe",command,2048));
    CHECK(SetEnvironmentVariableW(L"PATH",saved)); free(saved);
    CHECK(!sp_find_executable(L"../cmd.exe",command,2048));
    printf("process: %u checks passed (native children, no shell/Podman/Terminal invoked)\n",checks); return 0;
}
