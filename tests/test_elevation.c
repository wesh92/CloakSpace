#include "elevation.h"
#include "config_file.h"
#include "terminals.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

static unsigned checks,spawns,captures,lookups,token_queries;
static unsigned parsed_denials,parsed_raw_denials,parsed_permitted;
static bool token_high,integrity_high,fail_open,fail_elevation,fail_integrity;
static DWORD spawn_error;
static wchar_t message[1024];
static SpConfig config;
#define CHECK(x) do { ++checks; if (!(x)) { \
    fprintf(stderr,"%s:%d: %s\n",__FILE__,__LINE__,#x); exit(1); } } while(0)

/* Simulate privilege *observations*, never real elevation/impersonation. The
   token remains an ordinary real query handle, including its real cleanup. */
static BOOL WINAPI observed_open(HANDLE process,DWORD access,PHANDLE token) {
    ++token_queries; CHECK(process==GetCurrentProcess() && access==TOKEN_QUERY);
    if (fail_open) { SetLastError(ERROR_ACCESS_DENIED); return FALSE; }
    return OpenProcessToken(process,access,token);
}
static BOOL WINAPI observed_information(HANDLE token,TOKEN_INFORMATION_CLASS kind,LPVOID data,DWORD cap,PDWORD size) {
    if ((fail_elevation && kind==TokenElevation)||(fail_integrity && kind==TokenIntegrityLevel)) {
        *size=0; SetLastError(ERROR_ACCESS_DENIED); return FALSE;
    }
    BOOL ok=GetTokenInformation(token,kind,data,cap,size);
    if (ok && kind==TokenElevation) ((TOKEN_ELEVATION *)data)->TokenIsElevated=token_high;
    if (ok && kind==TokenIntegrityLevel) {
        TOKEN_MANDATORY_LABEL *label=data; BYTE n=*GetSidSubAuthorityCount(label->Label.Sid);
        CHECK(n>0); *GetSidSubAuthority(label->Label.Sid,n-1)=integrity_high?SECURITY_MANDATORY_HIGH_RID:SECURITY_MANDATORY_MEDIUM_RID;
    }
    return ok;
}
#define OpenProcessToken observed_open
#define GetTokenInformation observed_information
#define sp_elevation_guard observed_guard
#include "../src/elevation.c"
#undef OpenProcessToken
#undef GetTokenInformation
#undef sp_elevation_guard

/* These are the exact production action/guard functions; the process boundary
   is a counting fixture. No broker, shell, Terminal, container or UAC is run. */
static bool find_observed(const wchar_t *name,wchar_t *out,size_t cap) {
    ++lookups; swprintf(out,cap,L"C:/Fixture/%ls",name); return true;
}
static DWORD WINAPI attributes_observed(LPCWSTR path) { (void)path; return FILE_ATTRIBUTE_NORMAL; }
static bool spawn_observed(const wchar_t *exe,const wchar_t *const *args,size_t count,DWORD *error) {
    CHECK(exe && args && count); ++spawns; *error=spawn_error; return !spawn_error;
}
static bool capture_observed(const wchar_t *exe,const wchar_t *const *args,size_t count,DWORD ms,HANDLE cancel,SpProcessResult *result) {
    (void)cancel; CHECK(exe && args && count==6 && ms==8000); ++captures;
    memset(result,0,sizeof(*result)); strcpy(result->out,"fixture-one\nfixture-two\n"); result->out_size=strlen(result->out); return true;
}
#define sp_find_executable find_observed
#define GetFileAttributesW attributes_observed
#define sp_spawn spawn_observed
#define sp_capture capture_observed
#define sp_elevation_guard observed_guard
#define sp_terminal_run observed_run
#include "../src/terminals.c"
#undef sp_find_executable
#undef GetFileAttributesW
#undef sp_spawn
#undef sp_capture
#undef sp_elevation_guard
#undef sp_terminal_run

/* A second, uninstrumented production action is used ONLY with this test
   executable's harmless --probe mode. Never pass a broker fixture to it. */
#define resolve native_resolve
#define sp_terminal_run native_run
#include "../src/terminals.c"
#undef resolve
#undef sp_terminal_run

static void load(const wchar_t *text) {
    wchar_t error[512]; CHECK(sp_config_parse(&config,text,error,512));
}
static void scoped(void) {
    load(L"never-elevate\nglobal-tool.exe\n\"C:/Shared Ops/\u96ea.ps1\"\n"
         L"\"//Server/Share/ops/locked.vbs\"\nend-never-elevate\n"
         L"profile default\nnever-elevate\nlocal-tool\nC:/Work/locked.py\nend-never-elevate\n"
         L"bind Win+S scratch.toggle\nend\nprofile other\nbind Win+S scratch.toggle\nend\n"
         L"never-elevate\nlate.exe\nend-never-elevate\n");
}
static void request(const SpProfile *p,const wchar_t *const *args,size_t count,bool allow,const wchar_t *diagnostic) {
    wcscpy(message,L"retained information");
    bool actual=observed_guard(p,args[0],args,count,message,1024);
    if (actual!=allow) {
        fprintf(stderr,"guard unexpectedly %s:",actual?"allowed":"blocked");
        for (size_t i=0;i<count;++i) fprintf(stderr," [%ls]",args[i]);
        fputc('\n',stderr);
    }
    CHECK(actual==allow);
    if (allow) CHECK(!wcscmp(message,L"retained information"));
    else { CHECK(wcsstr(message,L"Elevation blocked") && wcsstr(message,L"never-elevate")); if(diagnostic)CHECK(wcsstr(message,diagnostic)); }
}
#define REQUEST(p,allow,diagnostic,...) do { const wchar_t *a[]={__VA_ARGS__}; request(p,a,sizeof(a)/sizeof(*a),allow,diagnostic); } while(0)
static void action(const SpProfile *p,const wchar_t *name,const wchar_t *const *args,size_t count,bool allow,const wchar_t *diagnostic) {
    SpBinding b={0}; b.action=name; b.argc=count; CHECK(count<=SP_MAX_ARGS);
    for (size_t i=0;i<count;++i) b.args[i]=args[i];
    unsigned before_spawn=spawns,before_capture=captures;
    CHECK(observed_run(p,&b,NULL,message,1024)==allow);
    if (allow) CHECK(spawns==before_spawn+1);
    else {
        CHECK(spawns==before_spawn && captures==before_capture);
        CHECK(wcsstr(message,L"Elevation blocked") && wcsstr(message,L"never-elevate"));
        if (diagnostic) CHECK(wcsstr(message,diagnostic));
    }
}
#define ACTION(p,name,allow,diagnostic,...) do { const wchar_t *a[]={__VA_ARGS__}; action(p,name,a,sizeof(a)/sizeof(*a),allow,diagnostic); } while(0)
static void actionable_denial(const SpProfile *p,const wchar_t *diagnostic) {
    wchar_t active[128];
    CHECK(swprintf(active,128,L"Active profile '%ls'",p->name)>0);
    CHECK(wcsstr(message,L"Elevation blocked") && wcsstr(message,L"never-elevate"));
    CHECK(wcsstr(message,L"Run normally from a non-elevated scratchpad"));
    CHECK(wcsstr(message,L"remove the elevation request"));
    CHECK(wcsstr(message,active));
    if (diagnostic) CHECK(wcsstr(message,diagnostic));
}
static void test_scopes_and_names(void) {
    scoped(); const SpProfile *p=&config.profiles[0],*other=&config.profiles[1];
    REQUEST(p,true,NULL,L"global-tool.exe");
    REQUEST(p,true,NULL,L"python.exe",L"C:/Work/locked.py");
    ACTION(p,L"process.exec",true,NULL,L"GLOBAL-TOOL.EXE");
    ACTION(p,L"process.exec",true,NULL,L"python.exe",L"C:/Work/locked.py");
    token_high=true;
    const wchar_t *variants[]={L"global-tool",L"GLOBAL-TOOL.EXE",L"C:/Tools/GLOBAL-TOOL.exe",L"C:\\Tools\\Global-Tool.Exe. "};
    for (size_t i=0;i<sizeof(variants)/sizeof(*variants);++i) {
        REQUEST(p,false,L"global-tool.exe",variants[i]); REQUEST(other,false,L"global-tool.exe",variants[i]);
        ACTION(p,L"process.exec",false,L"global-tool.exe",variants[i]);
    }
    REQUEST(p,false,L"local-tool",L"local-tool.EXE"); REQUEST(other,true,NULL,L"local-tool.EXE");
    ACTION(p,L"process.exec",false,L"local-tool",L"local-tool.exe");
    ACTION(other,L"process.exec",true,NULL,L"local-tool.exe");
    REQUEST(p,false,L"late.exe",L"late"); REQUEST(other,false,L"late.exe",L"late.EXE");
    REQUEST(p,true,NULL,L"not-global-tool.exe"); REQUEST(p,true,NULL,L"global-tool.exe.bak");
    REQUEST(p,true,NULL,L"editor.exe",L"global-tool.exe",L"-Verb",L"RunAs");
    token_high=false; integrity_high=true;
    REQUEST(p,false,L"global-tool.exe",L"global-tool.exe");
    integrity_high=false;
}
static void test_full_paths(void) {
    scoped(); const SpProfile *p=&config.profiles[0],*other=&config.profiles[1]; token_high=true;
    const wchar_t *paths[]={L"C:/Shared Ops/\u96ea.ps1",L"c:\\SHARED OPS\\\u96ea.PS1",L"C:/Shared Ops/child/.././\u96ea.ps1",L"C:/Shared Ops./\u96ea.ps1. "};
    for (size_t i=0;i<sizeof(paths)/sizeof(*paths);++i) {
        REQUEST(p,false,L"Shared Ops",L"pwsh.exe",L"-File",paths[i]);
        REQUEST(other,false,L"Shared Ops",L"pwsh.exe",L"-File",paths[i]);
        ACTION(p,L"process.exec",false,L"Shared Ops",L"pwsh.exe",L"-File",paths[i]);
    }
    REQUEST(p,false,L"locked.py",L"python.exe",L"C:/Work/locked.py");
    REQUEST(other,true,NULL,L"python.exe",L"C:/Work/locked.py");
    REQUEST(p,false,L"Server/Share",L"wscript.exe",L"\\\\SERVER\\share\\ops\\locked.VBS");
    REQUEST(p,false,L"locked.py",L"C:/Work/locked.py");
    REQUEST(p,true,NULL,L"python.exe",L"C:/Other/locked.py");
    REQUEST(p,true,NULL,L"python.exe",L"C:/Work/locked.py.bak");
    REQUEST(p,true,NULL,L"python.exe",L"C:/Work/locked.py/another.py");
    REQUEST(p,false,L"cannot safely",L"pwsh.exe",L"-File",L"\\\\?\\C:\\Work\\locked.py");
    token_high=false;
    REQUEST(p,true,NULL,L"pwsh.exe",L"-File",L"c:\\SHARED OPS\\\u96ea.PS1");
    ACTION(p,L"process.exec",true,NULL,L"pwsh.exe",L"-File",L"C:/Shared Ops/\u96ea.ps1");
}
static void test_explicit_brokers(void) {
    scoped(); const SpProfile *p=&config.profiles[0],*other=&config.profiles[1];
    const wchar_t *brokers[]={L"sudo.exe",L"GSUDO",L"C:/Tools/gsudo.exe"};
    for (size_t i=0;i<sizeof(brokers)/sizeof(*brokers);++i) {
        ACTION(p,L"process.exec",false,L"global-tool.exe",brokers[i],L"global-tool");
        ACTION(other,L"process.exec",false,L"global-tool.exe",brokers[i],L"GLOBAL-TOOL.EXE");
        ACTION(p,L"process.exec",false,L"local-tool",brokers[i],L"local-tool.exe");
        ACTION(other,L"process.exec",true,NULL,brokers[i],L"local-tool.exe");
        ACTION(p,L"process.exec",false,L"Shared Ops",brokers[i],L"pwsh.exe",L"-File",L"C:/Shared Ops/\u96ea.ps1");
        REQUEST(p,true,NULL,brokers[i],L"--help");
    }
    ACTION(p,L"process.exec",false,L"global-tool.exe",L"sudo",L"--",L"global-tool.exe");
    ACTION(p,L"process.exec",false,L"global-tool.exe",L"sudo",L"run",L"global-tool.exe");
    ACTION(p,L"process.exec",false,L"global-tool.exe",L"gsudo",L"-n",L"-w",L"global-tool.exe");
    ACTION(p,L"process.exec",false,L"cannot safely",L"gsudo");
    ACTION(p,L"process.exec",false,L"cannot safely",L"gsudo",L"--unknown",L"other.exe");
    ACTION(p,L"process.exec",false,L"cannot safely",L"gsudo",L"cmd.exe",L"/c",L"opaque");
    ACTION(p,L"process.exec",false,L"global-tool.exe",L"runas.exe",L"/user:Example",L"GLOBAL-TOOL.EXE --option");
    ACTION(p,L"process.exec",false,L"Shared Ops",L"runas.exe",L"/user:Example",L"pwsh.exe -File \"C:/Shared Ops/\u96ea.ps1\"");
    ACTION(other,L"process.exec",true,NULL,L"runas.exe",L"/user:Example",L"local-tool.exe");
    ACTION(p,L"process.exec",false,L"cannot safely",L"runas.exe",L"/user:Example",L"%DYNAMIC%");
    ACTION(p,L"process.exec",false,L"global-tool.exe",L"sudo.exe",L"gsudo.exe",L"global-tool.exe");
    const wchar_t *nested[]={L"sudo",L"sudo",L"sudo",L"sudo",L"sudo",L"sudo",L"sudo",L"sudo",L"sudo",L"safe.exe"};
    request(p,nested,sizeof(nested)/sizeof(*nested),false,L"cannot safely");
}
static void test_powershell(void) {
    scoped(); const SpProfile *p=&config.profiles[0],*other=&config.profiles[1];
    ACTION(p,L"process.exec",false,L"global-tool.exe",L"pwsh.exe",L"-NoProfile",L"-Command",L"Start-Process",L"global-tool.exe",L"-Verb",L"RunAs");
    ACTION(p,L"process.exec",false,L"Shared Ops",L"powershell",L"-Command",L"Start-Process -FilePath 'C:/Shared Ops/\u96ea.ps1' -Verb RunAs");
    ACTION(p,L"process.exec",false,L"local-tool",L"powershell",L"-Command",L"Start-Process -Verb:RunAs -FilePath local-tool.exe -Wait");
    ACTION(other,L"process.exec",true,NULL,L"powershell",L"-Command",L"Start-Process local-tool.exe -Verb RunAs");
    ACTION(p,L"process.exec",false,L"cannot safely",L"pwsh",L"-c",L"Start-Process $target -Verb RunAs");
    ACTION(p,L"process.exec",true,NULL,L"pwsh",L"-c",L"Start-Process global-tool.exe");
    ACTION(p,L"process.exec",true,NULL,L"pwsh",L"-c",L"Write-Output 'Start-Process global-tool.exe -Verb RunAs'");
    ACTION(p,L"process.exec",true,NULL,L"editor.exe",L"Start-Process",L"global-tool.exe",L"-Verb",L"RunAs");
    ACTION(p,L"process.exec",true,NULL,L"pwsh",L"-File",L"C:/Work/locked.py");
}
static void test_terminals_and_all(void) {
    scoped(); const SpProfile *p=&config.profiles[0]; unsigned before=lookups;
    ACTION(p,L"terminal.quake",false,L"global-tool.exe",L"Dev",L"sudo.exe",L"global-tool.exe");
    CHECK(lookups==before);
    ACTION(p,L"terminal.quake",true,NULL,L"Dev",L"pwsh.exe",L"-File",L"C:/Shared Ops/\u96ea.ps1");
    REQUEST(p,false,L"global-tool.exe",L"wt.exe",L"-w",L"_quake",L"nt",L"-p",L"Dev",L"--",L"sudo",L"global-tool.exe");
    REQUEST(p,false,L"global-tool.exe",L"wt.exe",L"nt",L"--elevate",L"global-tool.exe");
    REQUEST(p,true,NULL,L"wt.exe",L"nt",L"-p",L"--elevate",L"--",L"global-tool.exe");
    load(L"never-elevate\nall\nend-never-elevate\nprofile default\nbind Win+S scratch.toggle\nend"); p=&config.profiles[0];
    ACTION(p,L"process.exec",true,NULL,L"safe.exe");
    ACTION(p,L"process.exec",true,NULL,L"pwsh.exe",L"-File",L"C:/Work/locked.ps1");
    action(p,L"terminal.open",NULL,0,true,NULL);
    action(p,L"terminal.container",NULL,0,true,NULL); CHECK(wcsstr(message,L"2 matching"));
    ACTION(p,L"terminal.quake",true,NULL,L"Dev",L"safe.exe");
    ACTION(p,L"process.exec",false,L"all",L"sudo.exe",L"safe.exe");
    ACTION(p,L"process.exec",false,L"all",L"pwsh",L"-Command",L"Start-Process safe.exe -Verb RunAs");
    token_high=true; before=lookups;
    ACTION(p,L"process.exec",false,L"all",L"safe.exe");
    action(p,L"terminal.open",NULL,0,false,L"all");
    action(p,L"terminal.container",NULL,0,false,L"all");
    CHECK(lookups==before); token_high=false;
    load(L"profile default\nnever-elevate\nall\nend-never-elevate\nbind Win+S scratch.toggle\nend\n"
         L"profile other\nbind Win+S scratch.toggle\nend");
    ACTION(&config.profiles[0],L"process.exec",false,L"all",L"sudo.exe",L"safe.exe");
    ACTION(&config.profiles[1],L"process.exec",true,NULL,L"sudo.exe",L"safe.exe");
    load(L"never-elevate\npodman.exe\nend-never-elevate\nprofile default\nbind Win+S scratch.toggle\nend");
    token_high=true; before=lookups; action(&config.profiles[0],L"terminal.container",NULL,0,false,L"podman.exe"); CHECK(lookups==before);
    load(L"never-elevate\nC:/Fixture/wt.exe\nend-never-elevate\nprofile default\nbind Win+S scratch.toggle\nend");
    action(&config.profiles[0],L"terminal.container",NULL,0,false,L"Fixture/wt.exe");
    token_high=false;
}
static void test_default_terminal_tokens(void) {
    sp_config_defaults(&config);
    const SpProfile *p=sp_config_profile(&config,L"default");
    CHECK(p && sp_never_elevate_all(p));
    static const struct {
        const char *name;
        bool elevated,high_integrity,open_denied,elevation_denied,integrity_denied;
    } observations[]={
        {"standard/medium",false,false,false,false,false},
        {"elevated",true,false,false,false,false},
        {"high-integrity only",false,true,false,false,false},
        {"token open denied",false,false,true,false,false},
        {"elevation query denied",false,false,false,true,false},
        {"integrity query denied",false,false,false,false,true}
    };
    const wchar_t *actions[]={L"terminal.open",L"terminal.container"};
    for (size_t i=0;i<sizeof(observations)/sizeof(*observations);++i) {
        token_high=observations[i].elevated;integrity_high=observations[i].high_integrity;
        fail_open=observations[i].open_denied;fail_elevation=observations[i].elevation_denied;fail_integrity=observations[i].integrity_denied;
        bool allow=i==0,unknown=fail_open||fail_elevation||fail_integrity;
        for (size_t j=0;j<sizeof(actions)/sizeof(*actions);++j) {
            SpBinding b={0};b.action=actions[j];
            unsigned before_spawn=spawns,before_capture=captures,before_lookup=lookups,before_token=token_queries;
            wcscpy(message,L"stale diagnostic");
            bool actual=observed_run(p,&b,NULL,message,1024);
            if (actual!=allow) fprintf(stderr,"default %ls with %s observation: %ls\n",b.action,observations[i].name,message);
            CHECK(actual==allow && token_queries>before_token);
            CHECK(spawns==before_spawn+(allow?1u:0u));
            CHECK(lookups==before_lookup+(allow?(j?2u:1u):0u));
            CHECK(captures==before_capture+(allow && j?1u:0u));
            if (allow) {
                if (j) CHECK(wcsstr(message,L"2 matching containers"));
                else CHECK(!*message);
            } else actionable_denial(p,unknown?L"cannot determine the launch token's elevation":L"rule (all)");
        }
    }
    token_high=integrity_high=fail_open=fail_elevation=fail_integrity=false;
}
static void test_fail_closed_and_messages(void) {
    scoped(); const SpProfile *p=&config.profiles[0];
    fail_open=true; ACTION(p,L"process.exec",false,L"token",L"global-tool.exe"); fail_open=false;
    fail_elevation=true; ACTION(p,L"process.exec",false,L"token",L"safe.exe"); fail_elevation=false;
    fail_integrity=true; ACTION(p,L"process.exec",false,L"token",L"safe.exe"); fail_integrity=false;
    const wchar_t *args[]={L"sudo",L"global-tool.exe"};
    CHECK(!observed_guard(p,args[0],args,2,NULL,0));
    wchar_t tiny[2]={L'!',L'!'}; CHECK(!observed_guard(p,args[0],args,2,tiny,1)); CHECK(!tiny[0] && tiny[1]==L'!');
    wchar_t small[41]; wmemset(small,L'!',41); CHECK(!observed_guard(p,args[0],args,2,small,40)); CHECK(!small[39] && small[40]==L'!');
    CHECK(!observed_guard(p,L"sudo",NULL,1,message,1024));
    CHECK(!observed_guard(NULL,args[0],args,2,message,1024));
    /* Tray-sized text must retain recovery before an unbounded rule/path. */
    wchar_t long_rule[1024]=L"C:/",tray[257];
    for (unsigned i=0;i<40;++i) wcscat(long_rule,L"long-segment/");
    wcscat(long_rule,L"global-tool.exe"); CHECK(wcslen(long_rule)>256);
    SpProfile long_profile={0}; long_profile.name=L"long-rule";
    long_profile.never_elevate.entries[0]=long_rule; long_profile.never_elevate.count=1;
    const wchar_t *long_args[]={L"sudo",long_rule};
    request(&long_profile,long_args,2,false,long_rule);
    CHECK(wcsstr(message,L"profile 'long-rule'"));
    wmemset(tray,L'!',257);
    CHECK(!observed_guard(&long_profile,long_args[0],long_args,2,tray,256));
    CHECK(!tray[255] && tray[256]==L'!');
    CHECK(wcsstr(tray,L"Elevation blocked") && wcsstr(tray,L"never-elevate"));
    CHECK(wcsstr(tray,L"Run normally from a non-elevated scratchpad"));
    CHECK(wcsstr(tray,L"remove the elevation request"));
    CHECK(wcsstr(tray,L"profile 'long-rule'"));
    load(L"profile default\nbind Win+S scratch.toggle\nend");
    unsigned before=token_queries; fail_open=true;
    ACTION(&config.profiles[0],L"process.exec",true,NULL,L"safe.exe"); CHECK(token_queries==before); fail_open=false;
    /* A 740 error is reported; no fallback to an elevation broker is attempted. */
    SpBinding b={0}; b.action=L"process.exec"; b.args[0]=L"requires-admin.exe"; b.argc=1;
    spawn_error=ERROR_ELEVATION_REQUIRED; before=spawns;
    CHECK(!observed_run(&config.profiles[0],&b,NULL,message,1024)); CHECK(spawns==before+1 && wcsstr(message,L"no elevation retry")); spawn_error=0;
}
/* Every review regression exercises both the guard and the production worker
   action, with all external process/discovery boundaries counted above. */
static void regression(const SpProfile *p,const wchar_t *const *args,size_t count,bool allow,const wchar_t *diagnostic) {
    unsigned before_spawn=spawns,before_capture=captures,before_lookup=lookups;
    request(p,args,count,allow,diagnostic);
    CHECK(spawns==before_spawn && captures==before_capture && lookups==before_lookup);
    action(p,L"process.exec",args,count,allow,diagnostic);
    CHECK(captures==before_capture);
    if (!allow) CHECK(lookups==before_lookup);
}
#define REGRESSION(p,allow,diagnostic,...) do { const wchar_t *a[]={__VA_ARGS__}; regression(p,a,sizeof(a)/sizeof(*a),allow,diagnostic); } while(0)
static void test_terminal_review_regressions(void) {
    scoped(); const SpProfile *p=&config.profiles[0];
    REGRESSION(p,false,L"cannot safely",L"wt.exe",L"nt",L"safe.exe",L";",L"nt",L"--elevate",L"global-tool.exe");
    REGRESSION(p,false,L"cannot safely",L"wt.exe",L"nt",L"--",L"safe.exe",L";",L"nt",L"sudo",L"global-tool.exe");
    REGRESSION(p,false,L"cannot safely",L"wt.exe",L"safe.exe",L"argument;nt",L"--elevate",L"global-tool.exe");
    REGRESSION(p,false,L"cannot safely",L"wt.exe",L"--unsupported",L"nt",L"--elevate",L"global-tool.exe");
    REGRESSION(p,false,L"cannot safely",L"wt.exe",L"nt",L"--unsupported",L"sudo",L"global-tool.exe");
    REGRESSION(p,false,L"cannot safely",L"wt.exe",L"--unsupported",L"safe.exe");
    const wchar_t *flags[]={L"-M",L"-F",L"-f"};
    for (size_t i=0;i<sizeof(flags)/sizeof(*flags);++i) {
        REGRESSION(p,false,L"global-tool.exe",L"wt.exe",flags[i],L"nt",L"--elevate",L"global-tool.exe");
        REGRESSION(p,true,NULL,L"wt.exe",flags[i],L"nt",L"safe.exe");
    }
    REGRESSION(p,false,L"global-tool.exe",L"wt.exe",L"--pos",L"10,20",L"--size",L"120,40",L"nt",L"--elevate",L"global-tool.exe");
    REGRESSION(p,false,L"global-tool.exe",L"wt.exe",L"--pos",L"10,20",L"nt",L"sudo",L"global-tool.exe");
    REGRESSION(p,false,L"cannot safely",L"wt.exe",L"--size");
    REGRESSION(p,true,NULL,L"wt.exe",L"--pos",L"10,20",L"--size",L"120,40",L"nt",L"safe.exe");
    /* Metadata operands and arguments after the child boundary are not flags. */
    REGRESSION(p,true,NULL,L"wt.exe",L"nt",L"-p",L"--elevate",L"--title",L"sudo",L"--",L"global-tool.exe");
    REGRESSION(p,true,NULL,L"wt.exe",L"nt",L"safe.exe",L"--elevate",L"sudo",L"global-tool.exe");
    REGRESSION(p,true,NULL,L"wt.exe",L"nt",L"--",L"safe.exe",L"--elevate",L"global-tool.exe");
    REGRESSION(p,true,NULL,L"wt.exe",L"--help");
}
static void test_powershell_review_regressions(void) {
    scoped(); const SpProfile *p=&config.profiles[0];
    const wchar_t *verbs[]={L"-Verb:'RunAs'",L"-Verb:\"RunAs\"",L"-vErB:'rUnAs'"};
    for (size_t i=0;i<sizeof(verbs)/sizeof(*verbs);++i) {
        REGRESSION(p,false,L"global-tool.exe",L"pwsh",L"-c",L"Start-Process",L"global-tool.exe",verbs[i]);
    }
    REGRESSION(p,false,L"global-tool.exe",L"pwsh",L"-c",L"Start-Process global-tool.exe -Verb:'RunAs'");
    REGRESSION(p,false,L"global-tool.exe",L"powershell",L"-Command",L"Start-Process global-tool.exe -Verb:\"RunAs\"");
    REGRESSION(p,false,L"global-tool.exe",L"pwsh",L"-c",L"Start-Process",L"global-tool.exe",L"-Verb",L"'RunAs'");
    REGRESSION(p,false,L"global-tool.exe",L"pwsh",L"-c",L"Start-Process",L"global-tool.exe",L"-Verb",L"\"RunAs\"");
    REGRESSION(p,false,L"cannot safely",L"pwsh",L"-c",L"Start-Process global-tool.exe -Verb:$verb");
    REGRESSION(p,false,L"cannot safely",L"pwsh",L"-c",L"Start-Process global-tool.exe -Verb:\"$verb\"");
    REGRESSION(p,false,L"cannot safely",L"pwsh",L"-c",L"Start-Process global-tool.exe -Verb:");
    REGRESSION(p,false,L"cannot safely",L"pwsh",L"-c",L"Start-Process global-tool.exe -Verb 'RunAs");
    REGRESSION(p,false,L"cannot safely",L"pwsh",L"-c",L"Start-Process global-tool.exe -Verb");
    REGRESSION(p,true,NULL,L"pwsh",L"-c",L"Start-Process global-tool.exe -Verb Open");
    REGRESSION(p,true,NULL,L"pwsh",L"-c",L"Start-Process global-tool.exe -Verb:'Open'");
    REGRESSION(p,true,NULL,L"pwsh",L"-c",L"Start-Process",L"global-tool.exe",L"-Verb:\"Open\"");
    REGRESSION(p,true,NULL,L"pwsh",L"-c",L"Write-Output \"Start-Process global-tool.exe -Verb:'RunAs'\"");
    REGRESSION(p,true,NULL,L"pwsh",L"-c",L"Write-Output 'Start-Process global-tool.exe -Verb:\"RunAs\"'");
}
/* Native PowerShell joins every operand after -Command with one space. Test
   every grouping of the same four literal script words, not just both extremes. */
static void powershell_groups(const SpProfile *p,const wchar_t *target,bool allow,const wchar_t *diagnostic) {
    const wchar_t *words[]={L"Start-Process",target,L"-Verb",L"RunAs"};
    for (unsigned mask=0;mask<8;++mask) {
        wchar_t groups[4][512]={{0}}; size_t group=0;
        const wchar_t *args[6]={L"pwsh.exe",L"-Command",groups[0]};
        for (size_t i=0;i<4;++i) {
            if (i && (mask&(1u<<(i-1)))) { ++group; args[group+2]=groups[group]; }
            else if (i) wcscat(groups[group],L" ");
            CHECK(wcslen(groups[group])+wcslen(words[i])<512);
            wcscat(groups[group],words[i]);
        }
        regression(p,args,group+3,allow,diagnostic);
        args[0]=L"powershell.exe"; args[1]=L"-c";
        regression(p,args,group+3,allow,diagnostic);
    }
}
static void test_powershell_grouping_regressions(void) {
    scoped(); const SpProfile *p=&config.profiles[0],*other=&config.profiles[1];
    powershell_groups(p,L"global-tool.exe",false,L"global-tool.exe");
    powershell_groups(other,L"global-tool.exe",false,L"global-tool.exe");
    powershell_groups(p,L"'C:/Shared Ops/\u96ea.ps1'",false,L"Shared Ops");
    powershell_groups(other,L"\"C:/Shared Ops/\u96ea.ps1\"",false,L"Shared Ops");
    powershell_groups(p,L"local-tool.exe",false,L"local-tool");
    powershell_groups(other,L"local-tool.exe",true,NULL);
    powershell_groups(p,L"'C:/Work/locked.py'",false,L"locked.py");
    powershell_groups(other,L"'C:/Work/locked.py'",true,NULL);
    REGRESSION(p,false,L"Shared Ops",L"pwsh",L"-c",L"Start-Process -FilePath 'C:/Shared",L"Ops/\u96ea.ps1' -Verb",L"RunAs");
    REGRESSION(p,false,L"global-tool.exe",L"pwsh",L"-c",L"Start-Process global-tool.exe",L"-Verb:'RunAs'");
    REGRESSION(p,true,NULL,L"pwsh",L"-c",L"Start-Process global-tool.exe",L"-Verb",L"Open");
    REGRESSION(p,true,NULL,L"pwsh",L"-c",L"Write-Output 'Start-Process",L"global-tool.exe -Verb",L"RunAs'");
    /* Bound the joined text, including the spaces inserted between native args. */
    wchar_t *bounded=calloc(SP_COMMAND_CAP+1,sizeof(*bounded)); CHECK(bounded);
    const wchar_t *script=L"Start-Process global-tool.exe -Verb RunAs";
    wmemset(bounded,L' ',SP_COMMAND_CAP); memcpy(bounded,script,wcslen(script)*sizeof(*bounded));
    bounded[SP_COMMAND_CAP-1]=0;
    REGRESSION(p,false,L"global-tool.exe",L"pwsh",L"-c",bounded);
    REGRESSION(p,false,L"cannot safely",L"pwsh",L"-c",bounded,L"");
    bounded[SP_COMMAND_CAP-1]=L' ';
    REGRESSION(p,false,L"cannot safely",L"pwsh",L"-c",bounded);
    free(bounded);
    load(L"never-elevate\nall\nend-never-elevate\nprofile default\nbind Win+S scratch.toggle\nend");
    p=&config.profiles[0];
    powershell_groups(p,L"global-tool.exe",false,L"all");
    powershell_groups(p,L"'C:/Shared Ops/\u96ea.ps1'",false,L"all");
    REGRESSION(p,true,NULL,L"pwsh",L"-c",L"Start-Process safe.exe",L"-Verb:'Open'");
    load(L"profile default\nnever-elevate\nlocal-tool\nend-never-elevate\nbind Win+S scratch.toggle\nend\n"
         L"profile other\nbind Win+S scratch.toggle\nend");
    powershell_groups(&config.profiles[0],L"local-tool.exe",false,L"local-tool");
    powershell_groups(&config.profiles[1],L"local-tool.exe",true,NULL);
}
static void terminal_reconstruction_cases(const SpProfile *p) {
    const wchar_t *unsafe[]={L"\"gsudo.exe\"",L"gsu\"do.exe",L"",L"tab\tvalue",L"line\rvalue",L"line\nvalue",L"delete\x7f",L"space end\\",L"space end\\\\"};
    for (size_t i=0;i<sizeof(unsafe)/sizeof(*unsafe);++i) {
        REGRESSION(p,false,L"cannot safely",L"wt.exe",L"nt",L"--",unsafe[i],L"global-tool.exe");
        REGRESSION(p,false,L"cannot safely",L"wt.exe",L"nt",L"--",L"safe.exe",unsafe[i]);
    }
    REGRESSION(p,false,L"cannot safely",L"wt.exe",L"nt",L"-p",L"",L"--",L"safe.exe");
    REGRESSION(p,false,L"cannot safely",L"wt.exe",L"nt",L"--title",L"line\nvalue",L"--",L"safe.exe");
    /* These restrictions belong only to Terminal's reconstruction boundary. */
    REGRESSION(p,true,NULL,L"safe.exe",L"\"gsudo.exe\"",L"",L"space end\\",L"line\nvalue",L"value;other");
    REGRESSION(p,true,NULL,L"wt.exe",L"nt",L"-p",L"My Dev",L"--",L"global-tool.exe",L"normal argument");
}
static void test_terminal_reconstruction_regressions(void) {
    scoped(); terminal_reconstruction_cases(&config.profiles[0]); terminal_reconstruction_cases(&config.profiles[1]);
    load(L"never-elevate\nall\nend-never-elevate\nprofile default\nbind Win+S scratch.toggle\nend");
    terminal_reconstruction_cases(&config.profiles[0]);
    load(L"profile default\nnever-elevate\nlocal-tool\nend-never-elevate\nbind Win+S scratch.toggle\nend");
    terminal_reconstruction_cases(&config.profiles[0]);
}
static void test_broker_executable_ambiguity_regressions(void) {
    scoped(); const SpProfile *p=&config.profiles[0],*other=&config.profiles[1];
    const wchar_t *brokers[]={L"sudo.exe",L"gsudo.exe"};
    for (size_t i=0;i<sizeof(brokers)/sizeof(*brokers);++i) {
        REGRESSION(p,false,L"cannot safely",brokers[i],L"\"global-tool.exe\"");
        REGRESSION(other,false,L"cannot safely",brokers[i],L"\"global-tool.exe\"");
        REGRESSION(p,false,L"cannot safely",brokers[i],L"\"local-tool.exe\"");
        REGRESSION(other,false,L"cannot safely",brokers[i],L"\"local-tool.exe\"");
        REGRESSION(p,true,NULL,brokers[i],L"safe.exe",L"ordinary argument");
    }
    REGRESSION(p,false,L"cannot safely",L"runas",L"/user:Example",L"C:/Program Files/global-tool.exe");
    REGRESSION(other,false,L"cannot safely",L"runas",L"/user:Example",L"C:/Program Files/global-tool.exe --option");
    REGRESSION(p,false,L"cannot safely",L"runas",L"/user:Example",L"C:/Program Files/local-tool.exe");
    REGRESSION(p,false,L"global-tool.exe",L"runas",L"/user:Example",L"\"C:/Program Files/global-tool.exe\" --option");
    REGRESSION(p,false,L"local-tool",L"runas",L"/user:Example",L"\"C:/Program Files/local-tool.exe\"");
    REGRESSION(other,true,NULL,L"runas",L"/user:Example",L"\"C:/Program Files/local-tool.exe\" --option");
    REGRESSION(p,true,NULL,L"runas",L"/user:Example",L"known.exe --option");
    REGRESSION(p,true,NULL,L"runas",L"/user:Example",L"\"C:/Program Files/known.exe\" --option");
    REGRESSION(p,true,NULL,L"safe.exe",L"C:/Program Files/global-tool.exe",L"\"global-tool.exe\"");
    load(L"never-elevate\nall\nend-never-elevate\nprofile default\nbind Win+S scratch.toggle\nend");
    p=&config.profiles[0];
    REGRESSION(p,false,L"all",L"runas",L"/user:Example",L"C:/Program Files/global-tool.exe");
    REGRESSION(p,false,L"all",L"gsudo",L"\"global-tool.exe\"");
    load(L"profile default\nnever-elevate\nlocal-tool\nend-never-elevate\nbind Win+S scratch.toggle\nend");
    p=&config.profiles[0];
    REGRESSION(p,false,L"cannot safely",L"runas",L"/user:Example",L"C:/Program Files/local-tool.exe");
    REGRESSION(p,false,L"local-tool",L"runas",L"/user:Example",L"\"C:/Program Files/local-tool.exe\" --option");
    REGRESSION(p,false,L"cannot safely",L"sudo",L"\"local-tool.exe\"");
    load(L"never-elevate\n\"C:/Program Files/global-tool.exe\"\nend-never-elevate\n"
         L"profile default\nnever-elevate\n\"C:/Program Files/local-tool.exe\"\nend-never-elevate\nbind Win+S scratch.toggle\nend\n"
         L"profile other\nbind Win+S scratch.toggle\nend");
    p=&config.profiles[0]; other=&config.profiles[1];
    REGRESSION(p,false,L"cannot safely",L"runas",L"/user:Example",L"C:/Program Files/global-tool.exe");
    REGRESSION(other,false,L"cannot safely",L"runas",L"/user:Example",L"C:/Program Files/global-tool.exe --option");
    REGRESSION(p,false,L"Program Files/global-tool.exe",L"runas",L"/user:Example",L"\"C:/Program Files/global-tool.exe\" --option");
    REGRESSION(other,false,L"Program Files/global-tool.exe",L"runas",L"/user:Example",L"\"C:/Program Files/global-tool.exe\"");
    REGRESSION(p,false,L"cannot safely",L"runas",L"/user:Example",L"C:/Program Files/local-tool.exe");
    REGRESSION(p,false,L"Program Files/local-tool.exe",L"runas",L"/user:Example",L"\"C:/Program Files/local-tool.exe\" --option");
    REGRESSION(other,true,NULL,L"runas",L"/user:Example",L"\"C:/Program Files/local-tool.exe\" --option");
}
static void test_broker_absolute_whitespace_regressions(void) {
    /* Program-only policies must not be rescued by unrelated path rules. */
    load(L"never-elevate\nglobal-tool.exe\nend-never-elevate\n"
         L"profile default\nnever-elevate\nlocal-tool\nend-never-elevate\nbind Win+S scratch.toggle\nend\n"
         L"profile other\nbind Win+S scratch.toggle\nend");
    const SpProfile *p=&config.profiles[0],*other=&config.profiles[1];
    const wchar_t *brokers[]={L"gsudo.exe",L"sudo.exe"};
    for (size_t i=0;i<sizeof(brokers)/sizeof(*brokers);++i) {
        REGRESSION(p,false,L"cannot safely",brokers[i],L"C:/Tools/global-tool.exe --option");
        REGRESSION(other,false,L"cannot safely",brokers[i],L"C:/Tools/global-tool.exe --option");
        REGRESSION(p,false,L"cannot safely",brokers[i],L"C:/Tools/global-tool.exe\t--option");
        REGRESSION(p,false,L"cannot safely",brokers[i],L"C:/Tools/local-tool.exe --option");
        /* An unquoted shell prefix may be protected even if the basename is not. */
        REGRESSION(p,false,L"cannot safely",brokers[i],L"C:/Tools/global-tool.exe data/safe.exe");
        REGRESSION(other,false,L"cannot safely",brokers[i],L"C:/Program Files/safe.exe");
        REGRESSION(p,false,L"cannot safely",brokers[i],L"C:/Tools/global-tool.exe --option",L"extra");
        REGRESSION(p,false,L"global-tool.exe",brokers[i],L"C:/Tools/global-tool.exe",L"--option");
        REGRESSION(p,true,NULL,brokers[i],L"C:/Tools/safe.exe",L"ordinary argument");
        REGRESSION(p,false,L"global-tool.exe",brokers[i],L"\"C:/Program Files/global-tool.exe\" --option");
        REGRESSION(p,true,NULL,brokers[i],L"\"C:/Program Files/safe.exe\" --option");
    }
    REGRESSION(p,false,L"cannot safely",L"runas",L"/user:Example",L"C:/Tools/global-tool.exe data/safe.exe");
    REGRESSION(p,false,L"global-tool.exe",L"runas",L"/user:Example",L"\"C:/Program Files/global-tool.exe\" --option");
    REGRESSION(p,true,NULL,L"runas",L"/user:Example",L"\"C:/Program Files/safe.exe\" --option");
    REGRESSION(p,true,NULL,L"C:/Tools/global-tool.exe --option");
    REGRESSION(p,true,NULL,L"C:/Tools/global-tool.exe data/safe.exe",L"ordinary argument");
}
static void test_relative_path_review_regressions(void) {
    wchar_t saved[SP_COMMAND_CAP],directory[SP_COMMAND_CAP],parent[SP_COMMAND_CAP],absolute[SP_COMMAND_CAP],text[SP_COMMAND_CAP+128];
    DWORD size=GetCurrentDirectoryW(SP_COMMAND_CAP,saved); CHECK(size && size<SP_COMMAND_CAP);
    size=GetModuleFileNameW(NULL,directory,SP_COMMAND_CAP); CHECK(size && size<SP_COMMAND_CAP);
    wchar_t *tail=wcsrchr(directory,L'\\'); CHECK(tail); *tail=0;
    wcscpy(parent,directory); tail=wcsrchr(parent,L'\\'); CHECK(tail); *tail=0;
    /* No script or directory is created: the rule is an immutable lexical path
       in an existing directory, independent of each subsequent inherited CWD. */
    for (wchar_t *s=directory;*s;++s) if (*s==L'\\') *s=L'/';
    CHECK(swprintf(absolute,SP_COMMAND_CAP,L"%ls/locked.py",directory)>0);
    swprintf(text,SP_COMMAND_CAP+128,L"never-elevate\n\"%ls/locked.py\"\nend-never-elevate\nprofile default\nbind Win+S scratch.toggle\nend",directory);
    load(text); const SpProfile *p=&config.profiles[0];
    CHECK(SetCurrentDirectoryW(directory)); token_high=true;
    const wchar_t *relative[]={L"locked.py",L".\\locked.py",L".\\LOCKED.PY. ",L"unused/../locked.py"};
    for (size_t i=0;i<sizeof(relative)/sizeof(*relative);++i) {
        REGRESSION(p,false,L"locked.py",L"python.exe",relative[i]);
    }
    REGRESSION(p,true,NULL,L"python.exe",L"unrelated.py");
    REGRESSION(p,true,NULL,L"python.exe",L"locked.py.exe");
    REGRESSION(p,true,NULL,L"python.exe",L"locked");
    REGRESSION(p,true,NULL,L"python.exe",L"locked.ps1");
    REGRESSION(p,true,NULL,L"wt",L"nt",L"-p",L"locked.py",L"--",L"safe.exe");
    /* A native bare executable still uses absolute PATH, not the CWD script. */
    REGRESSION(p,true,NULL,L"locked.py");
    token_high=false;
    REGRESSION(p,true,NULL,L"python.exe",L"locked.py");
    REGRESSION(p,true,NULL,L"python.exe",L".\\locked.py");
    REGRESSION(p,false,L"locked.py",L"sudo",L"locked.py");
    REGRESSION(p,false,L"locked.py",L"sudo",L".\\locked.py");
    REGRESSION(p,false,L"locked.py",L"runas",L"/user:Example",L"locked.py");
    REGRESSION(p,false,L"locked.py",L"sudo",L"python.exe",L"locked.py");
    REGRESSION(p,false,L"locked.py",L"gsudo",L"python.exe",L".\\locked.py");
    REGRESSION(p,false,L"locked.py",L"runas",L"/user:Example",L"python.exe .\\locked.py");
    REGRESSION(p,false,L"locked.py",L"pwsh",L"-c",L"Start-Process .\\locked.py -Verb:'RunAs'");
    unsigned before=lookups;
    ACTION(p,L"terminal.quake",false,L"cannot safely",L"Dev",L"sudo",L"python.exe",L"locked.py");
    CHECK(lookups==before);
    CHECK(SetCurrentDirectoryW(parent)); token_high=true;
    REGRESSION(p,true,NULL,L"python.exe",L"locked.py");
    REGRESSION(p,true,NULL,L"python.exe",L".\\locked.py");
    /* Terminal profiles may choose a different CWD. Never use the host's
       non-matching CWD to authorize a potentially protected relative child. */
    REGRESSION(p,false,L"cannot safely",L"wt",L"nt",L"--",L"python.exe",L"locked.py");
    before=lookups;
    ACTION(p,L"terminal.quake",false,L"cannot safely",L"Dev",L"python.exe",L"locked.py");
    CHECK(lookups==before);
    ACTION(p,L"terminal.quake",true,NULL,L"locked.py",L"safe.exe");
    token_high=false;
    REGRESSION(p,false,L"cannot safely",L"wt",L"nt",L"--elevate",L"python.exe",L"locked.py");
    REGRESSION(p,false,L"cannot safely",L"wt",L"nt",L"--elevate",L"python.exe",L".\\LOCKED.PY. ");
    REGRESSION(p,false,L"cannot safely",L"wt",L"nt",L"-p",L"Dev",L"--",L"sudo",L"python.exe",L"locked.py");
    before=lookups;
    ACTION(p,L"terminal.quake",false,L"cannot safely",L"Dev",L"sudo",L"python.exe",L"locked.py");
    CHECK(lookups==before);
    REGRESSION(p,false,L"locked.py",L"wt",L"nt",L"--elevate",L"python.exe",absolute);
    REGRESSION(p,true,NULL,L"wt",L"nt",L"--elevate",L"python.exe",L"C:/Other/locked.py");
    REGRESSION(p,true,NULL,L"wt",L"nt",L"--elevate",L"python.exe",L"unrelated.py");
    REGRESSION(p,true,NULL,L"wt",L"nt",L"--elevate",L"python.exe",L"locked.py.exe");
    REGRESSION(p,true,NULL,L"wt",L"nt",L"--elevate",L"python.exe",L"locked");
    REGRESSION(p,true,NULL,L"wt",L"nt",L"--elevate",L"python.exe",L"locked.ps1");
    REGRESSION(p,true,NULL,L"wt",L"nt",L"-p",L"--elevate",L"--",L"python.exe",L"locked.py");
    REGRESSION(p,false,L"cannot safely",L"pwsh",L"-c",L"Start-Process locked.py -WorkingDirectory elsewhere -Verb RunAs");
    CHECK(SetCurrentDirectoryW(directory)); token_high=true;
    REGRESSION(p,false,L"locked.py",L"python.exe",L"locked.py");
    token_high=false;
    CHECK(SetCurrentDirectoryW(saved));
    size=GetCurrentDirectoryW(SP_COMMAND_CAP,directory); CHECK(size && size<SP_COMMAND_CAP);
    CHECK(!wcscmp(saved,directory));
}
/* PowerShell recognizes quote classes, not identical opening/closing glyphs.
   Keep the complete classes in sync with the parse-only native oracle evidence. */
static void quote_class_cases(const SpProfile *p,const SpProfile *other,const wchar_t *quote_chars) {
    for (const wchar_t *open=quote_chars;*open;++open) for (const wchar_t *close=quote_chars;*close;++close) {
        wchar_t script[512],verb[32];
        swprintf(verb,32,L"%lcRunAs%lc",*open,*close);
        swprintf(script,512,L"Start-Process global-tool.exe -Verb %ls",verb);
        REGRESSION(p,false,L"global-tool.exe",L"pwsh.exe",L"-c",script);
        REGRESSION(other,false,L"global-tool.exe",L"powershell.exe",L"-Command",script);
        swprintf(script,512,L"Start-Process local-tool.exe -Verb:%ls",verb);
        REGRESSION(p,false,L"local-tool",L"pwsh.exe",L"-Command",script);
        REGRESSION(other,true,NULL,L"powershell.exe",L"-c",script);
        REGRESSION(p,false,L"global-tool.exe",L"pwsh.exe",L"-c",L"Start-Process",L"global-tool.exe",L"-Verb",verb);
        swprintf(script,512,L"Start-Process -FilePath %lcC:/Shared Ops/\u96ea.ps1%lc -Verb RunAs",*open,*close);
        REGRESSION(p,false,L"Shared Ops",L"pwsh.exe",L"-c",script);
        REGRESSION(other,false,L"Shared Ops",L"powershell.exe",L"-c",script);
        swprintf(script,512,L"Start-Process global-tool.exe -Verb:%lcOpen%lc",*open,*close);
        REGRESSION(p,true,NULL,L"pwsh.exe",L"-c",script);
        swprintf(script,512,L"Write-Output %lcStart-Process global-tool.exe -Verb RunAs%lc",*open,*close);
        REGRESSION(p,true,NULL,L"pwsh.exe",L"-c",script);
    }
}
static void test_quote_class_regressions(void) {
    scoped(); const SpProfile *p=&config.profiles[0],*other=&config.profiles[1];
    quote_class_cases(p,other,L"'\u2018\u2019\u201a\u201b");
    quote_class_cases(p,other,L"\"\u201c\u201d\u201e");
    /* A doubled quote is literal content, never a second round of delimiters.
       Retain it as an ambiguous child operand, not a different filename. */
    REGRESSION(p,false,L"cannot safely",L"pwsh.exe",L"-c",L"Start-Process -FilePath 'C:/Work/lock''ed.py' -Verb RunAs");
    REGRESSION(p,false,L"cannot safely",L"pwsh.exe",L"-c",L"Start-Process safe.exe -Verb '''RunAs'''");
    REGRESSION(p,false,L"cannot safely",L"pwsh.exe",L"-c",L"Start-Process safe.exe -Verb \u2018\u2019\u2018RunAs\u2019\u2018\u2019");
    REGRESSION(p,false,L"cannot safely",L"pwsh.exe",L"-c",L"Start-Process safe.exe -Verb \u2018RunAs");
    REGRESSION(p,false,L"cannot safely",L"pwsh.exe",L"-c",L"Start-Process safe.exe -Verb \u201c$verb\u201d");
    load(L"never-elevate\nall\nend-never-elevate\nprofile default\nbind Win+S scratch.toggle\nend");
    p=&config.profiles[0];
    const wchar_t *pairs[]={L"\u2018RunAs\u2019",L"\u201aRunAs'",L"'RunAs\u201b",L"\u201cRunAs\u201d",L"\u201eRunAs\""};
    for (size_t i=0;i<sizeof(pairs)/sizeof(*pairs);++i) {
        REGRESSION(p,false,L"all",L"pwsh.exe",L"-c",L"Start-Process",L"safe.exe",L"-Verb",pairs[i]);
    }
    REGRESSION(p,true,NULL,L"pwsh.exe",L"-c",L"Start-Process safe.exe -Verb \u2018Open\u2019");
    load(L"never-elevate\nC:/Work/lock\u2019ed.py\nend-never-elevate\nprofile default\nbind Win+S scratch.toggle\nend");
    REGRESSION(&config.profiles[0],false,L"lock\u2019ed.py",L"pwsh.exe",L"-c",L"Start-Process -FilePath \u2018C:/Work/lock\u2019\u2019ed.py\u2019 -Verb RunAs");
    /* A doubled mixed-class glyph retains its SECOND character in PowerShell. */
    load(L"never-elevate\nC:/Work/lock\u2018ed.py\nend-never-elevate\nprofile default\nbind Win+S scratch.toggle\nend");
    REGRESSION(&config.profiles[0],false,L"lock\u2018ed.py",L"pwsh.exe",L"-c",L"Start-Process -FilePath \u2018C:/Work/lock\u2019\u2018ed.py\u2019 -Verb RunAs");
    load(L"never-elevate\nC:/Work/lock\u201ced.py\nend-never-elevate\nprofile default\nbind Win+S scratch.toggle\nend");
    REGRESSION(&config.profiles[0],false,L"lock\u201ced.py",L"pwsh.exe",L"-c",L"Start-Process -FilePath \u201cC:/Work/lock\u201d\u201ced.py\u201d -Verb RunAs");
}
static void doubled_quote_path_cases(const wchar_t *quotes) {
    for (const wchar_t *escape=quotes;*escape;++escape) for (const wchar_t *literal=quotes;*literal;++literal) {
        if (*literal==L'\'' || *literal==L'"') continue; /* ASCII child quotes remain opaque. */
        wchar_t path[128],script[256];
        swprintf(path,128,L"C:/Work/lock%lced.py",*literal);
        swprintf(script,256,L"Start-Process -FilePath %lcC:/Work/lock%lc%lced.py%lc -Verb RunAs",quotes[0],*escape,*literal,quotes[0]);
        SpProfile p={0}; p.name=L"doubled-quote-path"; p.never_elevate.entries[0]=path; p.never_elevate.count=1;
        REGRESSION(&p,false,path,L"pwsh.exe",L"-c",script);
    }
}
static void literal_character_class_cases(const SpProfile *p,const wchar_t *rule) {
    const wchar_t *spaces=L" \t\v\f\x0085\u00a0\u1680\u2000\u2001\u2002\u2003\u2004\u2005\u2006\u2007\u2008\u2009\u200a\u2028\u2029\u202f\u205f\u3000";
    for (const wchar_t *space=spaces;*space;++space) {
        wchar_t script[256];
        swprintf(script,256,L"Start-Process%lcglobal-tool.exe%lc-VerB%lcRunAs",*space,*space,*space);
        REGRESSION(p,false,rule,L"pwsh.exe",L"-c",script);
        swprintf(script,256,L"Start-Process global-tool.exe -Verb%lcOpen",*space);
        REGRESSION(p,true,NULL,L"pwsh.exe",L"-c",script);
        swprintf(script,256,L"Write-Output%lc'Start-Process global-tool.exe -Verb RunAs'",*space);
        REGRESSION(p,true,NULL,L"powershell.exe",L"-Command",script);
    }
    const wchar_t *dashes=L"-\u2013\u2014\u2015";
    for (const wchar_t *dash=dashes;*dash;++dash) {
        wchar_t script[256];
        swprintf(script,256,L"Start-Process global-tool.exe %lcVerb:\u2018RunAs\u2019",*dash);
        REGRESSION(p,false,rule,L"pwsh.exe",L"-c",script);
        swprintf(script,256,L"Start-Process global-tool.exe %lcVerb:\u2018Open\u2019",*dash);
        REGRESSION(p,true,NULL,L"pwsh.exe",L"-c",script);
    }
}
static void test_literal_character_class_regressions(void) {
    doubled_quote_path_cases(L"'\u2018\u2019\u201a\u201b");
    doubled_quote_path_cases(L"\"\u201c\u201d\u201e");
    scoped(); literal_character_class_cases(&config.profiles[0],L"global-tool.exe");
    literal_character_class_cases(&config.profiles[1],L"global-tool.exe");
    load(L"never-elevate\nall\nend-never-elevate\nprofile default\nbind Win+S scratch.toggle\nend");
    literal_character_class_cases(&config.profiles[0],L"all");
    load(L"profile default\nnever-elevate\nglobal-tool.exe\nend-never-elevate\nbind Win+S scratch.toggle\nend\nprofile other\nbind Win+S scratch.toggle\nend");
    literal_character_class_cases(&config.profiles[0],L"global-tool.exe");
    REGRESSION(&config.profiles[1],true,NULL,L"pwsh.exe",L"-c",L"Start-Process\u00a0global-tool.exe \u2013Verb:\u2018RunAs\u2019");
}
static void native_punctuation_cases(const SpProfile *p) {
    const wchar_t *paths[]={L"C:/Tools/R&D/safe.exe",L"C:/Tools/O'Brien/safe.exe",L"C:/Tools/literal;directory/safe.exe",
        L"C:/Tools/$literal/safe.exe",L"C:/Tools/%literal%/safe.exe",L"C:/Tools/`literal/safe.exe"};
    for (size_t i=0;i<sizeof(paths)/sizeof(*paths);++i) {
        REGRESSION(p,true,NULL,paths[i],L"ordinary argument",L"'\"&;$literal");
        /* No lpApplicationName is available for the nested broker's target. */
        REGRESSION(p,false,L"cannot safely",L"gsudo.exe",paths[i]);
    }
    REGRESSION(p,false,L"global-tool.exe",L"C:/Tools/R&D/global-tool.exe");
}
static void test_native_punctuation_regressions(void) {
    scoped(); token_high=true;
    native_punctuation_cases(&config.profiles[0]); native_punctuation_cases(&config.profiles[1]);
    token_high=false;
    REGRESSION(&config.profiles[0],true,NULL,L"C:/Tools/R&D/global-tool.exe");
    REGRESSION(&config.profiles[0],true,NULL,L"C:/Tools/O'Brien/safe.exe");
    load(L"never-elevate\nall\nend-never-elevate\nprofile default\nbind Win+S scratch.toggle\nend");
    token_high=true;
    REGRESSION(&config.profiles[0],false,L"all",L"C:/Tools/R&D/safe.exe");
    token_high=false;
    REGRESSION(&config.profiles[0],true,NULL,L"C:/Tools/R&D/safe.exe");
}
static void long_ordinary_command_cases(const SpProfile *p) {
    const unsigned counts[]={SP_INSPECT_ARGS-2,SP_INSPECT_ARGS-1,SP_INSPECT_ARGS,SP_INSPECT_ARGS+1,SP_INSPECT_ARGS*4};
    for (size_t i=0;i<sizeof(counts)/sizeof(*counts);++i) {
        wchar_t command[4096]=L"Write-Output";
        for (unsigned j=0;j<counts[i];++j) wcscat(command,L" x");
        REGRESSION(p,true,NULL,L"pwsh.exe",L"-c",command);
        REGRESSION(p,true,NULL,L"powershell.exe",L"-Command",L"Write-Output",command+wcslen(L"Write-Output "));
        wcscat(command,L" 'Start-Process safe.exe -Verb RunAs'");
        REGRESSION(p,true,NULL,L"pwsh.exe",L"-c",command);
    }
    /* The budget still fails closed for the recognized launch form. */
    wchar_t launch[2048]=L"Start-Process safe.exe -Verb RunAs";
    for (unsigned j=0;j<SP_INSPECT_ARGS;++j) wcscat(launch,L" -Wait");
    REGRESSION(p,false,L"cannot safely",L"pwsh.exe",L"-c",launch);
}
static void test_ordinary_command_budget_regressions(void) {
    scoped(); long_ordinary_command_cases(&config.profiles[0]); long_ordinary_command_cases(&config.profiles[1]);
    load(L"never-elevate\nall\nend-never-elevate\nprofile default\nbind Win+S scratch.toggle\nend");
    long_ordinary_command_cases(&config.profiles[0]);
    load(L"profile default\nnever-elevate\nlocal-tool\nend-never-elevate\nbind Win+S scratch.toggle\nend");
    long_ordinary_command_cases(&config.profiles[0]);
}
static void quoted_parameter_data_cases(const SpProfile *p) {
    const wchar_t *pairs[]={L"''",L"\"\"",L"\u2018\u2019",L"\u201c\u201d",L"'\u201b",L"\u201e\""};
    const wchar_t *data[]={L"-Verb",L"-ArgumentList",L"-FilePath",L"-WorkingDirectory",L"-Verb:RunAs"};
    for (size_t i=0;i<sizeof(pairs)/sizeof(*pairs);++i) for (size_t j=0;j<sizeof(data)/sizeof(*data);++j) {
        wchar_t script[256];
        swprintf(script,256,L"Start-Process global-tool.exe %lc%ls%lc -Verb RunAs",pairs[i][0],data[j],pairs[i][1]);
        REGRESSION(p,false,L"cannot safely",L"pwsh.exe",L"-c",script);
        swprintf(script,256,L"Start-Process global-tool.exe %lc%ls%lc",pairs[i][0],data[j],pairs[i][1]);
        REGRESSION(p,true,NULL,L"pwsh.exe",L"-c",script);
    }
    REGRESSION(p,true,NULL,L"pwsh.exe",L"-c",L"Start-Process safe.exe -Ve'rb':RunAs");
    REGRESSION(p,false,L"cannot safely",L"pwsh.exe",L"-c",L"Start-Process global-tool.exe -Ve'rb':Open -Verb RunAs");
    REGRESSION(p,false,L"cannot safely",L"pwsh.exe",L"-c",L"Start-Process global-tool.exe -FilePath -Verb RunAs");
    REGRESSION(p,false,L"cannot safely",L"pwsh.exe",L"-c",L"Start-Process global-tool.exe -ArgumentList -Verb RunAs");
}
static void test_powershell_parameter_context_regressions(void) {
    scoped(); quoted_parameter_data_cases(&config.profiles[0]); quoted_parameter_data_cases(&config.profiles[1]);
    load(L"never-elevate\nall\nend-never-elevate\nprofile default\nbind Win+S scratch.toggle\nend");
    quoted_parameter_data_cases(&config.profiles[0]);
}
static void test_powershell_native_prefix_regressions(void) {
    scoped(); const SpProfile *p=&config.profiles[0];
    const wchar_t *commands[]={L"-c",L"-co",L"-com",L"-command",L"--Command",L"/Command",L"\u2013c",L" \t-Command\u00a0"};
    for (size_t i=0;i<sizeof(commands)/sizeof(*commands);++i) {
        REGRESSION(p,false,L"global-tool.exe",L"pwsh.exe",commands[i],L"Start-Process global-tool.exe -Verb RunAs");
        REGRESSION(p,false,L"global-tool.exe",L"pwsh.exe",L"-NoProfile",L"-NonInteractive",commands[i],L"Start-Process global-tool.exe -Verb RunAs");
        REGRESSION(p,true,NULL,L"pwsh.exe",commands[i],L"Start-Process global-tool.exe -Verb Open");
    }
    REGRESSION(p,false,L"global-tool.exe",L"pwsh.exe",L"",L" \t",L"-c",L"Start-Process global-tool.exe -Verb RunAs");
    const wchar_t *files[]={L"-f",L"-fi",L"-file",L"--File",L"/File",L"\u2013File",L" -File\u00a0"};
    for (size_t i=0;i<sizeof(files)/sizeof(*files);++i) {
        REGRESSION(p,true,NULL,L"pwsh.exe",files[i],L"C:/Scripts/trusted.ps1",L"-Command",L"Start-Process global-tool.exe -Verb RunAs");
        REGRESSION(p,true,NULL,L"powershell.exe",files[i],L"C:/Scripts/trusted.ps1",L"-c",L"Start-Process global-tool.exe -Verb RunAs");
    }
    REGRESSION(p,true,NULL,L"pwsh.exe",L"C:/Scripts/trusted.ps1",L"-Command",L"Start-Process global-tool.exe -Verb RunAs");
    const wchar_t *valued[]={L"-ExecutionPolicy",L"-InputFormat",L"-OutputFormat",L"-WindowStyle",L"-ConfigurationName",L"-SettingsFile"};
    for (size_t i=0;i<sizeof(valued)/sizeof(*valued);++i) {
        REGRESSION(p,true,NULL,L"pwsh.exe",valued[i],L"-Command",L"Start-Process global-tool.exe -Verb RunAs");
    }
    REGRESSION(p,false,L"global-tool.exe",L"powershell.exe",L"-Version",L"5.1",L"-c",L"Start-Process global-tool.exe -Verb RunAs");
    REGRESSION(p,true,NULL,L"pwsh.exe",L"-Version",L"5.1",L"-c",L"Start-Process global-tool.exe -Verb RunAs");
    REGRESSION(p,true,NULL,L"pwsh.exe",L"-Help",L"-c",L"Start-Process global-tool.exe -Verb RunAs");
    REGRESSION(p,true,NULL,L"powershell.exe",L"-?",L"-c",L"Start-Process global-tool.exe -Verb RunAs");
    load(L"never-elevate\nC:/Work/locked.py\nend-never-elevate\nprofile default\nbind Win+S scratch.toggle\nend\nprofile other\nbind Win+S scratch.toggle\nend");
    p=&config.profiles[0];
    wchar_t actual[SP_COMMAND_CAP]; CHECK(GetFullPathNameW(L"./locked.py",SP_COMMAND_CAP,actual,NULL));
    CHECK(!same(actual,L"C:\\Work\\locked.py"));
    REGRESSION(p,true,NULL,L"pwsh.exe",L"-c",L"Start-Process ./locked.py -Verb RunAs");
    const wchar_t *cwd[]={L"-WorkingDirectory",L"-wo",L"-wd",L"--wd",L"/wd",L"\u2013wd",L" -wd\u00a0"};
    for (size_t i=0;i<sizeof(cwd)/sizeof(*cwd);++i) {
        REGRESSION(p,false,L"cannot safely",L"pwsh.exe",cwd[i],L"C:/Work",L"-c",L"Start-Process ./locked.py -Verb RunAs");
        REGRESSION(&config.profiles[1],false,L"cannot safely",L"pwsh.exe",cwd[i],L"C:/Work",L"-c",L"Start-Process ./locked.py -Verb RunAs");
        REGRESSION(p,true,NULL,L"pwsh.exe",cwd[i],L"C:/Work",L"-c",L"Start-Process ./unrelated.py -Verb RunAs");
        REGRESSION(p,true,NULL,L"pwsh.exe",cwd[i],L"C:/Work",L"-c",L"Start-Process C:/Other/locked.py -Verb RunAs");
        REGRESSION(p,false,L"locked.py",L"pwsh.exe",cwd[i],L"C:/Work",L"-c",L"Start-Process C:/Work/locked.py -Verb RunAs");
        token_high=true;
        REGRESSION(p,false,L"cannot safely",L"pwsh.exe",cwd[i],L"C:/Work",L"-File",L"./locked.py");
        REGRESSION(p,true,NULL,L"pwsh.exe",cwd[i],L"C:/Work",L"-File",L"C:/Other/locked.py");
        token_high=false;
        REGRESSION(p,true,NULL,L"pwsh.exe",cwd[i],L"C:/Work",L"-File",L"./locked.py");
    }
    token_high=true;
    REGRESSION(p,true,NULL,L"pwsh.exe",L"-File",L"C:/Other/trusted.ps1",L"-wd",L"C:/Work",L"./locked.py");
    token_high=false;
    load(L"never-elevate\nall\nend-never-elevate\nprofile default\nbind Win+S scratch.toggle\nend");
    p=&config.profiles[0];
    REGRESSION(p,false,L"all",L"pwsh.exe",L"",L"-c",L"Start-Process safe.exe -Verb RunAs");
    REGRESSION(p,true,NULL,L"pwsh.exe",L"-File",L"C:/Scripts/trusted.ps1",L"-Command",L"Start-Process safe.exe -Verb RunAs");
    REGRESSION(p,true,NULL,L"pwsh.exe",L"C:/Scripts/trusted.ps1",L"-Command",L"Start-Process safe.exe -Verb RunAs");
    REGRESSION(p,true,NULL,L"pwsh.exe",L"-Help",L"-c",L"Start-Process safe.exe -Verb RunAs");
}
static void legacy_default_cases(const SpProfile *p,const wchar_t *target,bool allow,const wchar_t *diagnostic) {
    const wchar_t *names[]={L"Start-Process",L"saps",L"start"};
    for (size_t i=0;i<sizeof(names)/sizeof(*names);++i) {
        wchar_t script[512]; swprintf(script,512,L"%ls %ls -Verb \u2018RunAs\u2019",names[i],target);
        REGRESSION(p,allow,diagnostic,L"powershell.exe",L"-NoProfile",script);
        REGRESSION(p,allow,diagnostic,L"powershell.exe",names[i],target,L"-Verb",L"RunAs");
    }
}
static void test_powershell_legacy_default_regressions(void) {
    scoped(); const SpProfile *p=&config.profiles[0],*other=&config.profiles[1];
    legacy_default_cases(p,L"global-tool.exe",false,L"global-tool.exe");
    legacy_default_cases(other,L"global-tool.exe",false,L"global-tool.exe");
    legacy_default_cases(p,L"local-tool.exe",false,L"local-tool");
    legacy_default_cases(other,L"local-tool.exe",true,NULL);
    legacy_default_cases(p,L"'C:/Shared Ops/\u96ea.ps1'",false,L"Shared Ops");
    REGRESSION(p,true,NULL,L"powershell.exe",L"-NoProfile",L"Start-Process global-tool.exe -Verb Open");
    REGRESSION(p,true,NULL,L"powershell.exe",L"-NoProfile",L"Write-Output 'Start-Process global-tool.exe -Verb RunAs'");
    REGRESSION(p,true,NULL,L"powershell.exe",L"C:/Scripts/trusted.ps1",L"-Command",L"Start-Process global-tool.exe -Verb RunAs");
    REGRESSION(p,true,NULL,L"pwsh.exe",L"C:/Scripts/trusted.ps1",L"-Command",L"Start-Process global-tool.exe -Verb RunAs");
    load(L"never-elevate\nall\nend-never-elevate\nprofile default\nbind Win+S scratch.toggle\nend");
    p=&config.profiles[0];
    legacy_default_cases(p,L"safe.exe",false,L"all");
    REGRESSION(p,true,NULL,L"powershell.exe",L"-File",L"C:/Scripts/trusted.ps1",L"-Command",L"Start-Process safe.exe -Verb RunAs");
    REGRESSION(p,true,NULL,L"pwsh.exe",L"C:/Scripts/trusted.ps1",L"-Command",L"Start-Process safe.exe -Verb RunAs");
    wchar_t ordinary[2048]=L"Write-Output";
    for (unsigned i=0;i<SP_INSPECT_ARGS*2;++i) wcscat(ordinary,L" x");
    REGRESSION(p,true,NULL,L"powershell.exe",L"-NoProfile",ordinary);
}
/* Parse both policies AND bindings. Select by profile name and key chord just
   like the host; do not manufacture an elevation flag or a separate policy API. */
static void parsed_command(const wchar_t *global,const wchar_t *local,const wchar_t *command) {
    wchar_t text[4096];
    int n=swprintf(text,4096,L"never-elevate\n%lsend-never-elevate\n"
        L"profile default\nnever-elevate\n%lsend-never-elevate\nbind Win+S %ls\nend\n"
        L"profile other\nnever-elevate\nend-never-elevate\nbind Win+S %ls\nend\n",
        global,local,command,command);
    CHECK(n>0 && n<4096); load(text);
}
static const SpBinding *selected_binding(const wchar_t *profile,const wchar_t *key,const SpProfile **selected) {
    const SpProfile *p=sp_config_profile(&config,profile); CHECK(p);
    SpChord chord; CHECK(sp_chord(key,&chord));
    size_t index=sp_match_binding(p,chord.vk,chord.scan,chord.mods);
    CHECK(index<p->binding_count); *selected=p; return &p->bindings[index];
}
static const SpBinding *parsed_binding(const wchar_t *profile,const SpProfile **selected) {
    return selected_binding(profile,L"Win+S",selected);
}
static void selected_action(const wchar_t *profile,const wchar_t *key,bool allow,const wchar_t *diagnostic,unsigned expected_lookups) {
    const SpProfile *p; const SpBinding *b=selected_binding(profile,key,&p);
    unsigned before_spawn=spawns,before_capture=captures,before_lookup=lookups;
    wcscpy(message,L"stale diagnostic");
    bool actual=observed_run(p,b,NULL,message,1024);
    if (actual!=allow) fprintf(stderr,"parsed action unexpectedly %s: %ls / %ls / %ls\n",actual?"allowed":"blocked",p->name,b->action,b->argc?b->args[0]:L"(no args)");
    CHECK(actual==allow);
    CHECK(spawns==before_spawn+(allow?1u:0u)); CHECK(captures==before_capture);
    CHECK(lookups==before_lookup+expected_lookups);
    if (allow) { ++parsed_permitted; CHECK(!*message); }
    else { ++parsed_denials; parsed_raw_denials+=expected_lookups==0; actionable_denial(p,diagnostic); }
}
static void parsed_action(const wchar_t *profile,bool allow,const wchar_t *diagnostic,unsigned expected_lookups) {
    selected_action(profile,L"Win+S",allow,diagnostic,expected_lookups);
}
static void load_file(const wchar_t *path) {
    wchar_t error[512]; bool ok=sp_config_load(&config,path,error,512);
    if (!ok) fprintf(stderr,"cannot load test configuration %ls: %ls\n",path,error);
    CHECK(ok && !*error);
}
static size_t read_example(const char *path,char *text,size_t cap) {
    FILE *file=fopen(path,"rt");
    if (!file) fprintf(stderr,"cannot read documentation fixture: %s\n",path);
    CHECK(file);
    size_t size=fread(text,1,cap-1,file);
    CHECK(!ferror(file) && feof(file)); CHECK(fclose(file)==0);
    text[size]=0; return size;
}
static void test_documented_file_policies(void) {
    /* Real UTF-8 file loader, parser, profile/chord selection and production
       worker/guard. Only token observations and external process seams are
       simulated: none of the example's brokers or scripts may be executed. */
    static char reference[65537],example[65537];
    size_t size=read_example("examples/never-elevate.conf",example,sizeof(example));
    read_example("docs/CONFIGURATION.md",reference,sizeof(reference));
    const char *fence="```scratchpad\n",*block=reference; bool documented=false;
    while ((block=strstr(block,fence))) {
        block+=strlen(fence);
        if (!strncmp(block,example,size) && !strncmp(block+size,"```",3)) documented=true;
    }
    CHECK(documented); /* The test exercises the actual fenced example. */
    load_file(L"examples/never-elevate.conf");
    const SpProfile *p=sp_config_profile(&config,L"default"),*other=sp_config_profile(&config,L"other");
    CHECK(config.count==2 && config.never_elevate.count==3 && !config.never_elevate.all);
    CHECK(p && other && p->binding_count==9 && other->binding_count==9);
    CHECK(p->global_never_elevate==&config.never_elevate && other->global_never_elevate==&config.never_elevate);
    CHECK(p->never_elevate.count==2 && !p->never_elevate.all);
    CHECK(other->never_elevate.count==0 && !other->never_elevate.all);
    CHECK(sp_never_elevate_count(p)==5 && sp_never_elevate_count(other)==3);
    CHECK(!sp_never_elevate_all(p) && !sp_never_elevate_all(other));
    CHECK(!wcscmp(sp_never_elevate_entry(p,1),L"C:/Shared Ops/maintenance-\u96ea.ps1"));
    CHECK(!wcscmp(sp_never_elevate_entry(other,2),L"late-tool.exe"));
    unsigned denied=parsed_denials,raw=parsed_raw_denials,permitted=parsed_permitted;
    const struct { const wchar_t *key,*rule; bool local; } elevated[]={
        {L"Ctrl+Alt+G",L"global-tool.exe",false},
        {L"Ctrl+Alt+L",L"local-tool",true},
        {L"Ctrl+Alt+S",L"maintenance-\u96ea.ps1",false},
        {L"Ctrl+Alt+P",L"C:/Work/locked.py",true},
        {L"Ctrl+Alt+T",L"late-tool.exe",false}
    };
    for (size_t i=0;i<sizeof(elevated)/sizeof(*elevated);++i) {
        selected_action(L"default",elevated[i].key,false,elevated[i].rule,0);
        selected_action(L"other",elevated[i].key,elevated[i].local,elevated[i].local?NULL:elevated[i].rule,elevated[i].local?1u:0u);
    }
    const struct { const wchar_t *key,*rule; bool local; } ordinary[]={
        {L"Ctrl+Alt+N",L"global-tool.exe",false},
        {L"Ctrl+Alt+O",L"local-tool",true},
        {L"Ctrl+Alt+F",L"maintenance-\u96ea.ps1",false},
        {L"Ctrl+Alt+Y",L"C:/Work/locked.py",true}
    };
    for (size_t i=0;i<sizeof(ordinary)/sizeof(*ordinary);++i) {
        selected_action(L"default",ordinary[i].key,true,NULL,1);
        selected_action(L"other",ordinary[i].key,true,NULL,1);
        token_high=true; /* Inherited elevated token, not an actual elevation. */
        selected_action(L"default",ordinary[i].key,false,ordinary[i].rule,0);
        selected_action(L"other",ordinary[i].key,ordinary[i].local,ordinary[i].local?NULL:ordinary[i].rule,ordinary[i].local?1u:0u);
        token_high=false;
    }
    integrity_high=true;
    selected_action(L"default",L"Ctrl+Alt+N",false,L"global-tool.exe",0);
    selected_action(L"other",L"Ctrl+Alt+N",false,L"global-tool.exe",0);
    integrity_high=false;

    /* The shipped default is a real file too, not a merge of built-in policy.
       It applies all to both public key maps, with no container binding. */
    load_file(L"scratchpad.conf");
    p=sp_config_profile(&config,L"default"); other=sp_config_profile(&config,L"portable");
    CHECK(config.count==2 && config.never_elevate.all && !config.never_elevate.count);
    CHECK(p && other && p->binding_count==5 && other->binding_count==9);
    CHECK(!p->never_elevate.count && !p->never_elevate.all && !other->never_elevate.count && !other->never_elevate.all);
    CHECK(sp_never_elevate_all(p) && sp_never_elevate_all(other));
    selected_action(L"default",L"Ctrl+Alt+Scan029",true,NULL,1);
    selected_action(L"portable",L"Ctrl+Alt+N",true,NULL,1);
    token_high=true;
    selected_action(L"default",L"Ctrl+Alt+Scan029",false,L"all",0);
    selected_action(L"portable",L"Ctrl+Alt+N",false,L"all",0);
    token_high=false;
    CHECK(parsed_denials-denied==parsed_raw_denials-raw);
    printf("file-loaded policies: %u denials before lookup/capture/spawn; %u permitted fixture launches; fenced example matches\n",parsed_denials-denied,parsed_permitted-permitted);
}
static void test_parsed_scope_and_path_commands(void) {
    const wchar_t *global=L"global-tool.exe\n\"C:/Shared Ops/\u96ea.ps1\"\n\"//Server/Share/ops/locked.vbs\"\n";
    const wchar_t *local=L"local-tool\nC:/Work/locked.py\n";
    const struct { const wchar_t *command,*rule; bool high,local_only; } cases[]={
        {L"process.exec gsudo.exe GLOBAL-TOOL",L"global-tool.exe",false,false},
        {L"process.exec runas.exe /user:Example \"local-tool.exe --option\"",L"local-tool",false,true},
        {L"process.exec pwsh.exe -NoProfile -Command \"Start-Process -FilePath 'C:/Shared Ops/\u96ea.ps1' -Verb RunAs\"",L"Shared Ops",false,false},
        {L"process.exec wt.exe nt --elevate -- python.exe C:/Work/locked.py",L"locked.py",false,true},
        {L"terminal.quake Dev gsudo.exe global-tool.exe",L"global-tool.exe",false,false},
        {L"process.exec gsudo.exe wscript.exe //SERVER/share/ops/LOCKED.vbs",L"Server/Share",false,false},
        {L"process.exec GLOBAL-TOOL.EXE",L"global-tool.exe",true,false},
        {L"process.exec local-tool.exe",L"local-tool",true,true},
        {L"process.exec pwsh.exe -File \"c:\\\\SHARED OPS\\\\\u96ea.PS1\"",L"Shared Ops",true,false},
        {L"process.exec python.exe C:/Work/child/.././locked.py",L"locked.py",true,true},
        {L"process.exec wscript.exe //server/SHARE/ops/locked.VBS",L"Server/Share",true,false}
    };
    for (size_t i=0;i<sizeof(cases)/sizeof(*cases);++i) {
        parsed_command(global,local,cases[i].command); token_high=cases[i].high;
        parsed_action(L"default",false,cases[i].rule,0);
        parsed_action(L"other",cases[i].local_only,cases[i].local_only?NULL:cases[i].rule,cases[i].local_only?1u:0u);
        token_high=false;
    }
    /* Removing the elevation request must allow the very same restricted
       executable/script, not merely an unrelated unprotected command. */
    const wchar_t *ordinary[]={L"process.exec GLOBAL-TOOL.EXE",L"process.exec local-tool.exe",
        L"process.exec pwsh.exe -File \"C:/Shared Ops/\u96ea.ps1\"",
        L"process.exec python.exe C:/Work/locked.py",L"process.exec wscript.exe //Server/Share/ops/locked.vbs"};
    for (size_t i=0;i<sizeof(ordinary)/sizeof(*ordinary);++i) {
        parsed_command(global,local,ordinary[i]);
        parsed_action(L"default",true,NULL,1); parsed_action(L"other",true,NULL,1);
    }
    const wchar_t *nonmatches[]={L"process.exec python.exe C:/Other/locked.py",
        L"process.exec python.exe C:/Work/locked.py.exe",L"process.exec python.exe C:/Work/locked.py.bak",
        L"process.exec python.exe C:/Work/locked",L"process.exec python.exe C:/Work/locked.ps1",
        L"process.exec not-global-tool.exe",L"process.exec global-tool.exe.bak",
        L"process.exec editor.exe global-tool.exe -Verb RunAs"};
    token_high=true;
    for (size_t i=0;i<sizeof(nonmatches)/sizeof(*nonmatches);++i) {
        parsed_command(global,local,nonmatches[i]);
        parsed_action(L"default",true,NULL,1); parsed_action(L"other",true,NULL,1);
    }
    token_high=false;
    /* The raw name can pass while its resolved absolute executable is denied.
       Discovery is allowed here, but capture/spawn must still remain at zero. */
    parsed_command(L"C:/Fixture/global-tool.exe\n",L"",L"process.exec global-tool.exe"); token_high=true;
    parsed_action(L"default",false,L"Fixture/global-tool.exe",1); parsed_action(L"other",false,L"Fixture/global-tool.exe",1);
    parsed_command(L"",L"C:/Fixture/local-tool.exe\n",L"process.exec local-tool.exe");
    parsed_action(L"default",false,L"Fixture/local-tool.exe",1); parsed_action(L"other",true,NULL,1);
    parsed_command(L"C:/Fixture/wt.exe\n",L"",L"terminal.container");
    parsed_action(L"default",false,L"Fixture/wt.exe",1); parsed_action(L"other",false,L"Fixture/wt.exe",1);
    parsed_command(L"C:/Fixture/podman.exe\n",L"",L"terminal.container");
    parsed_action(L"default",false,L"Fixture/podman.exe",2); parsed_action(L"other",false,L"Fixture/podman.exe",2);
    token_high=false;
    /* Local all cannot affect another profile; an empty local block cannot
       erase global all. Ordinary standard-token commands remain permitted. */
    parsed_command(L"",L"all\n",L"process.exec gsudo.exe safe.exe");
    parsed_action(L"default",false,L"all",0); parsed_action(L"other",true,NULL,1);
    parsed_command(L"all\n",L"",L"process.exec gsudo.exe safe.exe");
    parsed_action(L"default",false,L"all",0); parsed_action(L"other",false,L"all",0);
    parsed_command(L"all\n",L"",L"process.exec safe.exe");
    parsed_action(L"default",true,NULL,1); parsed_action(L"other",true,NULL,1); token_high=true;
    parsed_action(L"default",false,L"all",0); parsed_action(L"other",false,L"all",0); token_high=false;
}
static void test_drive_relative_targets(void) {
    /* Program-only policies prevent an unrelated path rule from masking a
       basename bypass. No drive is queried or required to exist. */
    const wchar_t *drives=L"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
    for (const wchar_t *drive=drives;*drive;++drive) {
        wchar_t command[256],target[64];
        swprintf(target,64,L"%lc:GlObAl-ToOl.ExE",*drive);
        swprintf(command,256,L"process.exec gsudo.exe %ls",target);
        parsed_command(L"global-tool.exe\n",L"local-tool\n",command);
        parsed_action(L"default",false,L"global-tool.exe",0); parsed_action(L"other",false,L"global-tool.exe",0);
        REQUEST(&config.profiles[0],true,NULL,target); /* ordinary, no broker */
        swprintf(command,256,L"process.exec %ls",target);
        parsed_command(L"global-tool.exe\n",L"local-tool\n",command); token_high=true;
        parsed_action(L"default",false,L"global-tool.exe",0); parsed_action(L"other",false,L"global-tool.exe",0); token_high=false;
        swprintf(target,64,L"%lc:LOCAL-TOOL.exe",*drive);
        swprintf(command,256,L"process.exec gsudo.exe %ls",target);
        parsed_command(L"global-tool.exe\n",L"local-tool\n",command);
        parsed_action(L"default",false,L"local-tool",0); parsed_action(L"other",true,NULL,1);
        token_high=true; REQUEST(&config.profiles[0],false,L"local-tool",target); REQUEST(&config.profiles[1],true,NULL,target); token_high=false;
        REQUEST(&config.profiles[0],true,NULL,target);
    }
    const wchar_t *variants[]={L"global-tool",L"C:/Tools/GLOBAL-TOOL.exe",L"C:\\Tools\\Global-Tool.Exe.",
        L"E:folder/global-tool.exe",L"\\Tools\\global-tool.exe",L"\\\\Server\\Share\\global-tool.exe"};
    scoped();
    for (size_t i=0;i<sizeof(variants)/sizeof(*variants);++i) {
        REGRESSION(&config.profiles[0],false,L"global-tool.exe",L"gsudo.exe",variants[i]);
        REQUEST(&config.profiles[1],false,L"global-tool.exe",L"runas.exe",L"/user:Example",variants[i]);
        REQUEST(&config.profiles[0],true,NULL,variants[i]);
    }
    REGRESSION(&config.profiles[0],true,NULL,L"gsudo.exe",L"C:global-tool.exe.bak");
    REGRESSION(&config.profiles[0],true,NULL,L"gsudo.exe",L"C:not-global-tool.exe");
}
static void test_drive_relative_brokers(void) {
    const wchar_t *drives=L"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
    const wchar_t *forms[]={L"gsudo.exe global-tool.exe",L"sudo.exe run global-tool.exe",
        L"runas.exe /user:Example global-tool.exe",L"pwsh.exe -c \"Start-Process global-tool.exe -Verb RunAs\"",
        L"powershell.exe \"Start-Process global-tool.exe -Verb RunAs\"",L"wt.exe nt --elevate -- global-tool.exe",
        L"wtd.exe nt --elevate -- global-tool.exe",L"WindowsTerminal.exe nt --elevate -- global-tool.exe"};
    for (const wchar_t *drive=drives;*drive;++drive) for (size_t i=0;i<sizeof(forms)/sizeof(*forms);++i) {
        wchar_t command[256]; CHECK(swprintf(command,256,L"process.exec %lc:%ls",*drive,forms[i])>0);
        parsed_command(L"global-tool.exe\n",L"local-tool\n",command);
        parsed_action(L"default",false,L"global-tool.exe",0); parsed_action(L"other",false,L"global-tool.exe",0);
    }
    scoped(); const SpProfile *p=&config.profiles[0],*other=&config.profiles[1];
    REGRESSION(p,false,L"global-tool.exe",L"gsudo.exe",L"D:gsudo.exe",L"C:global-tool.exe");
    REGRESSION(p,false,L"Shared Ops",L"C:gsudo.exe",L"pwsh.exe",L"-File",L"C:/Shared Ops/\u96ea.ps1");
    REGRESSION(p,false,L"cannot safely",L"C:wt.exe",L"nt",L"--elevate",L"--",L"python.exe",L"C:locked.py");
    REGRESSION(other,true,NULL,L"C:wt.exe",L"nt",L"--elevate",L"--",L"python.exe",L"C:locked.py");
    REQUEST(p,true,NULL,L"C:pwsh.exe",L"-File",L"C:/Shared Ops/\u96ea.ps1");
    REQUEST(p,true,NULL,L"C:gsudo.exe",L"--help");
}
static void test_basename_boundary_controls(void) {
    const struct { const wchar_t *input,*base; } names[]={
        {L"",L""},{L"Q",L"Q"},{L":",L":"},{L"C:",L""},{L"c:Q",L"Q"},
        {L"C:tool.exe",L"tool.exe"},{L"C:/dir/tool.exe",L"tool.exe"},{L"C:\\dir\\tool.exe",L"tool.exe"},
        {L"\\\\server\\share\\tool.exe",L"tool.exe"},{L"//server/share/tool.exe",L"tool.exe"},
        {L"1:tool.exe",L"1:tool.exe"},{L"prefix:tool.exe",L"prefix:tool.exe"}};
    for (size_t i=0;i<sizeof(names)/sizeof(*names);++i) CHECK(!wcscmp(basename_of(names[i].input),names[i].base));
    CHECK(!program(L"",L"")); CHECK(!program(L"Q",L"")); CHECK(program(L"Q",L"c:q.exe")); CHECK(!program(L"Q",L"C:"));
    CHECK(program(L"tool.exe",L"C:TOOL.exe. ")); CHECK(!program(L"tool.exe",L"1:tool.exe")); CHECK(!program(L"tool.exe",L"prefix:tool.exe"));
    CHECK(possible_relative_name(L"C:/Work/locked.py",L"D:LOCKED.PY"));
    CHECK(!possible_relative_name(L"C:/Work/locked.py",L"D:locked.py.exe"));
    CHECK(!possible_relative_name(L"C:/Work/locked.py",L"D:locked"));
    CHECK(!possible_relative_name(L"C:/Work/locked.py",L"D:locked.ps1"));
    /* NUL/one-letter inputs end immediately before an inaccessible page. This
       catches out-of-bounds s[1] reads which ordinary literals can hide. */
    parsed_command(L"Q\n",L"",L"process.exec Q");
    SYSTEM_INFO info; GetSystemInfo(&info);
    BYTE *pages=VirtualAlloc(NULL,(SIZE_T)info.dwPageSize*2,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE); CHECK(pages);
    DWORD old=0; CHECK(VirtualProtect(pages+info.dwPageSize,info.dwPageSize,PAGE_NOACCESS,&old));
    wchar_t *edge=(wchar_t *)(pages+info.dwPageSize)-1; *edge=0;
    CHECK(basename_of(edge)==edge); REQUEST(&config.profiles[0],false,L"cannot safely",edge);
    --edge; *edge=L'Q'; CHECK(program(L"Q",edge));
    REQUEST(&config.profiles[0],true,NULL,edge); token_high=true;
    REQUEST(&config.profiles[0],false,L"Q",edge); token_high=false;
    --edge; edge[0]=L'C'; edge[1]=L':'; CHECK(!*basename_of(edge)); CHECK(!program(L"Q",edge));
    CHECK(VirtualFree(pages,0,MEM_RELEASE));
}
static int native_probe(void) {
    /* These are direct Win32 queries in the child, not the observed guard or
       the parent's observations. Refuse success for any unknown/high token. */
    HANDLE token=NULL; TOKEN_ELEVATION value={0}; DWORD size=0; int result=10;
    TOKEN_MANDATORY_LABEL *label=NULL;
    if (!OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY,&token)) return result;
    if (!GetTokenInformation(token,TokenElevation,&value,sizeof(value),&size)) goto done;
    GetTokenInformation(token,TokenIntegrityLevel,NULL,0,&size);
    if (!size || !(label=malloc(size))) goto done;
    if (!GetTokenInformation(token,TokenIntegrityLevel,label,size,&size) || !IsValidSid(label->Label.Sid)) goto done;
    BYTE count=*GetSidSubAuthorityCount(label->Label.Sid);
    if (!count) goto done;
    DWORD integrity=*GetSidSubAuthority(label->Label.Sid,count-1);
    printf("native child token: elevation=%lu integrity=%lx\n",(unsigned long)value.TokenIsElevated,(unsigned long)integrity);
    result=value.TokenIsElevated || integrity>=SECURITY_MANDATORY_HIGH_RID?11:0;
 done:
    free(label); CloseHandle(token); return result;
}
static void test_native_parsed_children(const wchar_t *self,bool high) {
    wchar_t path[SP_COMMAND_CAP],rule[SP_COMMAND_CAP+4],command[SP_COMMAND_CAP+32];
    wcscpy(path,self); for (wchar_t *s=path;*s;++s) if (*s==L'\\') *s=L'/';
    const wchar_t *base=wcsrchr(path,L'/'); CHECK(base && base[1]); ++base;
    CHECK(swprintf(command,SP_COMMAND_CAP+32,L"process.exec \"%ls\" --probe",path)>0);
    const char *labels[]={"global program name","profile program name","global full path","profile full path"};
    for (unsigned i=0;i<4;++i) {
        CHECK(swprintf(rule,SP_COMMAND_CAP+4,L"\"%ls\"\n",i<2?base:path)>0);
        parsed_command(i%2?L"":rule,i%2?rule:L"",command);
        const SpProfile *p; const SpBinding *b=parsed_binding(L"default",&p);
        CHECK(!wcscmp(b->action,L"process.exec") && b->argc==2);
        CHECK(!wcscmp(b->args[0],path) && !wcscmp(b->args[1],L"--probe"));
        unsigned before_spawn=spawns,before_capture=captures,before_lookup=lookups,before_token=token_queries;
        /* This is the real action, real resolver, real guard and real spawn.
           The only possible child is this executable's token-query probe. */
        CHECK(native_run(p,b,NULL,message,1024)==!high);
        if (high) actionable_denial(p,i<2?base:path);
        else {
            CHECK(!*message);
            /* Production spawn is asynchronous. Independently capture the
               exact same parsed argv and verify completion + its real token. */
            CHECK(sp_elevation_guard(p,b->args[0],b->args,b->argc,message,1024));
            SpProcessResult result; CHECK(sp_capture(b->args[0],b->args,b->argc,5000,NULL,&result));
            unsigned long child_elevation=1,child_integrity=SECURITY_MANDATORY_SYSTEM_RID;
            CHECK(result.exit_code==0 && !result.timed_out && !result.cancelled && !result.truncated);
            CHECK(sscanf(result.out,"native child token: elevation=%lu integrity=%lx",&child_elevation,&child_integrity)==2);
            CHECK(child_elevation==0 && child_integrity<SECURITY_MANDATORY_HIGH_RID);
            printf("native %s: %s",labels[i],result.out);
        }
        CHECK(spawns==before_spawn && captures==before_capture && lookups==before_lookup && token_queries==before_token);
    }
    printf("native parsed policies: %s (4 name/path scopes; no elevation requested)\n",high?"elevated runner refused":"standard execution passed");
}
static void test_native_shipped_policy(const wchar_t *self,bool high) {
    load_file(L"scratchpad.conf");
    const wchar_t *names[]={L"default",L"portable"};
    /* Only this fixed self-probe crosses the real process boundary. Example
       elevation bindings above always use the inert observed_run instead. */
    SpBinding probe={0}; probe.action=L"process.exec"; probe.args[0]=self; probe.args[1]=L"--probe"; probe.argc=2;
    for (size_t i=0;i<sizeof(names)/sizeof(*names);++i) {
        const SpProfile *p=sp_config_profile(&config,names[i]); CHECK(p && sp_never_elevate_all(p));
        CHECK(native_run(p,&probe,NULL,message,1024)==!high);
        if (high) actionable_denial(p,L"all");
        else {
            CHECK(!*message && sp_elevation_guard(p,self,probe.args,probe.argc,message,1024));
            /* The action's spawn is asynchronous. This second exact-argv
               capture independently acknowledges completion and token state. */
            SpProcessResult result; CHECK(sp_capture(self,probe.args,probe.argc,5000,NULL,&result));
            unsigned long elevation=1,integrity=SECURITY_MANDATORY_SYSTEM_RID;
            CHECK(result.exit_code==0 && !result.timed_out && !result.cancelled && !result.truncated);
            CHECK(sscanf(result.out,"native child token: elevation=%lu integrity=%lx",&elevation,&integrity)==2);
            CHECK(elevation==0 && integrity<SECURITY_MANDATORY_HIGH_RID);
            printf("native shipped %ls policy: %s",names[i],result.out);
        }
    }
    printf("native file-loaded all: %s (2 profiles; fixed self-probe only)\n",high?"elevated runner refused":"standard execution passed");
}
static void test_native_standard_child(void) {
    /* Exercise the real token query + real policy + a disposable native child.
       If this test runner is elevated, refusal is the expected safe outcome. */
    sp_config_defaults(&config); const SpProfile *p=&config.profiles[0];
    wchar_t self[SP_COMMAND_CAP]; CHECK(GetModuleFileNameW(NULL,self,SP_COMMAND_CAP));
    HANDLE token=NULL; TOKEN_ELEVATION value={0}; DWORD size=0; bool high;
    CHECK(OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY,&token));
    CHECK(GetTokenInformation(token,TokenElevation,&value,sizeof(value),&size));
    CHECK(!GetTokenInformation(token,TokenIntegrityLevel,NULL,0,&size) && size>0);
    TOKEN_MANDATORY_LABEL *label=malloc(size); CHECK(label);
    CHECK(GetTokenInformation(token,TokenIntegrityLevel,label,size,&size));
    high=value.TokenIsElevated || *GetSidSubAuthority(label->Label.Sid,*GetSidSubAuthorityCount(label->Label.Sid)-1)>=SECURITY_MANDATORY_HIGH_RID;
    free(label); CloseHandle(token);
    const wchar_t *args[]={self,L"--probe"};
    CHECK(sp_elevation_guard(p,self,args,2,message,1024)==!high);
    if (!high) {
        SpProcessResult result; CHECK(sp_capture(self,args,2,5000,NULL,&result));
        unsigned long child_elevation=1,child_integrity=SECURITY_MANDATORY_SYSTEM_RID;
        CHECK(result.exit_code==0);
        CHECK(sscanf(result.out,"native child token: elevation=%lu integrity=%lx",&child_elevation,&child_integrity)==2);
        CHECK(child_elevation==0 && child_integrity<SECURITY_MANDATORY_HIGH_RID);
        printf("%s",result.out);
    } else CHECK(wcsstr(message,L"all"));
    printf("native policy/token: %s (no elevation requested)\n",high?"elevated runner refused":"standard child passed");
    test_native_parsed_children(self,high);
    test_native_shipped_policy(self,high);
}
int main(int argc,char **argv) {
    if (argc==2 && !strcmp(argv[1],"--probe")) return native_probe();
    test_scopes_and_names(); test_full_paths(); test_explicit_brokers(); test_powershell();
    test_terminals_and_all(); test_broker_absolute_whitespace_regressions(); test_fail_closed_and_messages();
    test_terminal_review_regressions(); test_powershell_review_regressions();
    test_powershell_grouping_regressions(); test_terminal_reconstruction_regressions();
    test_broker_executable_ambiguity_regressions();
    test_relative_path_review_regressions();
    test_quote_class_regressions(); test_literal_character_class_regressions();
    test_native_punctuation_regressions();
    test_ordinary_command_budget_regressions();
    test_powershell_parameter_context_regressions(); test_powershell_native_prefix_regressions();
    test_powershell_legacy_default_regressions();
    test_documented_file_policies();
    test_parsed_scope_and_path_commands(); test_drive_relative_targets(); test_drive_relative_brokers();
    test_basename_boundary_controls();
    test_default_terminal_tokens();
    test_native_standard_child();
    printf("parsed command policies: %u denials before spawn/capture (%u before lookup, %u after resolution); %u permitted fixture launches\n",parsed_denials,parsed_raw_denials,parsed_denials-parsed_raw_denials,parsed_permitted);
    printf("elevation enforcement: %u checks passed (observed tokens + real argv preflight; no elevation/Terminal/container attempts)\n",checks);
    return 0;
}
