#include "config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
static unsigned checks;
#define CHECK(x) do { ++checks; if (!(x)) { \
    fprintf(stderr,"%s:%d: %s\n",__FILE__,__LINE__,#x); exit(1); } } while(0)
static SpConfig cfg;
static void bad(const wchar_t *text) {
    wchar_t error[256]; CHECK(!sp_config_parse(&cfg,text,error,256)); CHECK(wcsstr(error,L"line "));
}
static void bad_message(const wchar_t *text,const wchar_t *message) {
    wchar_t error[256]; CHECK(!sp_config_parse(&cfg,text,error,256));
    CHECK(wcsstr(error,L"line ")==error); CHECK(wcsstr(error,message));
}
static void test_tint_values(void) {
    static const struct { const wchar_t *text;unsigned thousandths,alpha; } valid[]={
        {L"0",0,0},{L"1",1000,255},{L"0.5",500,128},{L".5",500,128},
        {L"1.0",1000,255},{L"0.25",250,64},{L"1.000",1000,255},
        {L"0.0",0,0},{L".000",0,0},{L"0.001",1,0},{L".002",2,1},
        {L"0.01",10,3},{L"0.05",50,13},{L"0.1",100,26},{L"0.14",140,36},
        {L"0.2",200,51},{L"0.3",300,77},{L".333",333,85},{L"0.499",499,127},
        {L".501",501,128},{L"0.7",700,179},{L"0.9",900,230},{L"0.999",999,255},
        {L"0000.250",250,64},{L"0001.000",1000,255},{L"\"0.5\"",500,128}
    };
    wchar_t text[1024],error[256];
    for (size_t i=0;i<sizeof(valid)/sizeof(*valid);++i) for (unsigned local=0;local<2;++local) {
        swprintf(text,1024,local?
            L"profile default\n\ttint-opacity %ls # local\nbind Win+S scratch.toggle\nend\n":
            L"tint-opacity %ls # global\nprofile default\nbind Win+S scratch.toggle\nend\n",valid[i].text);
        CHECK(sp_config_parse(&cfg,text,error,256));CHECK(!error[0]);
        const SpProfile *p=&cfg.profiles[0];
        const SpTintOpacity *tint=local?&p->tint_opacity:&cfg.tint_opacity;
        CHECK(tint->set && tint->thousandths==valid[i].thousandths);
        CHECK(p->global_tint_opacity==&cfg.tint_opacity);
        CHECK(sp_tint_alpha(p)==valid[i].alpha);
        CHECK(local?!cfg.tint_opacity.set:!p->tint_opacity.set);
    }
    /* Every representable value, including rounding transitions and half ties. */
    unsigned previous=0;
    for (unsigned value=0;value<=1000;++value) {
        swprintf(text,1024,L"tint-opacity %u.%03u\nprofile default\nbind Win+S scratch.toggle\nend\n",value/1000,value%1000);
        CHECK(sp_config_parse(&cfg,text,error,256));
        CHECK(cfg.tint_opacity.set && cfg.tint_opacity.thousandths==value);
        unsigned alpha=sp_tint_alpha(&cfg.profiles[0]);
        CHECK(alpha==(value*255u+500u)/1000u);CHECK(alpha>=previous && alpha<=255);
        previous=alpha;
    }
    /* A long run of leading zeroes must not overflow or change precision. */
    wmemset(text,L'0',512);text[512]=0;
    wchar_t long_value[1024];swprintf(long_value,1024,L"tint-opacity %ls.5\nprofile default\nbind Win+S scratch.toggle\nend",text);
    CHECK(sp_config_parse(&cfg,long_value,error,256));CHECK(sp_tint_alpha(&cfg.profiles[0])==128);
}
static void test_tint_scopes_and_defaults(void) {
    wchar_t error[256];
    sp_config_defaults(&cfg);
    CHECK(!cfg.tint_opacity.set && !cfg.profiles[0].tint_opacity.set);
    CHECK(cfg.profiles[0].global_tint_opacity==&cfg.tint_opacity);
    CHECK(sp_tint_alpha(&cfg.profiles[0])==36);
    SpProfile standalone={0};CHECK(sp_tint_alpha(&standalone)==36);CHECK(sp_tint_alpha(NULL)==36);
    standalone.tint_opacity=(SpTintOpacity){0,true};CHECK(sp_tint_alpha(&standalone)==0);
    standalone.tint_opacity=(SpTintOpacity){1000,true};CHECK(sp_tint_alpha(&standalone)==255);
    static const wchar_t *const global[]={L"",L"tint-opacity 0\n",L"tint-opacity 0.5\n",L"tint-opacity 1.000\n"};
    static const unsigned expected[]={36,0,128,255};
    for (size_t i=0;i<sizeof(global)/sizeof(*global);++i) for (unsigned late=0;late<2;++late) {
        wchar_t text[2048];
        swprintf(text,2048,L"%lsprofile inherited\nbind Win+S scratch.toggle\nend\n"
            L"profile local\ntint-opacity 0.14\nbind Win+S scratch.toggle\nend\n"
            L"profile clear\ntint-opacity 0\nbind Win+S scratch.toggle\nend\n%ls",
            late?L"":global[i],late?global[i]:L"");
        CHECK(sp_config_parse(&cfg,text,error,256));CHECK(cfg.count==3);
        CHECK(cfg.tint_opacity.set==(i!=0));
        CHECK(sp_tint_alpha(&cfg.profiles[0])==expected[i]);
        CHECK(sp_tint_alpha(&cfg.profiles[1])==36);CHECK(sp_tint_alpha(&cfg.profiles[2])==0);
        for (size_t j=0;j<cfg.count;++j) CHECK(cfg.profiles[j].global_tint_opacity==&cfg.tint_opacity);
    }
    CHECK(sp_config_parse(&cfg,L"tint-opacity 0.1\ntint-opacity 0.25\nnever-elevate\nall\nend-never-elevate\n"
        L"profile local\ntint-opacity 0.25\nbind Win+S scratch.toggle\ntint-opacity 1\ntint-opacity 0.5\nend\n"
        L"profile inherited\nbind Win+S scratch.toggle\nend\ntint-opacity 0.9\ntint-opacity 0\n",error,256));
    CHECK(cfg.tint_opacity.set && cfg.tint_opacity.thousandths==0);
    CHECK(cfg.profiles[0].tint_opacity.set && cfg.profiles[0].tint_opacity.thousandths==500);
    CHECK(sp_tint_alpha(&cfg.profiles[0])==128 && sp_tint_alpha(&cfg.profiles[1])==0);
    CHECK(sp_never_elevate_all(&cfg.profiles[0]) && sp_never_elevate_all(&cfg.profiles[1]));
    /* Reusing the same cfg for defaults/old explicit files must not retain tint. */
    sp_config_defaults(&cfg);CHECK(sp_tint_alpha(&cfg.profiles[0])==36);
    CHECK(!cfg.tint_opacity.set && !cfg.profiles[0].tint_opacity.set);
    CHECK(sp_config_parse(&cfg,L"profile default\nbind Win+S scratch.toggle\nend\n"
        L"profile other\nbind Win+S scratch.toggle\nend",error,256));
    CHECK(!cfg.tint_opacity.set);
    for (size_t i=0;i<cfg.count;++i) CHECK(!cfg.profiles[i].tint_opacity.set && sp_tint_alpha(&cfg.profiles[i])==36);
}
static void test_tint_malformed(void) {
    static const wchar_t *const invalid[]={
        L"",L"\"\"",L"# missing",L"-1",L"-0",L"-0.5",L"-.5",L"+0.5",L"+1",L"+0",
        L"1.001",L"1.5",L"2",L"10",L"9999999999999999999999999999999999999999999",
        L"0.1234",L".0000",L"1.0000",L"0.0000",L"0.5junk",L"0.5f",L"0x1",L"0x1p0",
        L"1e0",L"5e-1",L"1,0",L"0,5",L"50%",L"NaN",L"nan",L"inf",L"infinity",L"INF",
        L"1..0",L"..5",L".",L"0.",L"1.",L"0.5 0.6",L"0 1",L"\" \"",L"\" 0.5\"",L"\"0.5 \"",
        L"\u0661",L"\uff10.5",L"0\u066b5",L"'0.5'"
    };
    const wchar_t *message=L"tint-opacity must be a decimal from 0.0 to 1.0 (up to 3 decimal places)";
    for (size_t i=0;i<sizeof(invalid)/sizeof(*invalid);++i) {
        wchar_t text[2048];
        swprintf(text,2048,L"tint-opacity %ls\nprofile default\nbind Win+S scratch.toggle\nend\n",invalid[i]);
        bad_message(text,message);
        swprintf(text,2048,L"profile default\ntint-opacity %ls\nbind Win+S scratch.toggle\nend\n",invalid[i]);
        bad_message(text,message);
    }
    /* A valid preceding value must not make a later malformed value disappear. */
    bad_message(L"tint-opacity .5\ntint-opacity 1.001\nprofile default\nbind Win+S scratch.toggle\nend",L"line 2: tint-opacity");
    bad_message(L"profile default\ntint-opacity .5\ntint-opacity -0.5\nbind Win+S scratch.toggle\nend",L"line 3: tint-opacity");
    bad_message(L"never-elevate\ntint-opacity .5\nend-never-elevate\nprofile default\nbind Win+S scratch.toggle\nend",L"never-elevate needs one program");
    bad_message(L"profile default\nnever-elevate\ntint-opacity .5\nend-never-elevate\nbind Win+S scratch.toggle\nend",L"never-elevate needs one program");
    CHECK(!sp_config_parse(&cfg,L"tint-opacity 1.001",NULL,0));
    wchar_t untouched=L'!';CHECK(!sp_config_parse(&cfg,L"tint-opacity -1",&untouched,0));CHECK(untouched==L'!');
}
static void check_member_alpha(const SpProfile *p,bool active,unsigned expected) {
    SpAppearanceValue alpha=sp_member_alpha(p,active);
    CHECK(alpha.set==(expected!=0) && alpha.value==expected);
}
static void check_appearance_omitted(const SpProfile *p) {
    SpAppearance a=sp_appearance_resolve(p);
    CHECK(!a.tint_fade_ms.set && a.tint_fade_ms.value==0);
    CHECK(!a.member_opacity.set && !a.inactive_opacity.set && !a.active_border_rgb.set);
    check_member_alpha(p,true,0);check_member_alpha(p,false,0);
    CHECK(sp_tint_alpha(p)==36);
}
static void test_appearance_scopes_and_defaults(void) {
    sp_config_defaults(&cfg);
    CHECK(cfg.profiles[0].global_appearance==&cfg.appearance);
    check_appearance_omitted(&cfg.profiles[0]);
    check_appearance_omitted(NULL);
    SpProfile standalone={0};check_appearance_omitted(&standalone);
    CHECK(sp_appearance_set(&standalone.appearance,SP_APPEARANCE_INACTIVE_OPACITY,L".001"));
    check_member_alpha(&standalone,true,0);check_member_alpha(&standalone,false,1);

    const wchar_t *text=L"profile default\nbind Win+S scratch.toggle\nend\n"
        L"profile work\nbind Win+S scratch.toggle\nend\n";
    wchar_t error[256];CHECK(sp_config_parse(&cfg,text,error,256));
    SpProfile *def=&cfg.profiles[0],*work=&cfg.profiles[1];
    CHECK(def->global_appearance==&cfg.appearance && work->global_appearance==&cfg.appearance);
    check_appearance_omitted(def);check_appearance_omitted(work);

    SpAppearance *global=&cfg.appearance,*local=&work->appearance;
    /* Stage values through the internal setter, not a test-only DSL parser. */
    CHECK(sp_appearance_set(global,SP_APPEARANCE_TINT_FADE_MS,L"125"));
    CHECK(sp_appearance_set(global,SP_APPEARANCE_TINT_FADE_MS,L"500"));
    CHECK(sp_appearance_set(global,SP_APPEARANCE_MEMBER_OPACITY,L".25"));
    check_member_alpha(def,true,64);check_member_alpha(def,false,64);
    CHECK(sp_appearance_set(global,SP_APPEARANCE_MEMBER_OPACITY,L".5"));
    CHECK(sp_appearance_set(global,SP_APPEARANCE_INACTIVE_OPACITY,L".25"));
    CHECK(sp_appearance_set(global,SP_APPEARANCE_ACTIVE_BORDER,L"AbCdEf"));
    SpAppearance inherited=sp_appearance_resolve(def);
    CHECK(inherited.tint_fade_ms.set && inherited.tint_fade_ms.value==500);
    CHECK(inherited.member_opacity.set && inherited.member_opacity.value==500);
    CHECK(inherited.inactive_opacity.set && inherited.inactive_opacity.value==250);
    CHECK(inherited.active_border_rgb.set && inherited.active_border_rgb.value==0xabcdefu);
    check_member_alpha(def,true,128);check_member_alpha(def,false,64);

    CHECK(sp_appearance_set(local,SP_APPEARANCE_TINT_FADE_MS,L"200"));
    CHECK(sp_appearance_set(local,SP_APPEARANCE_TINT_FADE_MS,L"0"));
    CHECK(sp_appearance_set(local,SP_APPEARANCE_MEMBER_OPACITY,L"1.000"));
    CHECK(sp_appearance_set(local,SP_APPEARANCE_ACTIVE_BORDER,L"000000"));
    SpAppearance effective=sp_appearance_resolve(work);
    CHECK(effective.tint_fade_ms.set && effective.tint_fade_ms.value==0);
    CHECK(effective.active_border_rgb.set && effective.active_border_rgb.value==0);
    check_member_alpha(work,true,255);check_member_alpha(work,false,64); /* Global inactive beats local base. */
    CHECK(sp_appearance_set(local,SP_APPEARANCE_INACTIVE_OPACITY,L"0"));
    check_member_alpha(work,true,255);check_member_alpha(work,false,1);
    check_member_alpha(def,true,128);check_member_alpha(def,false,64);

    CHECK(sp_config_parse(&cfg,text,error,256)); /* Reused config must not retain staged values. */
    check_appearance_omitted(def);check_appearance_omitted(work);
    CHECK(sp_appearance_set(&cfg.appearance,SP_APPEARANCE_ACTIVE_BORDER,L"FFFFFF"));
    sp_config_defaults(&cfg);check_appearance_omitted(&cfg.profiles[0]);
}
static void test_appearance_validation(void) {
    static const wchar_t *const bad_fade[]={NULL,L"",L"-1",L"+0",L"501",L"1.5",L"0x1",L"1 2",
        L"\uff11",L"999999999999999999999999999"};
    static const wchar_t *const bad_opacity[]={NULL,L"",L"-0",L"+0.5",L"1.001",L"2",L".0001",L"0.",
        L"NaN",L"1e0",L"50%",L"0,5",L"0.5junk",L" 0.5"};
    static const wchar_t *const bad_border[]={NULL,L"",L"12345",L"1234567",L"#AABBCC",L"0x1234",
        L"GG0000",L"-12345",L"12 345",L"\uff1100000"};
    SpAppearance a={0};
    CHECK(sp_appearance_set(&a,SP_APPEARANCE_TINT_FADE_MS,L"500"));
    CHECK(sp_appearance_set(&a,SP_APPEARANCE_MEMBER_OPACITY,L"0"));
    CHECK(sp_appearance_set(&a,SP_APPEARANCE_INACTIVE_OPACITY,L".999"));
    CHECK(sp_appearance_set(&a,SP_APPEARANCE_ACTIVE_BORDER,L"FFFFFF"));
    for (size_t i=0;i<sizeof(bad_fade)/sizeof(*bad_fade);++i)
        CHECK(!sp_appearance_set(&a,SP_APPEARANCE_TINT_FADE_MS,bad_fade[i]));
    for (size_t i=0;i<sizeof(bad_opacity)/sizeof(*bad_opacity);++i) {
        CHECK(!sp_appearance_set(&a,SP_APPEARANCE_MEMBER_OPACITY,bad_opacity[i]));
        CHECK(!sp_appearance_set(&a,SP_APPEARANCE_INACTIVE_OPACITY,bad_opacity[i]));
    }
    for (size_t i=0;i<sizeof(bad_border)/sizeof(*bad_border);++i)
        CHECK(!sp_appearance_set(&a,SP_APPEARANCE_ACTIVE_BORDER,bad_border[i]));
    CHECK(!sp_appearance_set(NULL,SP_APPEARANCE_TINT_FADE_MS,L"1"));
    CHECK(!sp_appearance_set(&a,(SpAppearanceSetting)100,L"1"));
    CHECK(a.tint_fade_ms.set && a.tint_fade_ms.value==500);
    CHECK(a.member_opacity.set && a.member_opacity.value==0);
    CHECK(a.inactive_opacity.set && a.inactive_opacity.value==999);
    CHECK(a.active_border_rgb.set && a.active_border_rgb.value==0xffffffu);
}
static void test_unwired_appearance_is_rejected(void) {
    static const wchar_t *const proposed[]={L"tint-fade-ms 150",L"member-opacity .5",
        L"inactive-opacity .25",L"active-border ABCDEF"};
    const wchar_t *def=L"profile default\nbind Win+S scratch.toggle\nend\n";
    for (size_t i=0;i<sizeof(proposed)/sizeof(*proposed);++i) {
        wchar_t text[256];
        swprintf(text,256,L"%ls\n%ls",proposed[i],def);
        bad_message(text,L"line 1: directive must be inside a profile");
        swprintf(text,256,L"%lsprofile unused\nbind Win+S scratch.toggle\n%ls\nend\n",def,proposed[i]);
        bad_message(text,L"line 6: unknown directive");
    }
}
static void test_never_elevate_scopes(void) {
    wchar_t text[]=L"# global policy\r\nnever-elevate # shared\r\n"
        L"notepad.exe\r\n\t\"C:/Shared/# Ops/\u96ea.ps1\" # quoted comment marker\r\n"
        L"C:\\Scripts\\plain.ps1\r\nend-never-elevate\r\n"
        L"profile default\nlayout gap 19\nnever-elevate\npowershell\n"
        L"\"\\\\\\\\server\\\\share\\\\scripts\\\\work task.ps1\"\nend-never-elevate\n"
        L"bind Ctrl+Alt+S scratch.toggle\nnever-elevate\n\"tool # one.exe\"\nend-never-elevate\nend\n"
        L"profile other\nnever-elevate\n# deliberately empty\nend-never-elevate\n"
        L"bind Ctrl+Alt+S scratch.toggle\nend\n"
        L"never-elevate\npwsh.exe\nnotepad.exe\nend-never-elevate";
    static const wchar_t *const global[]={L"notepad.exe",L"C:/Shared/# Ops/\u96ea.ps1",L"C:\\Scripts\\plain.ps1",L"pwsh.exe",L"notepad.exe"};
    static const wchar_t *const local[]={L"powershell",L"\\\\server\\share\\scripts\\work task.ps1",L"tool # one.exe"};
    size_t globals=sizeof(global)/sizeof(*global),locals=sizeof(local)/sizeof(*local);
    wchar_t error[256]=L"stale error"; CHECK(sp_config_parse(&cfg,text,error,256)); CHECK(!error[0]);
    /* Parsed targets must not borrow memory from the caller's input. */
    wmemset(text,L'?',sizeof(text)/sizeof(*text)-1);
    const SpProfile *p=sp_config_profile(&cfg,L"default"),*other=sp_config_profile(&cfg,L"other");
    CHECK(p && other && cfg.count==2 && p->layout.gap==19 && p->binding_count==1);
    CHECK(cfg.never_elevate.count==globals && !cfg.never_elevate.all);
    CHECK(p->never_elevate.count==locals && !p->never_elevate.all);
    CHECK(p->global_never_elevate==&cfg.never_elevate && other->global_never_elevate==&cfg.never_elevate);
    CHECK(other->never_elevate.count==0 && !other->never_elevate.all);
    CHECK(!sp_never_elevate_all(p) && !sp_never_elevate_all(other));
    CHECK(sp_never_elevate_count(p)==globals+locals && sp_never_elevate_count(other)==globals);
    for (size_t i=0;i<globals;++i) {
        CHECK(!wcscmp(cfg.never_elevate.entries[i],global[i]));
        CHECK(sp_never_elevate_entry(p,i)==cfg.never_elevate.entries[i]);
        CHECK(sp_never_elevate_entry(other,i)==cfg.never_elevate.entries[i]);
    }
    for (size_t i=0;i<locals;++i) {
        CHECK(!wcscmp(p->never_elevate.entries[i],local[i]));
        CHECK(sp_never_elevate_entry(p,globals+i)==p->never_elevate.entries[i]);
    }
    CHECK(!sp_never_elevate_entry(p,globals+locals)); CHECK(!sp_never_elevate_entry(p,SIZE_MAX));
    CHECK(!sp_never_elevate_entry(other,globals));
}
static void test_never_elevate_empty_and_all(void) {
    wchar_t error[256];
    CHECK(sp_config_parse(&cfg,L"never-elevate\n\t# nothing\n\nend-never-elevate\n"
        L"profile default\nnever-elevate\nend-never-elevate\nbind Win+S scratch.toggle\nend\n"
        L"never-elevate\nend-never-elevate",error,256));
    CHECK(cfg.never_elevate.count==0 && !cfg.never_elevate.all);
    const SpProfile *p=&cfg.profiles[0]; CHECK(p->never_elevate.count==0 && !p->never_elevate.all);
    CHECK(!sp_never_elevate_all(p) && sp_never_elevate_count(p)==0 && !sp_never_elevate_entry(p,0));
    for (unsigned global=0;global<2;++global) for (unsigned local=0;local<2;++local) {
        wchar_t text[2048];
        swprintf(text,2048,L"profile default\nnever-elevate\nlocal.exe\n%lsend-never-elevate\n"
            L"never-elevate\nend-never-elevate\nbind Win+S scratch.toggle\nend\n"
            L"profile other\nbind Win+S scratch.toggle\nend\n"
            L"never-elevate\nshared.exe\n%lsend-never-elevate\nnever-elevate\nend-never-elevate\n",
            local?L"\"all\"\nall\n":L"",global?L"all\nall\n":L"");
        CHECK(sp_config_parse(&cfg,text,error,256)); p=&cfg.profiles[0];
        const SpProfile *other=&cfg.profiles[1];
        CHECK(cfg.never_elevate.all==!!global && p->never_elevate.all==!!local);
        CHECK(sp_never_elevate_all(p)==!!(global||local) && sp_never_elevate_all(other)==!!global);
        CHECK(sp_never_elevate_count(p)==2 && sp_never_elevate_count(other)==1);
        CHECK(!wcscmp(sp_never_elevate_entry(p,0),L"shared.exe"));
        CHECK(!wcscmp(sp_never_elevate_entry(p,1),L"local.exe"));
        CHECK(!sp_never_elevate_entry(p,2) && !other->never_elevate.all);
    }
    CHECK(sp_config_parse(&cfg,L"never-elevate\nALL\nall.exe\nend-never-elevate\n"
        L"profile default\nbind Win+S scratch.toggle\nend",error,256));
    CHECK(!sp_never_elevate_all(&cfg.profiles[0]) && sp_never_elevate_count(&cfg.profiles[0])==2);
    SpProfile standalone={0};
    CHECK(!sp_never_elevate_all(&standalone) && !sp_never_elevate_count(&standalone));
    CHECK(!sp_never_elevate_entry(&standalone,0));
    standalone.never_elevate.entries[0]=L"local.exe"; standalone.never_elevate.count=1; standalone.never_elevate.all=true;
    CHECK(sp_never_elevate_all(&standalone) && sp_never_elevate_count(&standalone)==1);
    CHECK(!wcscmp(sp_never_elevate_entry(&standalone,0),L"local.exe"));
    CHECK(!sp_never_elevate_entry(&standalone,1));
    CHECK(!sp_never_elevate_all(NULL) && !sp_never_elevate_count(NULL) && !sp_never_elevate_entry(NULL,0));
}
static void test_never_elevate_malformed(void) {
    static const struct { const wchar_t *text,*message; } invalid[]={
        {L"never-elevate all\n",L"never-elevate takes no arguments"},
        {L"never-elevate tool.exe\n",L"never-elevate takes no arguments"},
        {L"end-never-elevate\n",L"requires an open never-elevate"},
        {L"never-elevate\nend-never-elevate extra\n",L"end-never-elevate takes no arguments"},
        {L"never-elevate\nend-never-elevate\nend-never-elevate\n",L"requires an open never-elevate"},
        {L"never-elevate\nnever-elevate\n",L"expected end-never-elevate"},
        {L"never-elevate\nprofile nested\n",L"expected end-never-elevate"},
        {L"never-elevate\nend\n",L"expected end-never-elevate"},
        {L"never-elevate\nend never-elevate\n",L"expected end-never-elevate"},
        {L"never-elevate\none.exe two.exe\n",L"one program name or absolute script path"},
        {L"never-elevate\nall\nall extra\n",L"one program name or absolute script path"},
        {L"never-elevate\nbind Win+S scratch.toggle\n",L"one program name or absolute script path"},
        {L"never-elevate\nlayout gap 10\n",L"one program name or absolute script path"},
        {L"never-elevate\n\"\"\n",L"one program name or absolute script path"},
        {L"never-elevate\n\"   \"\n",L"one program name or absolute script path"},
        {L"never-elevate\n\"unclosed\n",L"invalid quoting/escape"},
        {L"never-elevate\n\"C:\\bad\\q.ps1\"\n",L"invalid quoting/escape"},
        {L"never-elevate\n\"tool.exe\"suffix\n",L"invalid quoting/escape"}
    };
    for (size_t i=0;i<sizeof(invalid)/sizeof(*invalid);++i) {
        wchar_t text[2048];
        swprintf(text,2048,L"%lsprofile default\nbind Win+S scratch.toggle\nend\n",invalid[i].text);
        bad_message(text,invalid[i].message);
        swprintf(text,2048,L"profile default\n%lsbind Win+S scratch.toggle\nend\n",invalid[i].text);
        bad_message(text,invalid[i].message);
    }
    static const wchar_t *const targets[]={L".",L"..",L"./script.ps1",L"scripts/task.ps1",L"C:task.ps1",
        L"/scripts/task.ps1",L"\\scripts\\task.ps1",L"1:/task.ps1",L"C:/",L"C:/scripts/",L"C:/scripts/.",L"C:/scripts/..",
        L"C:/task.ps1:stream",L"//server",L"//server/share",L"//server//task.ps1",L"//server/share/",L"*.exe",
        L"C:/scripts/?.ps1",L"bad|name.exe",L"\"bad\\\"name.exe\"",L"bad\x7f.exe",L"bad\tname.exe"};
    for (size_t i=0;i<sizeof(targets)/sizeof(*targets);++i) {
        wchar_t text[2048];
        swprintf(text,2048,L"never-elevate\n%ls\nend-never-elevate\nprofile default\nbind Win+S scratch.toggle\nend",targets[i]);
        bad(text);
        swprintf(text,2048,L"profile default\nnever-elevate\n%ls\nend-never-elevate\nbind Win+S scratch.toggle\nend",targets[i]);
        bad(text);
    }
    bad_message(L"never-elevate",L"line 1: unterminated never-elevate");
    bad_message(L"# header\r\nnever-elevate\r\nname.exe\r\n# comment",L"line 2: unterminated never-elevate");
    bad_message(L"profile default\nbind Win+S scratch.toggle\nnever-elevate\nname.exe\n",L"line 3: unterminated never-elevate");
    bad_message(L"profile default\nbind Win+S scratch.toggle\nnever-elevate\nname.exe\nend",L"line 5: expected end-never-elevate");
    bad_message(L"profile default\nnever-elevate\nend-never-elevate\nend",L"at least one binding");
    bad_message(L"profile default\nnever-elevate\nend-never-elevate\nbind Win+S scratch.toggle",L"missing end");
    bad_message(L"never-elevate\nend-never-elevate",L"no profiles");
    CHECK(!sp_config_parse(&cfg,L"never-elevate",NULL,0));
    wchar_t untouched=L'!'; CHECK(!sp_config_parse(&cfg,L"never-elevate",&untouched,0)); CHECK(untouched==L'!');
}
static void append(wchar_t *text,const wchar_t *suffix) {
    CHECK(wcslen(text)+wcslen(suffix)<=SP_CONFIG_CHARS); wcscat(text,suffix);
}
static void append_policy(wchar_t *text,size_t count,const wchar_t *prefix) {
    append(text,L"never-elevate\nall\n");
    for (size_t i=0;i<count;++i) {
        wchar_t entry[64]; swprintf(entry,64,L"%ls%zu.exe\n",prefix,i); append(text,entry);
        if (i==SP_MAX_NEVER_ELEVATE_ENTRIES/2) append(text,L"end-never-elevate\nnever-elevate\n");
    }
    append(text,L"end-never-elevate\n");
}
static void test_never_elevate_bounds(void) {
    static wchar_t text[SP_CONFIG_CHARS+1]; wchar_t error[256];
    text[0]=0; append_policy(text,SP_MAX_NEVER_ELEVATE_ENTRIES,L"global");
    append(text,L"profile default\nbind Win+S scratch.toggle\n");
    append_policy(text,SP_MAX_NEVER_ELEVATE_ENTRIES,L"local"); append(text,L"end\n");
    CHECK(sp_config_parse(&cfg,text,error,256)); const SpProfile *p=&cfg.profiles[0];
    CHECK(cfg.never_elevate.count==SP_MAX_NEVER_ELEVATE_ENTRIES && p->never_elevate.count==SP_MAX_NEVER_ELEVATE_ENTRIES);
    CHECK(sp_never_elevate_count(p)==2*SP_MAX_NEVER_ELEVATE_ENTRIES && sp_never_elevate_all(p));
    for (size_t i=0;i<SP_MAX_NEVER_ELEVATE_ENTRIES;++i) {
        wchar_t entry[64]; swprintf(entry,64,L"global%zu.exe",i); CHECK(!wcscmp(sp_never_elevate_entry(p,i),entry));
        swprintf(entry,64,L"local%zu.exe",i); CHECK(!wcscmp(sp_never_elevate_entry(p,SP_MAX_NEVER_ELEVATE_ENTRIES+i),entry));
    }
    CHECK(!sp_never_elevate_entry(p,2*SP_MAX_NEVER_ELEVATE_ENTRIES));
    for (unsigned local=0;local<2;++local) {
        text[0]=0;
        if (local) append(text,L"profile default\nbind Win+S scratch.toggle\n");
        append_policy(text,SP_MAX_NEVER_ELEVATE_ENTRIES+1,L"overflow");
        append(text,local?L"end\n":L"profile default\nbind Win+S scratch.toggle\nend\n");
        bad_message(text,L"maximum 128 per scope");
    }
    text[0]=0;
    for (size_t i=0;i<SP_MAX_PROFILES;++i) {
        wchar_t profile[128]; swprintf(profile,128,L"profile p%zu\nbind Win+S scratch.toggle\nend\n",i); append(text,profile);
        if (i==SP_MAX_PROFILES/2) append(text,L"never-elevate\nshared.exe\nend-never-elevate\n");
    }
    CHECK(sp_config_parse(&cfg,text,error,256)); CHECK(cfg.count==SP_MAX_PROFILES);
    for (size_t i=0;i<cfg.count;++i) {
        CHECK(cfg.profiles[i].global_never_elevate==&cfg.never_elevate);
        CHECK(sp_never_elevate_count(&cfg.profiles[i])==1 && !sp_never_elevate_all(&cfg.profiles[i]));
        CHECK(!wcscmp(sp_never_elevate_entry(&cfg.profiles[i],0),L"shared.exe"));
    }
}
typedef struct { const wchar_t *path,*name,*text,*from; } FakeConfigFile;
typedef struct { const FakeConfigFile *files;size_t count,calls; } FakeConfigFiles;
static wchar_t *clone_text(const wchar_t *text) {
    size_t count=wcslen(text)+1;wchar_t *copy=malloc(count*sizeof(*copy));
    CHECK(copy);memcpy(copy,text,count*sizeof(*copy));return copy;
}
static bool fake_config_loader(void *context,const wchar_t *from,const wchar_t *path,
        SpConfigSource *source,wchar_t *error,size_t cap) {
    FakeConfigFiles *fake=context;++fake->calls;
    for (size_t i=0;i<fake->count;++i) {
        const FakeConfigFile *f=&fake->files[i];
        if (wcscmp(path,f->path)) continue;
        if (f->from) CHECK(!wcscmp(from,f->from));
        source->text=clone_text(f->text);source->name=clone_text(f->name);return true;
    }
    if (error && cap) {swprintf(error,cap,L"missing fake import %ls",path);error[cap-1]=0;}
    return false;
}
static bool parse_imports(const wchar_t *text,FakeConfigFiles *fake,wchar_t *error,size_t cap) {
    fake->calls=0;
    return sp_config_parse_ex(&cfg,text,L"entry.conf",fake_config_loader,fake,error,cap);
}
static void import_bad(const wchar_t *text,FakeConfigFiles *fake,const wchar_t *message,const wchar_t *where) {
    wchar_t error[4096];CHECK(!parse_imports(text,fake,error,4096));
    CHECK(wcsstr(error,message));CHECK(wcsstr(error,where));
}
static void test_import_order_and_policy(void) {
    static const FakeConfigFile files[]={
        {L"globals.conf",L"cfg/globals.conf",L"tint-opacity .2\nnever-elevate\nglobal.exe\nend-never-elevate\nimport nested.conf\n",L"entry.conf"},
        {L"nested.conf",L"cfg/nested.conf",L"tint-opacity .3\nnever-elevate\nnested.exe\nend-never-elevate\n",L"cfg/globals.conf"},
        {L"profiles/default.conf",L"cfg/profiles/default.conf",L"profile default\nnever-elevate\nlocal.exe\nend-never-elevate\n"
            L"bind Ctrl+Alt+S scratch.toggle\nend\n",L"entry.conf"},
        {L"profiles/other.conf",L"cfg/profiles/other.conf",L"profile other\ntint-opacity 1\ntint-opacity .5\n"
            L"bind Ctrl+Alt+N process.exec \"tool.exe\" \"a b\"\nend\n",L"entry.conf"},
        {L"late globals.conf",L"cfg/late globals.conf",L"tint-opacity .4\nnever-elevate\nafter.exe\nend-never-elevate\n",L"entry.conf"},
    };
    FakeConfigFiles fake={files,sizeof(files)/sizeof(*files),0};wchar_t error[512];
    wchar_t entry[]=L"tint-opacity .1\nnever-elevate\nentry.exe\nend-never-elevate\nimport globals.conf\n"
        L"import profiles/default.conf\nimport profiles/other.conf\nimport \"late globals.conf\" # comment\n"
        L"tint-opacity .75\nnever-elevate\nend.exe\nend-never-elevate\n";
    CHECK(parse_imports(entry,&fake,error,512));CHECK(!*error && fake.calls==5 && cfg.count==2);
    wmemset(entry,L'?',sizeof(entry)/sizeof(*entry)-1);
    CHECK(cfg.tint_opacity.set && cfg.tint_opacity.thousandths==750);
    const SpProfile *p=sp_config_profile(&cfg,L"default"),*other=sp_config_profile(&cfg,L"other");CHECK(p && other);
    CHECK(!p->tint_opacity.set && sp_tint_alpha(p)==(750u*255u+500u)/1000u);
    CHECK(other->tint_opacity.thousandths==500 && sp_tint_alpha(other)==128);
    CHECK(!wcscmp(other->bindings[0].args[1],L"a b"));
    CHECK(cfg.never_elevate.count==5 && sp_never_elevate_count(p)==6 && sp_never_elevate_count(other)==5);
    const wchar_t *const expected[]={L"entry.exe",L"global.exe",L"nested.exe",L"after.exe",L"end.exe",L"local.exe"};
    for (size_t i=0;i<6;++i) CHECK(!wcscmp(sp_never_elevate_entry(p,i),expected[i]));
    for (size_t i=0;i<cfg.count;++i) {
        CHECK(cfg.profiles[i].global_never_elevate==&cfg.never_elevate);
        CHECK(cfg.profiles[i].global_tint_opacity==&cfg.tint_opacity);
        CHECK(!sp_never_elevate_all(&cfg.profiles[i]));
    }
    /* Empty imported/local blocks cannot remove all, regardless of file order. */
    const FakeConfigFile policies[]={
        {L"deny",L"deny.conf",L"never-elevate\nall\nend-never-elevate\n",NULL},
        {L"empty",L"empty.conf",L"never-elevate\nend-never-elevate\n",NULL},
        {L"profile",L"profile.conf",L"profile default\nnever-elevate\nend-never-elevate\nbind Win+S scratch.toggle\nend\n",NULL},
    };
    fake=(FakeConfigFiles){policies,3,0};
    const wchar_t *const orders[]={L"import deny\nimport empty\nimport profile\n",
        L"import profile\nimport deny\nimport empty\n",
        L"never-elevate\nall\nend-never-elevate\nimport empty\nimport profile\n"};
    for (size_t i=0;i<3;++i) {
        CHECK(parse_imports(orders[i],&fake,error,512));CHECK(sp_never_elevate_all(&cfg.profiles[0]));
        CHECK(!cfg.profiles[0].never_elevate.all && cfg.never_elevate.all);
    }
}
static void test_import_errors_and_identity(void) {
    bad_message(L"import one.conf",L"import requires a file-backed configuration");
    const wchar_t *const arities[]={L"import",L"import a b",L"import \"\""};
    FakeConfigFiles fake={0};
    for (size_t i=0;i<3;++i) {
        bad_message(arities[i],L"exactly one nonempty path");
        import_bad(arities[i],&fake,L"exactly one nonempty path",L"entry.conf:1:");CHECK(!fake.calls);
    }
    const wchar_t *const scopes[]={L"profile default\nimport file\n",L"never-elevate\nimport file\n",
        L"profile default\nnever-elevate\nimport file\n"};
    for (size_t i=0;i<3;++i) {
        import_bad(scopes[i],&fake,L"only at top level",L"entry.conf:");CHECK(!fake.calls);
    }
    import_bad(L"# header\nimport missing",&fake,L"missing fake import missing",L"entry.conf:2:");
    FakeConfigFile file={L"bad",L"broken.conf",L"# header\r\nnever-elevate\r\ntool.exe\r\n",NULL};
    fake=(FakeConfigFiles){&file,1,0};
    import_bad(L"import bad\nend-never-elevate",&fake,L"unterminated never-elevate",L"broken.conf:2:");
    file.text=L"profile default\nbind Win+S scratch.toggle\n";
    import_bad(L"import bad\nend",&fake,L"missing end",L"broken.conf:1:");
    file.text=L"# header\r\nprofile default\r\nunknown directive\r\nend\r\n";
    import_bad(L"import bad",&fake,L"unknown directive",L"broken.conf:3:");
    file.text=L"tint-opacity .5\n";
    import_bad(L"import bad",&fake,L"no profiles",L"entry.conf:1:");
    file.text=L"profile default\nbind Win+S scratch.toggle\nend\n";
    import_bad(L"profile default\nbind Win+S scratch.toggle\nend\nimport bad",&fake,L"duplicate",L"broken.conf:1:");
    const FakeConfigFile cycle[]={
        {L"a",L"a.conf",L"import root",L"entry.conf"},
        {L"root",L"ENTRY.CONF",L"",L"a.conf"},
    };
    fake=(FakeConfigFiles){cycle,2,0};wchar_t error[4096];
    CHECK(!parse_imports(L"import a",&fake,error,4096));CHECK(fake.calls==2);
    CHECK(wcsstr(error,L"a.conf:1: duplicate import"));
    CHECK(wcsstr(error,L"entry.conf -> a.conf -> ENTRY.CONF"));CHECK(wcsstr(error,L"first loaded via: entry.conf"));
    const FakeConfigFile diamond[]={
        {L"a",L"a.conf",L"import shared",L"entry.conf"},
        {L"b",L"b.conf",L"import shared",L"entry.conf"},
        {L"shared",L"shared.conf",L"tint-opacity .5",NULL},
    };
    fake=(FakeConfigFiles){diamond,3,0};
    CHECK(!parse_imports(L"import a\nimport b",&fake,error,4096));CHECK(fake.calls==4);
    CHECK(wcsstr(error,L"entry.conf -> b.conf -> shared.conf"));
    CHECK(wcsstr(error,L"first loaded via: entry.conf -> a.conf -> shared.conf"));
    const FakeConfigFile cases[]={
        {L"one",L"C:/\u00c4/Shared.conf",L"",NULL},
        {L"two",L"c:/\u00e4/shared.CONF",L"",NULL},
    };
    fake=(FakeConfigFiles){cases,2,0};
    import_bad(L"import one\nimport two",&fake,L"duplicate import",L"entry.conf:2:");
    /* Truncated diagnostics must remain NUL terminated and inside the buffer. */
    for (size_t cap=0;cap<32;++cap) {
        wchar_t guarded[34];wmemset(guarded,L'!',34);
        CHECK(!parse_imports(L"import one\nimport two",&fake,guarded+1,cap));
        CHECK(guarded[0]==L'!' && guarded[cap+1]==L'!');
        if (cap) CHECK(guarded[cap]==0);else CHECK(guarded[1]==L'!');
    }
    CHECK(!parse_imports(L"import one\nimport two",&fake,NULL,0));
    CHECK(!sp_config_parse_ex(&cfg,L"",NULL,fake_config_loader,&fake,error,4096));
}
static void test_import_depth_files_profiles(void) {
    wchar_t names[SP_CONFIG_MAX_FILES][32],texts[SP_CONFIG_MAX_FILES][128];
    FakeConfigFile files[SP_CONFIG_MAX_FILES];
    const wchar_t *profile=L"profile default\nbind Win+S scratch.toggle\nend\n";
    for (size_t i=0;i<SP_CONFIG_MAX_FILES;++i) {
        swprintf(names[i],32,L"f%zu",i);swprintf(texts[i],128,L"import f%zu",i+1);
        files[i]=(FakeConfigFile){names[i],names[i],texts[i],NULL};
    }
    FakeConfigFiles fake={files,SP_CONFIG_MAX_FILES,0};wchar_t error[4096];
    files[SP_CONFIG_MAX_DEPTH-1].text=profile;
    CHECK(parse_imports(L"import f0",&fake,error,4096));CHECK(fake.calls==SP_CONFIG_MAX_DEPTH);
    files[SP_CONFIG_MAX_DEPTH-1].text=texts[SP_CONFIG_MAX_DEPTH-1];
    import_bad(L"import f0",&fake,L"maximum import nesting depth is 8",L"f7:1:");
    CHECK(fake.calls==SP_CONFIG_MAX_DEPTH);
    wchar_t entry[4096]=L"";
    for (size_t i=0;i<SP_CONFIG_MAX_FILES-1;++i) {
        files[i].text=L"";wchar_t directive[64];swprintf(directive,64,L"import f%zu\n",i);wcscat(entry,directive);
    }
    wcscat(entry,profile);CHECK(parse_imports(entry,&fake,error,4096));CHECK(fake.calls==SP_CONFIG_MAX_FILES-1);
    wcscat(entry,L"import f31\n");
    import_bad(entry,&fake,L"maximum configuration file count is 32",L"entry.conf:");CHECK(fake.calls==SP_CONFIG_MAX_FILES-1);
    entry[0]=0;
    for (size_t i=0;i<SP_MAX_PROFILES;++i) {
        swprintf(texts[i],128,L"profile p%zu\nbind Win+S scratch.toggle\nend\n",i);files[i].text=texts[i];
        wchar_t directive[64];swprintf(directive,64,L"import f%zu\n",i);wcscat(entry,directive);
    }
    CHECK(parse_imports(entry,&fake,error,4096));CHECK(cfg.count==SP_MAX_PROFILES);
    files[SP_MAX_PROFILES].text=profile;wcscat(entry,L"import f8\n");
    import_bad(entry,&fake,L"maximum 8",L"f8:1:");
}
static void test_import_policy_bounds(void) {
    static wchar_t policy[8192];policy[0]=0;append_policy(policy,SP_MAX_NEVER_ELEVATE_ENTRIES,L"imported");
    const FakeConfigFile files[]={
        {L"policy",L"policy.conf",policy,NULL},
        {L"extra",L"extra.conf",L"never-elevate\none-more.exe\nend-never-elevate\n",NULL},
        {L"profiles",L"profiles.conf",L"profile local\nnever-elevate\nall\nend-never-elevate\nbind Win+S scratch.toggle\nend\n"
            L"profile other\nbind Win+S scratch.toggle\nend\n",NULL},
    };
    FakeConfigFiles fake={files,3,0};wchar_t error[512];
    CHECK(parse_imports(L"import profiles\n",&fake,error,512));
    CHECK(sp_never_elevate_all(&cfg.profiles[0]) && !sp_never_elevate_all(&cfg.profiles[1]));
    CHECK(!cfg.never_elevate.all);
    CHECK(parse_imports(L"import policy\nimport profiles\n",&fake,error,512));
    for (size_t i=0;i<cfg.count;++i) CHECK(sp_never_elevate_count(&cfg.profiles[i])==SP_MAX_NEVER_ELEVATE_ENTRIES);
    import_bad(L"import policy\nimport extra\nimport profiles\n",&fake,L"maximum 128 per scope",L"extra.conf:2:");
}
static void test_import_aggregate_limit(void) {
    const wchar_t *entry=L"import a\nimport b\nprofile default\nbind Win+S scratch.toggle\nend\n";
    size_t first=SP_CONFIG_FILE_BYTES,second=SP_CONFIG_CHARS-wcslen(entry)-first;
    CHECK(second<=SP_CONFIG_FILE_BYTES);
    wchar_t *a=malloc((first+1)*sizeof(*a)),*b=malloc((second+2)*sizeof(*b));CHECK(a && b);
    wmemset(a,L'x',first);a[0]=L'#';a[first]=0;
    wmemset(b,L'x',second+1);b[0]=L'#';b[second]=0;
    const FakeConfigFile files[]={{L"a",L"a.conf",a,NULL},{L"b",L"b.conf",b,NULL}};
    FakeConfigFiles fake={files,2,0};wchar_t error[512];
    CHECK(parse_imports(entry,&fake,error,512));CHECK(fake.calls==2 && cfg.count==1);
    b[second]=L'x';b[second+1]=0;
    import_bad(entry,&fake,L"aggregate configuration exceeds 131072",L"b.conf:1:");
    free(a);free(b);
    static wchar_t exact[SP_CONFIG_CHARS+2];
    const wchar_t *profile=L"profile default\nbind Win+S scratch.toggle\nend\n#";
    size_t prefix=wcslen(profile);wcscpy(exact,profile);wmemset(exact+prefix,L'x',SP_CONFIG_CHARS-prefix);exact[SP_CONFIG_CHARS]=0;
    CHECK(sp_config_parse(&cfg,exact,error,512));
    exact[SP_CONFIG_CHARS]=L'x';exact[SP_CONFIG_CHARS+1]=0;
    bad_message(exact,L"aggregate configuration exceeds 131072");
    printf("configuration storage: %zu bytes (static/heap allocation required; never copy)\n",sizeof(SpConfig));
}
int main(void) {
    test_import_order_and_policy();test_import_errors_and_identity();
    test_import_depth_files_profiles();test_import_policy_bounds();test_import_aggregate_limit();
    test_tint_values();test_tint_scopes_and_defaults();test_tint_malformed();
    test_appearance_scopes_and_defaults();test_appearance_validation();test_unwired_appearance_is_rejected();
    test_never_elevate_scopes(); test_never_elevate_empty_and_all();
    test_never_elevate_malformed(); test_never_elevate_bounds();
    sp_config_defaults(&cfg); CHECK(cfg.count==1);
    const SpProfile *p=sp_config_profile(&cfg,L"default"); CHECK(p && p->binding_count==5);
    CHECK(!p->terminal_profile && !p->container_profile && !p->container_directory);
    CHECK(!wcscmp(p->quake_window,L"_quake") && !wcscmp(p->container_filter,L"label=scratchpad=1"));
    CHECK(cfg.never_elevate.all && !cfg.never_elevate.count && p->global_never_elevate==&cfg.never_elevate);
    CHECK(!p->never_elevate.all && !p->never_elevate.count && sp_never_elevate_all(p));
    CHECK(!sp_never_elevate_count(p) && !sp_never_elevate_entry(p,0));
    unsigned mods=SP_MOD_CTRL|SP_MOD_ALT;
    CHECK(sp_match_binding(p,'S',0x1f,mods)==0);
    CHECK(sp_match_binding(p,'S',0x1f,mods|SP_MOD_SHIFT)==1);
    CHECK(sp_match_binding(p,'P',0x19,mods)==2);
    CHECK(sp_match_binding(p,'R',0x13,mods)==3);
    CHECK(sp_match_binding(p,0xc0,0x29,mods)==4);
    CHECK(sp_match_binding(p,0xde,0x29,mods)==4);
    CHECK(!wcscmp(p->bindings[4].action,L"terminal.open"));
    CHECK(sp_match_binding(p,'S',0x1f,SP_MOD_WIN)==SIZE_MAX);
    CHECK(sp_match_binding(p,'S',0x1f,SP_MOD_WIN|SP_MOD_SHIFT)==SIZE_MAX);
    for (size_t i=0;i<p->binding_count;++i) CHECK(!(p->bindings[i].chord.mods&SP_MOD_WIN));
    SpChord c,d; CHECK(sp_chord(L"Alt+Ctrl+Win+F24",&c)); CHECK(c.vk==0x87);
    CHECK(sp_chord(L"Win+Ctrl+Alt+F24",&d)); CHECK(sp_same_chord(c,d));
    CHECK(sp_chord(L"F13",&c)); CHECK(sp_chord(L"Ctrl+Scan11D",&c) && c.physical && c.scan==0x11d);
    CHECK(!sp_chord(L"Win+Win+S",&c)); CHECK(!sp_chord(L"S",&c));
    CHECK(!sp_chord(L"Win+Scan000",&c)); CHECK(!sp_chord(L"Win+F25",&c));
    CHECK(!sp_chord(L"Win+VK10",&c)); CHECK(!sp_chord(L"Win+",&c));
    const wchar_t *text=L"# user configuration\r\nprofile default\nlayout gap 20\nlayout split vertical\n"
        L"layout anchor bottom-right\nlayout single 80 60\nlayout multi 95 90\nquake-window \"_quake\"\n"
        L"terminal-profile \"My Terminal\" # comment\ncontainer-profile \"\u96ea\"\ncontainer-filter \"label=team=blue\"\n"
        L"container-directory \"/app/shared files\"\n"
        L"bind Win+S scratch.toggle\nbind Win+Alt+N process.exec \"C:/Tools/a.exe\" \"a b\" \"x\\\"y\" \"z\\\\\"\nend\n"
        L"profile work\nbind Ctrl+Alt+F1 window.center\nend\n";
    wchar_t error[256]; CHECK(sp_config_parse(&cfg,text,error,256)); CHECK(cfg.count==2);
    CHECK(!cfg.never_elevate.all && !cfg.never_elevate.count);
    for (size_t i=0;i<cfg.count;++i) {
        CHECK(cfg.profiles[i].global_never_elevate==&cfg.never_elevate);
        CHECK(!cfg.profiles[i].never_elevate.all && !cfg.profiles[i].never_elevate.count);
        CHECK(!sp_never_elevate_all(&cfg.profiles[i]) && !sp_never_elevate_count(&cfg.profiles[i]));
    }
    p=sp_config_profile(&cfg,L"default"); CHECK(p && p->layout.gap==20 && p->layout.split==SP_SPLIT_VERTICAL);
    CHECK(p->layout.mirror_x && p->layout.mirror_y && p->layout.single_w==80 && p->layout.multi_h==90);
    CHECK(!wcscmp(p->terminal_profile,L"My Terminal")); CHECK(!wcscmp(p->container_profile,L"\u96ea"));
    CHECK(!wcscmp(p->container_filter,L"label=team=blue"));
    CHECK(!wcscmp(p->container_directory,L"/app/shared files"));
    const SpProfile *work=sp_config_profile(&cfg,L"work");CHECK(work);
    CHECK(!work->terminal_profile && !work->container_profile && !work->container_directory);
    CHECK(!wcscmp(work->container_filter,L"label=scratchpad=1"));
    CHECK(p->bindings[1].argc==4 && !wcscmp(p->bindings[1].args[2],L"x\"y"));
    CHECK(!wcscmp(p->bindings[1].args[3],L"z\\")); CHECK(!sp_config_profile(&cfg,L"missing"));
    bad(L"profile default\nbind Win+S scratch.toggle\n");
    bad(L"profile x\nend\n"); bad(L"bind Win+S scratch.toggle\n");
    bad(L"profile x\nbind Win+S scratch.toggle\nbind Win+S scratch.restore\nend\n");
    bad(L"profile x\nbind Win+S scratch.toggle\nend\nprofile x\nbind Win+X scratch.restore\nend\n");
    bad(L"profile x\nlayout gap -1\nend\n"); bad(L"profile x\nlayout single 0 70\nend\n");
    bad(L"profile x\nlayout gap 99999999999999999999\nend\n");
    bad(L"profile x\nunknown value\nend\n");
    bad(L"profile x\nterminal-profile \"unclosed\nend\n");
    bad(L"profile x\nterminal-profile \"bad\\q\"\nend\n");
    static const wchar_t *const settings[]={L"quake-window",L"terminal-profile",L"container-profile",L"container-filter",L"container-directory"};
    for (size_t i=0;i<sizeof(settings)/sizeof(*settings);++i) {
        wchar_t input[1400],value[1026];
        swprintf(input,1400,L"profile default\n%ls \"\"\nbind Ctrl+Alt+S scratch.toggle\nend",settings[i]);
        bad_message(input,L"setting needs one nonempty value");
        wmemset(value,L'x',1025);value[1024]=0;
        swprintf(input,1400,L"profile default\n%ls %ls\nbind Ctrl+Alt+S scratch.toggle\nend",settings[i],value);
        CHECK(sp_config_parse(&cfg,input,error,256));
        value[1024]=L'x';value[1025]=0;
        swprintf(input,1400,L"profile default\n%ls %ls\nbind Ctrl+Alt+S scratch.toggle\nend",settings[i],value);
        bad_message(input,L"setting needs one nonempty value");
    }
    SpLayout o=sp_default_layout(); SpRect r[3]; SpRect area={-100,0,1000,500};
    o.gap=0; o.split=SP_SPLIT_HORIZONTAL;
    CHECK(sp_layout_with(area,3,r,3,o)==SP_OK);
    CHECK(r[0].x==-50 && r[0].w==450 && r[1].x==400 && r[1].w==225 && r[2].x==625);
    o.mirror_x=true; CHECK(sp_layout_with(area,3,r,3,o)==SP_OK);
    CHECK(r[0].x==400 && r[1].x==175 && r[2].x==-50);
    o.split=SP_SPLIT_VERTICAL; o.mirror_x=false; o.mirror_y=true;
    CHECK(sp_layout_with(area,3,r,3,o)==SP_OK); CHECK(r[0].y>r[1].y && r[1].y>r[2].y);
    o.split=SP_SPLIT_ALTERNATE; CHECK(sp_layout_with(area,3,r,3,o)==SP_OK);
    CHECK(r[0].w==450 && r[1].h<r[0].h && r[1].w==450);
    o.gap=-1; CHECK(!sp_valid_layout(o)); CHECK(sp_layout_with(area,1,r,3,o)==SP_BAD_ARGUMENT);
    printf("config/layout extensions: %u checks passed\n",checks); return 0;
}
