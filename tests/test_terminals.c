#include "terminals.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
static unsigned checks,spawns,queries,finds;
static bool have_wt=true,have_podman=true,capture_ok=true,unsafe_wt_path,unsafe_podman_path;
static SpProcessResult fixture;
static const wchar_t *expected_filter=L"label=team=blue";
static wchar_t launch[32][1024];static size_t launch_count;
#define CHECK(x) do { ++checks; if (!(x)) { \
    fprintf(stderr,"%s:%d: %s\n",__FILE__,__LINE__,#x); exit(1); } } while(0)
/* Argument/boundary fixtures need a deterministic standard/medium observation,
   even on elevated runners or when their real token cannot be queried. Keep
   the production guard and default policy; test refusals in test_elevation.c.
   This sentinel is never a real token and must never reach a native API. */
static unsigned standard_token;
static BOOL WINAPI standard_open(HANDLE process,DWORD access,PHANDLE token) {
    CHECK(process==GetCurrentProcess() && access==TOKEN_QUERY);
    *token=(HANDLE)&standard_token;return TRUE;
}
static BOOL WINAPI standard_information(HANDLE token,TOKEN_INFORMATION_CLASS kind,LPVOID data,DWORD cap,PDWORD size) {
    CHECK(token==(HANDLE)&standard_token);
    CHECK(kind==TokenElevation || kind==TokenIntegrityLevel);
    *size=kind==TokenElevation?sizeof(TOKEN_ELEVATION):sizeof(TOKEN_MANDATORY_LABEL)+GetSidLengthRequired(1);
    if (!data || cap<*size) { SetLastError(ERROR_INSUFFICIENT_BUFFER);return FALSE; }
    memset(data,0,*size);
    if (kind==TokenIntegrityLevel) {
        TOKEN_MANDATORY_LABEL *label=data;
        SID_IDENTIFIER_AUTHORITY authority=SECURITY_MANDATORY_LABEL_AUTHORITY;
        label->Label.Sid=(BYTE *)data+sizeof(*label);label->Label.Attributes=SE_GROUP_INTEGRITY;
        CHECK(InitializeSid(label->Label.Sid,&authority,1));
        *GetSidSubAuthority(label->Label.Sid,0)=SECURITY_MANDATORY_MEDIUM_RID;
    }
    return TRUE;
}
static BOOL WINAPI standard_close(HANDLE token) { CHECK(token==(HANDLE)&standard_token);return TRUE; }
#define OpenProcessToken standard_open
#define GetTokenInformation standard_information
#define CloseHandle standard_close
#define sp_elevation_guard standard_guard
#include "../src/elevation.c"
#undef OpenProcessToken
#undef GetTokenInformation
#undef CloseHandle
#undef sp_elevation_guard
static void check_boundary(const wchar_t *exe,const wchar_t *const *args,size_t count);
static bool find_fixture(const wchar_t *name,wchar_t *out,size_t cap) {
    ++finds;
    if((!wcscmp(name,L"wt.exe")&&!have_wt)||(!wcscmp(name,L"podman.exe")&&!have_podman))return false;
    bool unsafe=(!wcscmp(name,L"wt.exe")&&unsafe_wt_path)||(!wcscmp(name,L"podman.exe")&&unsafe_podman_path);
    swprintf(out,cap,L"C:/%ls/%ls",unsafe?L"Fixture;Other":L"Fixture",name);return true;
}
static bool capture_fixture(const wchar_t *exe,const wchar_t *const *args,size_t n,DWORD timeout,HANDLE cancel,SpProcessResult *r) {
    (void)cancel;CHECK(wcsstr(exe,L"podman.exe"));CHECK(n==6 && timeout==8000);
    CHECK(!wcscmp(args[1],L"ps")&&!wcscmp(args[3],expected_filter)&&!wcscmp(args[5],L"{{.Names}}"));
    ++queries;*r=fixture;return capture_ok;
}
static bool spawn_fixture(const wchar_t *exe,const wchar_t *const *args,size_t n,DWORD *error) {
    CHECK(n<32);check_boundary(exe,args,n);++spawns;launch_count=n;*error=0;
    for(size_t i=0;i<n;++i){CHECK(wcslen(args[i])<1024);wcscpy(launch[i],args[i]);}return true;
}
#define sp_find_executable find_fixture
#define sp_capture capture_fixture
#define sp_spawn spawn_fixture
#define sp_elevation_guard standard_guard
#define sp_terminal_run fixture_terminal_run
#include "../src/terminals.c"
#undef sp_find_executable
#undef sp_capture
#undef sp_spawn
#undef sp_elevation_guard
#undef sp_terminal_run
static void output_fixture(const char *text) {
    memset(&fixture,0,sizeof(fixture));fixture.out_size=strlen(text);CHECK(fixture.out_size<SP_OUTPUT_CAP);
    strcpy(fixture.out,text);capture_ok=true;
}
#include "terminal_boundary.h"
static void test_container_directories(void) {
    static SpConfig config;sp_config_defaults(&config);
    SpProfile p=config.profiles[0];SpBinding b={0};wchar_t message[1024];
    CHECK(sp_never_elevate_all(&p));
    b.action=L"terminal.container";expected_filter=p.container_filter;output_fixture("demo\n");
    const wchar_t *bad[]={L".",L"..",L"app",L"../app",L"C:/app",L"C:\\app"};
    for (size_t i=0;i<sizeof(bad)/sizeof(bad[0]);++i) {
        p.container_directory=bad[i];
        unsigned found=finds,queried=queries,before=spawns;
        CHECK(!fixture_terminal_run(&p,&b,NULL,message,1024));
        CHECK(wcsstr(message,L"container-directory") && wcsstr(message,L"absolute POSIX"));
        CHECK(finds==found && queries==queried && spawns==before);
    }
    const wchar_t *good[]={NULL,L"/",L"/app/shared files",L"/app/snow-\u96ea files"};
    for (size_t i=0;i<sizeof(good)/sizeof(good[0]);++i) {
        p.container_directory=good[i];
        unsigned found=finds,queried=queries,before=spawns;
        CHECK(fixture_terminal_run(&p,&b,NULL,message,1024));
        CHECK(finds==found+2 && queries==queried+1 && spawns==before+1);
        if (good[i]) CHECK(launch_count==14 && !wcscmp(launch[10],L"-w") && !wcscmp(launch[11],good[i]));
        else CHECK(launch_count==12 && !wcscmp(launch[10],L"demo"));
    }
    expected_filter=L"label=team=blue";
}
static void test_public_terminal_options(void) {
    static SpConfig config;sp_config_defaults(&config);
    SpProfile p=config.profiles[0];SpBinding b={0};wchar_t message[1024];
    CHECK(sp_never_elevate_all(&p));
    b.action=L"terminal.open";unsigned queried=queries;
    CHECK(fixture_terminal_run(&p,&b,NULL,message,1024));
    CHECK(launch_count==4 && !wcscmp(launch[2],L"_quake") && !wcscmp(launch[3],L"nt"));
    CHECK(queries==queried); /* No Podman requirement for an ordinary terminal. */
    p.terminal_profile=L"Local Shell";
    CHECK(fixture_terminal_run(&p,&b,NULL,message,1024));
    CHECK(launch_count==6 && !wcscmp(launch[4],L"-p") && !wcscmp(launch[5],L"Local Shell"));
    p.terminal_profile=NULL;expected_filter=L"label=scratchpad=1";
    b.action=L"terminal.container";output_fixture("demo\n");
    CHECK(fixture_terminal_run(&p,&b,NULL,message,1024));
    CHECK(launch_count==12 && !wcscmp(launch[4],L"--title") && !wcscmp(launch[5],L"container: demo"));
    CHECK(!wcscmp(launch[6],L"--") && !wcscmp(launch[7],L"C:/Fixture/podman.exe"));
    CHECK(!wcscmp(launch[8],L"exec") && !wcscmp(launch[9],L"-it"));
    CHECK(!wcscmp(launch[10],L"demo") && !wcscmp(launch[11],L"/bin/sh"));
    p.terminal_profile=L"Local Shell";
    CHECK(fixture_terminal_run(&p,&b,NULL,message,1024));
    CHECK(launch_count==14 && !wcscmp(launch[5],L"Local Shell"));
    p.container_profile=L"Containers";p.container_directory=L"/app/shared files";
    b.argc=2;b.args[0]=L"/bin/bash";b.args[1]=L"-l";
    CHECK(fixture_terminal_run(&p,&b,NULL,message,1024));
    CHECK(launch_count==17 && !wcscmp(launch[5],L"Containers"));
    CHECK(!wcscmp(launch[12],L"-w") && !wcscmp(launch[13],p.container_directory));
    CHECK(!wcscmp(launch[14],L"demo") && !wcscmp(launch[15],L"/bin/bash") && !wcscmp(launch[16],L"-l"));
    b.argc=SP_MAX_ARGS;
    for (size_t i=0;i<b.argc;++i) b.args[i]=i?L"arg":L"/bin/sh";
    CHECK(fixture_terminal_run(&p,&b,NULL,message,1024));
    CHECK(launch_count==15+SP_MAX_ARGS && !wcscmp(launch[15],L"/bin/sh"));
    for (size_t i=1;i<b.argc;++i) CHECK(!wcscmp(launch[15+i],L"arg"));
    queried=queries;unsigned before=spawns;b.argc=SP_MAX_ARGS+1;
    CHECK(!fixture_terminal_run(&p,&b,NULL,message,1024));CHECK(queries==queried && spawns==before);
    b.argc=0;p.terminal_profile=L"";
    CHECK(!fixture_terminal_run(&p,&b,NULL,message,1024));CHECK(queries==queried && spawns==before);
    p.terminal_profile=NULL;p.container_filter=NULL;
    CHECK(!fixture_terminal_run(&p,&b,NULL,message,1024));CHECK(queries==queried && spawns==before && wcsstr(message,L"container-filter"));
    p.container_filter=L"label=team=blue;literal";expected_filter=p.container_filter;
    CHECK(fixture_terminal_run(&p,&b,NULL,message,1024)); /* Filter is not Terminal syntax. */
    expected_filter=L"label=team=blue";
}
int main(void) {
    test_parser_witnesses();
    SpProfile p={0};p.quake_window=L"my-quake";p.terminal_profile=L"My Terminal";p.container_profile=L"Team Containers";p.container_filter=L"label=team=blue";
    SpBinding b={0};wchar_t message[1024];b.action=L"terminal.open";
    CHECK(fixture_terminal_run(&p,&b,NULL,message,1024));CHECK(spawns==1 && launch_count==6);
    CHECK(!wcscmp(launch[2],L"my-quake")&&!wcscmp(launch[5],L"My Terminal"));
    b.action=L"terminal.quake";b.argc=4;b.args[0]=L"Ubuntu";b.args[1]=L"wsl.exe";b.args[2]=L"-d";b.args[3]=L"Team Linux";
    CHECK(fixture_terminal_run(&p,&b,NULL,message,1024));CHECK(launch_count==10 && !wcscmp(launch[6],L"--") && !wcscmp(launch[9],L"Team Linux"));
    b.action=L"terminal.container";b.argc=0;have_wt=false;unsigned before=spawns;
    CHECK(!fixture_terminal_run(&p,&b,NULL,message,1024) && wcsstr(message,L"Terminal"));CHECK(spawns==before);
    have_wt=true;have_podman=false;
    CHECK(!fixture_terminal_run(&p,&b,NULL,message,1024) && wcsstr(message,L"Podman CLI"));have_podman=true;
    output_fixture("");CHECK(!fixture_terminal_run(&p,&b,NULL,message,1024)&&wcsstr(message,L"No running"));CHECK(spawns==before);
    output_fixture("container-one\r\n");CHECK(fixture_terminal_run(&p,&b,NULL,message,1024));
    CHECK(launch_count==14&&!wcscmp(launch[5],L"Team Containers")&&!wcscmp(launch[7],L"container: container-one"));
    CHECK(!wcscmp(launch[8],L"--")&&!wcscmp(launch[9],L"C:/Fixture/podman.exe")&&!wcscmp(launch[12],L"container-one")&&!wcscmp(launch[13],L"/bin/sh"));
    output_fixture("one\ntwo\n");CHECK(fixture_terminal_run(&p,&b,NULL,message,1024)&&wcsstr(message,L"2 matching"));
    CHECK(!wcscmp(launch[12],L"one"));before=spawns;
    output_fixture("unsafe;command\n");CHECK(!fixture_terminal_run(&p,&b,NULL,message,1024)&&wcsstr(message,L"invalid"));CHECK(spawns==before);
    output_fixture("");fixture.exit_code=125;strcpy(fixture.err,"fixture error");fixture.err_size=strlen(fixture.err);
    CHECK(!fixture_terminal_run(&p,&b,NULL,message,1024)&&wcsstr(message,L"125")&&wcsstr(message,L"fixture error"));
    capture_ok=false;fixture.timed_out=true;
    CHECK(!fixture_terminal_run(&p,&b,NULL,message,1024)&&wcsstr(message,L"timed out"));
    fixture.timed_out=false;fixture.truncated=true;
    CHECK(!fixture_terminal_run(&p,&b,NULL,message,1024)&&wcsstr(message,L"bounded"));
    HANDLE cancel=CreateEventW(NULL,TRUE,TRUE,NULL);CHECK(cancel);
    CHECK(!fixture_terminal_run(&p,&b,cancel,message,1024));CHECK(spawns==before);CloseHandle(cancel);
    b.action=L"process.exec";b.argc=3;b.args[0]=L"custom-module.exe";b.args[1]=L"one & two";b.args[2]=L"a\"b\\";
    CHECK(fixture_terminal_run(&p,&b,NULL,message,1024));CHECK(launch_count==3 && !wcscmp(launch[1],L"one & two")&&!wcscmp(launch[2],L"a\"b\\"));
    CHECK(queries==7);
    test_restrictions(p);
    test_public_terminal_options();
    test_container_directories();
    printf("terminal actions: %u checks passed (CRT/Terminal parser-boundary fixtures; no actual terminal/container launches)\n",checks);return 0;
}
