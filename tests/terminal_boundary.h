/* Test-only C projections of Terminal's delimiter/reconstruction boundary.
   Provenance, accepted subset and limitations: docs/TERMINAL-BOUNDARY.md.
   These run after the real CRT serializer plus Windows' argv decoder.
   No real Terminal or container is started. */
#include <shellapi.h>
static size_t terminal_commands(const wchar_t *const *args,size_t count) {
    size_t commands=1;
    for (size_t i=0;i<count;++i) for (const wchar_t *p=args[i];*p;++p)
        if (*p==L';' && (p==args[i] || p[-1]!=L'\\')) ++commands;
    return commands;
}
static bool terminal_child_roundtrip(const wchar_t *const *args,size_t count) {
    if (!count) return true;
    wchar_t command[SP_COMMAND_CAP]={0};
    for (size_t i=0;i<count;++i) {
        bool quoted=wcschr(args[i],L' ')!=NULL;
        if (wcslen(command)+wcslen(args[i])+4>=SP_COMMAND_CAP) return false;
        /* Upstream tests tellp(), not the index: an initial empty arg vanishes. */
        if (*command) wcscat(command,L" ");
        if (quoted) wcscat(command,L"\"");
        wcscat(command,args[i]);
        if (quoted) wcscat(command,L"\"");
    }
    if (!*command) return false; /* Avoid CommandLineToArgvW's empty-input special case. */
    int decoded_count=0;wchar_t **decoded=CommandLineToArgvW(command,&decoded_count);
    bool same=decoded && (size_t)decoded_count==count;
    for (size_t i=0;same && i<count;++i) same=!wcscmp(decoded[i],args[i]);
    if (decoded) LocalFree(decoded);
    return same;
}
static void check_boundary(const wchar_t *exe,const wchar_t *const *args,size_t count) {
    wchar_t command[SP_COMMAND_CAP];CHECK(sp_commandline(command,SP_COMMAND_CAP,args,count));
    int decoded_count=0;wchar_t **decoded=CommandLineToArgvW(command,&decoded_count);
    CHECK(decoded && decoded_count==(int)count);
    for (size_t i=0;i<count;++i) CHECK(!wcscmp(decoded[i],args[i]));
    if (wcsstr(exe,L"wt.exe")) {
        CHECK(terminal_commands((const wchar_t *const *)decoded,count)==1);
        size_t child=count;
        for (size_t i=4;i<count;++i) if (!wcscmp(decoded[i],L"--")) {child=i+1;break;}
        CHECK(child<count || count==4 || (count==6 && !wcscmp(decoded[4],L"-p")));
        CHECK(terminal_child_roundtrip((const wchar_t *const *)(decoded+child),count-child));
    }
    LocalFree(decoded);
}
static void test_parser_witnesses(void) {
    const wchar_t *plain[]={L"wt.exe",L"-w",L"_quake",L"nt",L"-p",L"Dev Tools"};
    const wchar_t *profile[]={L"wt.exe",L"-w",L"_quake",L"nt",L"-p",L"Dev;Tools"};
    const wchar_t *window[]={L"wt.exe",L"-w",L"one;two",L"nt",L"-p",L"Dev"};
    const wchar_t *child[]={L"wt.exe",L"-w",L"_quake",L"nt",L"-p",L"Dev",L"--",L"tool.exe",L"one;two"};
    CHECK(terminal_commands(plain,6)==1);CHECK(terminal_commands(profile,6)==2);
    CHECK(terminal_commands(window,6)==2);CHECK(terminal_commands(child,9)==2);
    const wchar_t *empty[]={L"tool.exe",L""};
    const wchar_t *quote[]={L"tool.exe",L"a\"b"};
    const wchar_t *tab[]={L"tool.exe",L"a\tb"};
    const wchar_t *odd_slash[]={L"tool.exe",L"C:\\Some Folder\\",L"tail"};
    const wchar_t *even_slash[]={L"tool.exe",L"C:\\Some Folder\\\\",L"tail"};
    CHECK(!terminal_child_roundtrip(empty,2));CHECK(!terminal_child_roundtrip(quote,2));
    CHECK(!terminal_child_roundtrip(tab,2));CHECK(!terminal_child_roundtrip(odd_slash,3));
    CHECK(!terminal_child_roundtrip(even_slash,3));
}
static void test_restrictions(SpProfile p) {
    wchar_t message[1024];SpBinding b={0};b.action=L"terminal.open";
    const wchar_t *unsafe[]={L"Dev;Tools",L";",L"Dev\\;Tools",L"Dev\tTools"};
    for (size_t i=0;i<sizeof(unsafe)/sizeof(unsafe[0]);++i) {
        unsigned before=spawns;SpProfile changed=p;
        changed.terminal_profile=unsafe[i];
        CHECK(!fixture_terminal_run(&changed,&b,NULL,message,1024));CHECK(spawns==before && wcsstr(message,L"Windows Terminal"));
        changed=p;changed.quake_window=unsafe[i];
        CHECK(!fixture_terminal_run(&changed,&b,NULL,message,1024));CHECK(spawns==before);
        changed=p;changed.container_profile=unsafe[i];b.action=L"terminal.container";
        CHECK(!fixture_terminal_run(&changed,&b,NULL,message,1024));CHECK(spawns==before);b.action=L"terminal.open";
    }
    /* Metadata isn't reconstructed as a child command line. */
    p.terminal_profile=L"Dev \"Quoted\"\\";p.quake_window=L"--help";
    CHECK(fixture_terminal_run(&p,&b,NULL,message,1024));
    CHECK(!wcscmp(launch[2],p.quake_window) && !wcscmp(launch[5],p.terminal_profile));
    p.quake_window=L"-1";CHECK(fixture_terminal_run(&p,&b,NULL,message,1024));
    b.action=L"terminal.quake";b.argc=3;b.args[0]=L"--help";b.args[1]=L"tool.exe";
    const wchar_t *bad_child[]={L"",L"a;b",L"a\\;b",L"a\"b",L"a\tb",L"a\177b",
        L"C:\\Some Folder\\",L"C:\\Some Folder\\\\"};
    for (size_t i=0;i<sizeof(bad_child)/sizeof(bad_child[0]);++i) {
        b.args[2]=bad_child[i];unsigned before=spawns;
        CHECK(!fixture_terminal_run(&p,&b,NULL,message,1024));CHECK(spawns==before && *message);
    }
    const wchar_t *good_child[]={L"one & two",L"C:\\Tools\\",L"\\\\server\\share",L"snow-\u96ea",L"--"};
    for (size_t i=0;i<sizeof(good_child)/sizeof(good_child[0]);++i) {
        b.args[2]=good_child[i];CHECK(fixture_terminal_run(&p,&b,NULL,message,1024));
        CHECK(launch_count==9 && !wcscmp(launch[6],L"--") && !wcscmp(launch[8],good_child[i]));
    }
    b.args[0]=L"Dev;Tools";unsigned before=spawns;
    CHECK(!fixture_terminal_run(&p,&b,NULL,message,1024));CHECK(spawns==before);
    b.args[0]=L"";CHECK(!fixture_terminal_run(&p,&b,NULL,message,1024));CHECK(spawns==before);
    b.args[0]=L"Dev";b.args[1]=L"";CHECK(!fixture_terminal_run(&p,&b,NULL,message,1024));CHECK(spawns==before);
    b.args[1]=L"--help";CHECK(fixture_terminal_run(&p,&b,NULL,message,1024));
    CHECK(!wcscmp(launch[6],L"--") && !wcscmp(launch[7],L"--help"));
    b.action=L"terminal.open";b.argc=0;before=spawns;unsafe_wt_path=true;
    CHECK(!fixture_terminal_run(&p,&b,NULL,message,1024));CHECK(spawns==before);unsafe_wt_path=false;
    b.action=L"terminal.container";unsafe_podman_path=true;output_fixture("one\n");
    CHECK(!fixture_terminal_run(&p,&b,NULL,message,1024));CHECK(spawns==before);unsafe_podman_path=false;
    /* Container command/directory values traverse the same child boundary. */
    for (size_t i=0;i<sizeof(bad_child)/sizeof(bad_child[0]);++i) {
        unsigned queried=queries;before=spawns;
        b.argc=1;b.args[0]=bad_child[i];
        CHECK(!fixture_terminal_run(&p,&b,NULL,message,1024));CHECK(spawns==before && queries==queried);
        b.argc=2;b.args[0]=L"/bin/sh";b.args[1]=bad_child[i];
        CHECK(!fixture_terminal_run(&p,&b,NULL,message,1024));CHECK(spawns==before && queries==queried);
        SpProfile changed=p;changed.container_directory=bad_child[i];b.argc=0;
        CHECK(!fixture_terminal_run(&changed,&b,NULL,message,1024));CHECK(spawns==before && queries==queried);
    }
    /* Restrictions must not leak into the generic executable serializer. */
    b.action=L"process.exec";b.argc=6;b.args[0]=L"custom-module.exe";
    b.args[1]=L"";b.args[2]=L"a;b";b.args[3]=L"a\"b";b.args[4]=L"C:\\Some Folder\\";b.args[5]=L"a\tb";
    CHECK(fixture_terminal_run(&p,&b,NULL,message,1024));CHECK(launch_count==6);
    for (size_t i=1;i<b.argc;++i) CHECK(!wcscmp(launch[i],b.args[i]));
    before=spawns;b.args[0]=L"";
    CHECK(!fixture_terminal_run(&p,&b,NULL,message,1024));CHECK(spawns==before);
}
