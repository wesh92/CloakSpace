/* Discovery is injected; real imports use only test-owned files under build/.
   Never read the operator's config, install hooks or contact network shares. */
#include "config_file.h"
#include "win32.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

static unsigned checks,lookups,opens;
static DWORD appdata_error,adjacent_error,environment_error;
static DWORD appdata_attributes,adjacent_attributes;
static bool environment_present=true;
static SpConfig cfg;
#define CHECK(x) do { ++checks; if (!(x)) { \
    fprintf(stderr,"%s:%d: %s\n",__FILE__,__LINE__,#x); exit(1); } } while(0)

static DWORD WINAPI fixture_environment(LPCWSTR name,LPWSTR buffer,DWORD cap) {
    CHECK(!wcscmp(name,L"APPDATA"));
    if (!environment_present) { SetLastError(environment_error); return 0; }
    static const wchar_t value[]=L"C:\\Fixture\\AppData";
    CHECK(cap>sizeof(value)/sizeof(*value));
    wcscpy(buffer,value); return (DWORD)wcslen(value);
}
static DWORD WINAPI fixture_module(HMODULE module,LPWSTR buffer,DWORD cap) {
    (void)module;
    static const wchar_t value[]=L"C:\\Fixture\\Bin\\cloakspace.exe";
    CHECK(cap>sizeof(value)/sizeof(*value));
    wcscpy(buffer,value); return (DWORD)wcslen(value);
}
static DWORD WINAPI fixture_attributes(LPCWSTR path) {
    ++lookups;
    bool appdata=!wcscmp(path,L"C:\\Fixture\\AppData\\cloakspace\\scratchpad.conf");
    CHECK(appdata || !wcscmp(path,L"C:\\Fixture\\Bin\\scratchpad.conf"));
    SetLastError(appdata?appdata_error:adjacent_error);
    return appdata?appdata_attributes:adjacent_attributes;
}
static HANDLE WINAPI fixture_open(LPCWSTR path,DWORD access,DWORD sharing,
        LPSECURITY_ATTRIBUTES security,DWORD disposition,DWORD flags,HANDLE template_file) {
    (void)path;(void)access;(void)sharing;(void)security;(void)disposition;(void)flags;(void)template_file;
    ++opens; SetLastError(ERROR_ACCESS_DENIED); return INVALID_HANDLE_VALUE;
}
#define GetEnvironmentVariableW fixture_environment
#define GetModuleFileNameW fixture_module
#define GetFileAttributesW fixture_attributes
#define CreateFileW fixture_open
#define sp_config_load fixture_config_load
#include "../src/config_file.c"
#undef GetEnvironmentVariableW
#undef GetModuleFileNameW
#undef GetFileAttributesW
#undef CreateFileW
#undef sp_config_load

static void reset(void) {
    ZeroMemory(&cfg,sizeof(cfg));lookups=opens=0;
    environment_present=true;environment_error=ERROR_ENVVAR_NOT_FOUND;
    appdata_attributes=adjacent_attributes=INVALID_FILE_ATTRIBUTES;
    appdata_error=adjacent_error=ERROR_FILE_NOT_FOUND;
}
static void refused(DWORD error,bool appdata) {
    reset();
    if (appdata) appdata_error=error; else adjacent_error=error;
    wchar_t diagnostic[256]={0},code[32];
    CHECK(!fixture_config_load(&cfg,NULL,diagnostic,256));
    CHECK(lookups==(appdata?1u:2u) && opens==0 && cfg.count==0);
    swprintf(code,32,L"%lu",(unsigned long)error);
    CHECK(wcsstr(diagnostic,code));
    CHECK(wcsstr(diagnostic,appdata?L"APPDATA":L"executable-adjacent"));
}
static void test_path_resolution(void) {
    static const struct {const wchar_t *from,*path,*expected;} valid[]={
        {L"C:\\config\\main.conf",L"nested/../globals.conf",L"C:\\config\\globals.conf"},
        {L"C:\\config\\main.conf",L"D:/other/global.conf",L"D:\\other\\global.conf"},
        {L"C:\\config\\main.conf",L"/root.conf",L"C:\\root.conf"},
        {L"C:\\config\\main.conf",L"//server/share/global.conf",L"\\\\server\\share\\global.conf"},
        {L"\\\\server\\share\\config\\main.conf",L"../global.conf",L"\\\\server\\share\\global.conf"},
        {L"\\\\server\\share\\config\\main.conf",L"/global.conf",L"\\\\server\\share\\global.conf"},
        {L"C:\\config\\main.conf",L"%APPDATA%/literal.conf",L"C:\\config\\%APPDATA%\\literal.conf"},
    };
    wchar_t error[512];
    for (size_t i=0;i<sizeof(valid)/sizeof(*valid);++i) {
        wchar_t *path=resolve_config_path(valid[i].from,valid[i].path,error,512);
        CHECK(path && !wcscmp(path,valid[i].expected));free(path);
    }
    static const wchar_t *const invalid[]={L"\\\\?\\C:\\bad.conf",L"\\\\.\\NUL",L"//?/C:/bad",L"//./NUL",
        L"C:bad.conf",L"C:",L"*.conf",L"sub/?.conf",L"file.conf:stream",L"C:/file.conf:stream",
        L"\\\\server\\pipe\\test",L"//server/PIPE/test",L"//server",L"//server//test",L""};
    for (size_t i=0;i<sizeof(invalid)/sizeof(*invalid);++i) {
        reset();SpConfigSource source={0};
        CHECK(!filesystem_loader(NULL,L"C:\\config\\main.conf",invalid[i],&source,error,512));
        CHECK(!source.text && !source.name && !opens && !lookups && *error);
    }
    /* An accepted UNC path reaches only the injected opener, never the network. */
    reset();SpConfigSource source={0};
    CHECK(!filesystem_loader(NULL,L"C:\\config\\main.conf",L"//server/share/global.conf",&source,error,512));
    CHECK(opens==1 && !source.text && !source.name);
}

#define FIXTURE_PATH_CAP 2048u
static wchar_t real_root[FIXTURE_PATH_CAP],real_error[4096];
static struct {wchar_t relative[FIXTURE_PATH_CAP];bool directory;} real_paths[96];
static size_t real_count;
static void real_path(const wchar_t *relative,wchar_t *path) {
    CHECK(wcslen(real_root)+wcslen(relative)+2<FIXTURE_PATH_CAP);
    swprintf(path,FIXTURE_PATH_CAP,*relative?L"%ls\\%ls":L"%ls%ls",real_root,relative);
    for (wchar_t *q=path;*q;++q) if (*q==L'/') *q=L'\\';
}
static void real_native_path(const wchar_t *relative,wchar_t *path) {
    wchar_t absolute[FIXTURE_PATH_CAP];real_path(relative,absolute);
    CHECK(wcslen(absolute)+5<FIXTURE_PATH_CAP);swprintf(path,FIXTURE_PATH_CAP,L"\\\\?\\%ls",absolute);
}
static void record_path(const wchar_t *relative,bool directory) {
    for (size_t i=0;i<real_count;++i) if (!wcscmp(real_paths[i].relative,relative)) return;
    CHECK(real_count<sizeof(real_paths)/sizeof(*real_paths));
    wcscpy(real_paths[real_count].relative,relative);real_paths[real_count++].directory=directory;
}
static void real_directory(const wchar_t *relative) {
    wchar_t path[FIXTURE_PATH_CAP];real_native_path(relative,path);
    CHECK(CreateDirectoryW(path,NULL));record_path(relative,true);
}
static void real_bytes(const wchar_t *relative,const void *bytes,size_t count) {
    wchar_t path[FIXTURE_PATH_CAP];real_native_path(relative,path);
    HANDLE file=CreateFileW(path,GENERIC_WRITE,0,NULL,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,NULL);CHECK(file!=INVALID_HANDLE_VALUE);
    DWORD written=0;CHECK(WriteFile(file,bytes,(DWORD)count,&written,NULL) && written==count);
    CHECK(CloseHandle(file));record_path(relative,false);
}
static void real_text(const wchar_t *relative,const wchar_t *text,bool bom) {
    int length=(int)wcslen(text),bytes=length?WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,text,length,NULL,0,NULL,NULL):0;
    CHECK(!length || bytes>0);size_t prefix=bom?3:0;
    unsigned char *data=malloc((size_t)bytes+prefix+1);CHECK(data);
    if (bom) {data[0]=0xef;data[1]=0xbb;data[2]=0xbf;}
    if (bytes) CHECK(WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,text,length,(char *)data+prefix,bytes,NULL,NULL)==bytes);
    real_bytes(relative,data,(size_t)bytes+prefix);free(data);
}
static bool real_load(const wchar_t *entry) {
    real_text(L"main.conf",entry,false);wchar_t path[FIXTURE_PATH_CAP];real_path(L"main.conf",path);
    return sp_config_load(&cfg,path,real_error,4096);
}
static void real_bad(const wchar_t *entry,const wchar_t *message,const wchar_t *where) {
    CHECK(!real_load(entry));CHECK(wcsstr(real_error,message));CHECK(wcsstr(real_error,where));
}
static void test_real_imports(void) {
    wchar_t root[128];swprintf(root,128,L"build/config-import-fixtures-%lu-%lu",(unsigned long)GetCurrentProcessId(),(unsigned long)GetTickCount());
    DWORD length=GetFullPathNameW(root,FIXTURE_PATH_CAP,real_root,NULL);CHECK(length && length<FIXTURE_PATH_CAP);
    real_directory(L"");real_directory(L"sub");real_directory(L"sub/nested");real_directory(L"profiles");real_directory(L"cwd");
    real_text(L"sub/first.conf",L"import nested/second.conf\nimport ../profiles/default.conf\n",false);
    real_text(L"sub/nested/second.conf",L"tint-opacity .25\nnever-elevate\nall\nend-never-elevate\n",true);
    const wchar_t *profile=L"profile default\nnever-elevate\nend-never-elevate\nbind Ctrl+Alt+S scratch.toggle\nend\n";
    real_text(L"profiles/default.conf",profile,false);
    real_text(L"profiles/work.conf",L"profile work\ntint-opacity .5\nbind Ctrl+Alt+S scratch.toggle\nend\n",true);
    wchar_t main_path[FIXTURE_PATH_CAP],cwd[FIXTURE_PATH_CAP];static wchar_t previous[32768];
    CHECK(GetCurrentDirectoryW(32768,previous)>0);real_path(L"main.conf",main_path);real_path(L"cwd",cwd);
    real_text(L"main.conf",L"import sub/first.conf\nimport profiles/work.conf\ntint-opacity .75\n",true);
    CHECK(SetCurrentDirectoryW(cwd));CHECK(sp_config_load(&cfg,main_path,real_error,4096));CHECK(SetCurrentDirectoryW(previous));
    CHECK(cfg.count==2 && !*real_error && cfg.tint_opacity.thousandths==750);
    CHECK(sp_tint_alpha(&cfg.profiles[0])==(750u*255u+500u)/1000u && sp_tint_alpha(&cfg.profiles[1])==128);
    for (size_t i=0;i<cfg.count;++i) CHECK(sp_never_elevate_all(&cfg.profiles[i]) && !cfg.profiles[i].never_elevate.all);
    wchar_t absolute[FIXTURE_PATH_CAP],entry[FIXTURE_PATH_CAP];real_path(L"profiles/default.conf",absolute);
    for (wchar_t *q=absolute;*q;++q) if (*q==L'\\') *q=L'/';
    CHECK(wcslen(absolute)+32<FIXTURE_PATH_CAP);swprintf(entry,FIXTURE_PATH_CAP,L"import \"%ls\"\n",absolute);
    CHECK(real_load(entry) && cfg.count==1);
    swprintf(entry,FIXTURE_PATH_CAP,L"import \"%ls\"\n",absolute+2);CHECK(real_load(entry)); /* Containing drive's root. */
    real_directory(L"%APPDATA%");real_text(L"%APPDATA%/literal.conf",L"tint-opacity .5\n",false);
    CHECK(real_load(L"import %APPDATA%/literal.conf\nimport profiles/default.conf\n"));CHECK(sp_tint_alpha(&cfg.profiles[0])==128);
    real_text(L"Case-\u96ea.conf",L"tint-opacity .25\n",false);
    real_bad(L"import Case-\u96ea.conf\nimport case-\u96ea.CONF\n",L"duplicate import",L"main.conf:2:");
    CHECK(wcsstr(real_error,L"import chain:") && wcsstr(real_error,L"first loaded via:"));
    real_bad(L"import Case-\u96ea.conf\nimport sub/../Case-\u96ea.conf\n",L"duplicate import",L"main.conf:2:");
    real_text(L"sub/cycle.conf",L"import ../main.conf\n",false);
    real_bad(L"import sub/cycle.conf\n",L"duplicate import",L"cycle.conf:1:");
    CHECK(wcsstr(real_error,L"import chain:"));
    real_bad(L"# header\nimport missing.conf\n",L"Cannot open configuration",L"main.conf:2:");CHECK(wcsstr(real_error,L"missing.conf"));
    real_bad(L"import sub\n",L"regular file",L"main.conf:1:");
    static const struct {const wchar_t *path,*message;} rejected[]={
        {L"\\\\?\\C:\\bad.conf",L"Device/namespace"},{L"\\\\.\\NUL",L"Device/namespace"},
        {L"//?/C:/bad",L"Device/namespace"},{L"//./NUL",L"Device/namespace"},
        {L"C:relative.conf",L"Drive-relative"},{L"C:",L"Drive-relative"},
        {L"*.conf",L"globs/wildcards"},{L"sub/?.conf",L"globs/wildcards"},
        {L"file.conf:stream",L"alternate streams"},{L"//server/pipe/test",L"regular file"},
    };
    for (size_t i=0;i<sizeof(rejected)/sizeof(*rejected);++i) {
        swprintf(entry,FIXTURE_PATH_CAP,L"# header\nimport %ls\n",rejected[i].path);real_bad(entry,rejected[i].message,L"main.conf:2:");
    }
    static const unsigned char invalid_utf8[]={0xf0,0x28,0x8c,0xbc},embedded_nul[]={'#',0,'\n'};
    real_bytes(L"bad.conf",invalid_utf8,sizeof(invalid_utf8));real_bad(L"import bad.conf\n",L"valid UTF-8",L"bad.conf:1:");
    real_bytes(L"bad.conf",embedded_nul,sizeof(embedded_nul));real_bad(L"import bad.conf\n",L"embedded NUL",L"bad.conf:1:");
    real_text(L"bad.conf",L"# header\nnever-elevate\nall\n",true);
    real_bad(L"import bad.conf\nend-never-elevate\n",L"unterminated never-elevate",L"bad.conf:2:");
    real_text(L"bad.conf",L"profile split\nbind Win+S scratch.toggle\n",false);
    real_bad(L"import bad.conf\nend\n",L"missing end",L"bad.conf:1:");
    real_bytes(L"empty.conf","",0);real_bytes(L"bom-only.conf","\xef\xbb\xbf",3);
    CHECK(real_load(L"import empty.conf\nimport bom-only.conf\nimport profiles/default.conf\n"));
    const wchar_t *sized=L"import padding-a.conf\nimport padding-b.conf\nimport profiles/default.conf\n";
    size_t remainder=SP_CONFIG_CHARS-wcslen(sized)-wcslen(profile)-SP_CONFIG_FILE_BYTES;
    unsigned char *padding=malloc(SP_CONFIG_FILE_BYTES+1);CHECK(padding);memset(padding,'x',SP_CONFIG_FILE_BYTES+1);padding[0]='#';
    real_bytes(L"padding-a.conf",padding,SP_CONFIG_FILE_BYTES);real_bytes(L"padding-b.conf",padding,remainder);
    CHECK(real_load(sized));real_bytes(L"padding-b.conf",padding,remainder+1);
    real_bad(sized,L"aggregate configuration exceeds 131072",L"default.conf:1:");
    real_bytes(L"padding-a.conf",padding,SP_CONFIG_FILE_BYTES+1);
    real_bad(L"import padding-a.conf\n",L"65536 bytes per file",L"padding-a.conf:1:");free(padding);
    /* Long-path handling is exercised without relying on the test EXE's manifest. */
    wchar_t long_relative[1024]=L"";
    for (unsigned i=0;i<7;++i) {
        if (i) wcscat(long_relative,L"/");wcscat(long_relative,L"long-directory-01234567890123456789");real_directory(long_relative);
    }
    wcscat(long_relative,L"/profile.conf");real_text(long_relative,profile,false);
    swprintf(entry,FIXTURE_PATH_CAP,L"import \"%ls\"\n",long_relative);CHECK(real_load(entry));
    real_path(long_relative,absolute);CHECK(wcslen(absolute)>260);
    DWORD handles_before=0,handles_after=0;CHECK(GetProcessHandleCount(GetCurrentProcess(),&handles_before));
    for (unsigned i=0;i<32;++i) real_bad(L"import sub/cycle.conf\n",L"duplicate import",L"cycle.conf:1:");
    CHECK(GetProcessHandleCount(GetCurrentProcess(),&handles_after) && handles_after==handles_before);
    /* All error capacities, including zero and NULL, are safe for file-backed errors. */
    for (size_t cap=0;cap<32;++cap) {
        wchar_t guarded[34];wmemset(guarded,L'!',34);
        CHECK(!sp_config_load(&cfg,main_path,guarded+1,cap));
        CHECK(guarded[0]==L'!' && guarded[cap+1]==L'!');if (cap) CHECK(guarded[cap]==0);
    }
    CHECK(!sp_config_load(&cfg,main_path,NULL,0));
    while (real_count) {
        --real_count;wchar_t path[FIXTURE_PATH_CAP];real_native_path(real_paths[real_count].relative,path);
        CHECK(real_paths[real_count].directory?RemoveDirectoryW(path):DeleteFileW(path));
    }
}
static size_t read_fixture(const char *path,char *text,size_t cap) {
    FILE *file=fopen(path,"rb");CHECK(file);size_t count=fread(text,1,cap-1,file);
    CHECK(!ferror(file) && feof(file));CHECK(!fclose(file));text[count]=0;return count;
}
static void test_import_example_and_sample(void) {
    static char reference[65537],example[65537];
    size_t size=read_fixture("examples/imports/main.conf",example,sizeof(example));
    read_fixture("docs/CONFIGURATION.md",reference,sizeof(reference));
    const char *fence="```scratchpad\n",*block=reference;bool documented=false;
    while ((block=strstr(block,fence))) {
        block+=strlen(fence);
        if (!strncmp(block,example,size) && !strncmp(block+size,"```",3)) documented=true;
    }
    CHECK(documented);
    CHECK(sp_config_load(&cfg,L"examples/imports/main.conf",real_error,4096));
    const SpProfile *def=sp_config_profile(&cfg,L"default"),*work=sp_config_profile(&cfg,L"work");
    CHECK(cfg.count==2 && def && work && def->binding_count==3 && work->binding_count==3);
    CHECK(cfg.tint_opacity.thousandths==140 && sp_tint_alpha(def)==36 && sp_tint_alpha(work)==128);
    CHECK(!def->tint_opacity.set && work->tint_opacity.thousandths==500);
    CHECK(cfg.never_elevate.all && !cfg.never_elevate.count);
    CHECK(sp_never_elevate_all(def) && sp_never_elevate_all(work));
    CHECK(!sp_never_elevate_count(def) && sp_never_elevate_count(work)==1);
    CHECK(!wcscmp(sp_never_elevate_entry(work,0),L"work-tool.exe"));
    CHECK(def->global_tint_opacity==&cfg.tint_opacity && work->global_never_elevate==&cfg.never_elevate);
    CHECK(sp_config_load(&cfg,L"scratchpad.conf",real_error,4096));
    CHECK(cfg.count==2 && cfg.never_elevate.all && !cfg.tint_opacity.set);
    CHECK(cfg.profiles[0].binding_count==5 && cfg.profiles[1].binding_count==9);
    for (size_t i=0;i<cfg.count;++i) {
        const SpProfile *p=&cfg.profiles[i];
        CHECK(sp_never_elevate_all(p) && sp_tint_alpha(p)==36);
        CHECK(!p->terminal_profile && !p->container_profile && !p->container_directory);
        CHECK(!wcscmp(p->container_filter,L"label=scratchpad=1"));
        for (size_t j=0;j<p->binding_count;++j) {
            CHECK(!(p->bindings[j].chord.mods&SP_MOD_WIN));
            CHECK(wcscmp(p->bindings[j].action,L"terminal.container")); /* Explicit opt-in only. */
        }
    }
    static SpConfig builtin;sp_config_defaults(&builtin);
    CHECK(builtin.profiles[0].binding_count==cfg.profiles[0].binding_count);
    for (size_t i=0;i<builtin.profiles[0].binding_count;++i) {
        CHECK(sp_same_chord(builtin.profiles[0].bindings[i].chord,cfg.profiles[0].bindings[i].chord));
        CHECK(!wcscmp(builtin.profiles[0].bindings[i].action,cfg.profiles[0].bindings[i].action));
    }
}
int main(void) {
    const DWORD errors[]={ERROR_ACCESS_DENIED,ERROR_BAD_NETPATH,ERROR_NETWORK_UNREACHABLE,
        ERROR_SHARING_VIOLATION,ERROR_INVALID_NAME,ERROR_GEN_FAILURE};
    for (size_t i=0;i<sizeof(errors)/sizeof(errors[0]);++i) {
        refused(errors[i],true); refused(errors[i],false);
    }
    const DWORD absent[]={ERROR_FILE_NOT_FOUND,ERROR_PATH_NOT_FOUND};
    for (size_t i=0;i<2;++i) for (size_t j=0;j<2;++j) {
        reset();appdata_error=absent[i];adjacent_error=absent[j];
        wchar_t diagnostic[256]={0};
        CHECK(fixture_config_load(&cfg,NULL,diagnostic,256));
        CHECK(lookups==2 && opens==0 && cfg.count==1 && cfg.profiles[0].binding_count==5);
        CHECK(!diagnostic[0]);
    }
    for (size_t i=0;i<2;++i) {
        reset();environment_present=false;
        environment_error=i?ERROR_SUCCESS:ERROR_ENVVAR_NOT_FOUND;
        wchar_t diagnostic[256]={0};
        CHECK(fixture_config_load(&cfg,NULL,diagnostic,256));
        CHECK(lookups==1 && cfg.count==1);
    }
    reset();environment_present=false;environment_error=ERROR_ACCESS_DENIED;
    wchar_t diagnostic[256]={0};
    CHECK(!fixture_config_load(&cfg,NULL,diagnostic,256));
    CHECK(lookups==0 && opens==0 && cfg.count==0 && wcsstr(diagnostic,L"APPDATA"));
    /* A present but unreadable file is an error, never an excuse to change keymaps. */
    for (size_t i=0;i<2;++i) {
        reset();
        if (i) adjacent_attributes=FILE_ATTRIBUTE_NORMAL; else appdata_attributes=FILE_ATTRIBUTE_NORMAL;
        CHECK(!fixture_config_load(&cfg,NULL,diagnostic,256));
        CHECK(opens==1 && lookups==i+1 && cfg.count==0);
    }
    test_path_resolution();test_real_imports();test_import_example_and_sample();
    printf("config discovery/imports: %u checks passed (injected discovery/UNC; real build-owned files; no user config reads)\n",checks);
    return 0;
}
