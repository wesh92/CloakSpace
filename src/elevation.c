#include "elevation.h"
#include "launch_args.h"
#include <shellapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#define SP_INSPECT_DEPTH 8u
#define SP_INSPECT_ARGS 64u

static bool same(const wchar_t *a,const wchar_t *b) {
    return CompareStringOrdinal(a,-1,b,-1,TRUE)==CSTR_EQUAL;
}
static bool starts(const wchar_t *s,const wchar_t *prefix) {
    size_t n=wcslen(prefix);
    return wcslen(s)>=n && CompareStringOrdinal(s,(int)n,prefix,(int)n,TRUE)==CSTR_EQUAL;
}
static bool separator(wchar_t c) { return c==L'/' || c==L'\\'; }
static bool full_path(const wchar_t *s) {
    size_t n=wcslen(s);
    return (n>=3 && ((s[0]>=L'A'&&s[0]<=L'Z')||(s[0]>=L'a'&&s[0]<=L'z')) &&
            s[1]==L':' && separator(s[2])) || (n>2 && separator(s[0]) && separator(s[1]));
}
static const wchar_t *basename_of(const wchar_t *s) {
    /* A drive-qualified relative target (C:tool.exe) has no directory slash;
       its alphabetic drive prefix is not part of the executable basename. */
    if (((s[0]>=L'A'&&s[0]<=L'Z')||(s[0]>=L'a'&&s[0]<=L'z')) && s[1]==L':') s+=2;
    const wchar_t *base=s;
    for (;*s;++s) if (separator(*s)) base=s+1;
    return base;
}
static size_t program_length(const wchar_t *s) {
    size_t n=wcslen(s);
    /* Ordinary Win32 names ignore final dots/spaces; .exe is optional in the
       existing resolver. Do not apply executable suffix rules to script paths. */
    while (n && (s[n-1]==L'.'||s[n-1]==L' ')) --n;
    if (n>=4 && CompareStringOrdinal(s+n-4,4,L".exe",4,TRUE)==CSTR_EQUAL) n-=4;
    return n;
}
static bool program(const wchar_t *a,const wchar_t *b) {
    a=basename_of(a); b=basename_of(b);
    size_t an=program_length(a),bn=program_length(b);
    return an && an==bn && CompareStringOrdinal(a,(int)an,b,(int)bn,TRUE)==CSTR_EQUAL;
}
static bool reject(const SpProfile *p,const wchar_t *rule,const wchar_t *why,wchar_t *out,size_t cap) {
    if (out && cap) {
        if (rule) swprintf(out,cap,L"Elevation blocked by never-elevate. Run normally from a non-elevated scratchpad; remove the elevation request. Active profile '%ls', rule (%ls).",p&&p->name?p->name:L"(unnamed)",rule);
        else swprintf(out,cap,L"Elevation blocked with active never-elevate. Run normally from a non-elevated scratchpad; remove the elevation request. Use an explicit executable and separate arguments, without elevation. Active profile '%ls': %ls.",p&&p->name?p->name:L"(unnamed)",why);
        out[cap-1]=0;
    }
    return false;
}
/* Lexical Win32 comparison only: no PATH lookup, network/filesystem access,
   junction/short-name resolution or script evaluation. Extended/device paths
   are deliberately not accepted as proof of a non-match. */
static wchar_t *normalized_path(const wchar_t *path,bool inherited_cwd) {
    if ((!inherited_cwd && !full_path(path)) || starts(path,L"\\\\?\\") || starts(path,L"\\\\.\\") ||
        starts(path,L"//?/") || starts(path,L"//./")) return NULL;
    size_t n=wcslen(path);
    if (n>=SP_COMMAND_CAP) return NULL;
    wchar_t *clean=calloc(n+1,sizeof(*clean));
    if (!clean) return NULL;
    size_t used=0;
    for (size_t i=0;i<n;) {
        if (separator(path[i])) { clean[used++]=L'\\'; ++i; continue; }
        size_t end=i;
        while (end<n && !separator(path[end])) ++end;
        size_t trim=end;
        bool dots=(end-i==1 && path[i]==L'.') || (end-i==2 && path[i]==L'.' && path[i+1]==L'.');
        if (!dots) while (trim>i && (path[trim-1]==L' '||path[trim-1]==L'.')) --trim;
        if (trim==i) { free(clean); return NULL; }
        memcpy(clean+used,path+i,(trim-i)*sizeof(*clean)); used+=trim-i; i=end;
    }
    DWORD size=GetFullPathNameW(clean,0,NULL,NULL);
    wchar_t *out=size && size<=SP_COMMAND_CAP?calloc(size,sizeof(*out)):NULL;
    DWORD got=out?GetFullPathNameW(clean,size,out,NULL):0;
    free(clean);
    if (!got || got>=size) { free(out); return NULL; }
    return out;
}
/* Unlike executable names, script filenames retain their full extensions. */
static bool possible_relative_name(const wchar_t *expected,const wchar_t *candidate) {
    const wchar_t *a=basename_of(expected),*b=basename_of(candidate);
    if (b==candidate && b[0] && b[1]==L':') b+=2;
    /* Final dot components can select a name in the unknown parent CWD. */
    if (same(b,L".") || same(b,L"..")) return true;
    size_t an=wcslen(a),bn=wcslen(b);
    while (an && (a[an-1]==L'.'||a[an-1]==L' ')) --an;
    while (bn && (b[bn-1]==L'.'||b[bn-1]==L' ')) --bn;
    return an && an==bn && CompareStringOrdinal(a,(int)an,b,(int)bn,TRUE)==CSTR_EQUAL;
}
/* 1 match, 0 no match, -1 cannot safely compare a path. Relative operands use
   only a known inherited CWD; a Terminal profile's child CWD is not the host's.
   Expected policy paths always stay absolute, independent of the current CWD. */
static int matching(const SpProfile *p,const wchar_t *exe,const wchar_t *const *argv,size_t argc,bool cwd_known,bool native_exe,const wchar_t **rule) {
    for (size_t i=0;i<sp_never_elevate_count(p);++i) {
        const wchar_t *entry=sp_never_elevate_entry(p,i);
        if (!entry || !*entry) return -1;
        if (!wcspbrk(entry,L"\\/:")) {
            if (program(entry,exe)) { *rule=entry; return 1; }
            continue;
        }
        wchar_t *expected=normalized_path(entry,false);
        if (!expected) return -1;
        for (size_t j=0;j<=argc;++j) {
            const wchar_t *candidate=j?argv[j-1]:exe;
            bool relative=!full_path(candidate);
            if (relative) {
                if (!*candidate) continue;
                /* Only the outer native executable has our absolute-PATH
                   resolver. A broker's bare target is itself a path operand. */
                if (j<2 && native_exe && cwd_known && !wcspbrk(candidate,L"\\/:")) continue;
                if (!cwd_known) {
                    if (!possible_relative_name(expected,candidate)) continue;
                    free(expected); return -1;
                }
            }
            wchar_t *actual=normalized_path(candidate,relative);
            if (!actual) {
                if (relative && !possible_relative_name(expected,candidate)) continue;
                free(expected); return -1;
            }
            bool equal=same(expected,actual); free(actual);
            if (equal) { free(expected); *rule=entry; return 1; }
        }
        free(expected);
    }
    return 0;
}
static bool elevated_token(bool *elevated) {
    HANDLE token=NULL; DWORD size=0; TOKEN_ELEVATION elevation={0}; bool ok=false;
    if (!OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY,&token)) return false;
    if (!GetTokenInformation(token,TokenElevation,&elevation,sizeof(elevation),&size)) goto done;
    GetTokenInformation(token,TokenIntegrityLevel,NULL,0,&size);
    if (!size) goto done;
    TOKEN_MANDATORY_LABEL *label=malloc(size);
    if (!label) goto done;
    if (GetTokenInformation(token,TokenIntegrityLevel,label,size,&size) && IsValidSid(label->Label.Sid)) {
        BYTE count=*GetSidSubAuthorityCount(label->Label.Sid);
        if (count) {
            *elevated=elevation.TokenIsElevated || *GetSidSubAuthority(label->Label.Sid,count-1)>=SECURITY_MANDATORY_HIGH_RID;
            ok=true;
        }
    }
    free(label);
done:
    CloseHandle(token); return ok;
}
static bool inspect(const SpProfile *,const wchar_t *,const wchar_t *const *,size_t,bool,bool,unsigned,wchar_t *,size_t);
static bool opaque(const SpProfile *p,wchar_t *out,size_t cap) {
    return reject(p,NULL,L"cannot safely identify the target of an elevation request",out,cap);
}
static bool packed(const SpProfile *p,const wchar_t *command,bool elevated,bool cwd_known,unsigned depth,wchar_t *out,size_t cap) {
    if (!*command || wcspbrk(command,L"&|<>;`$%\r\n")) return opaque(p,out,cap);
    /* With no explicit application name, Win32 can try successively longer
       unquoted absolute executable prefixes. CommandLineToArgvW does not model
       that selection. Require quoting even when an absolute packed command's
       first space might merely separate arguments; no filesystem guessing. */
    if (command[0]!=L'"' && full_path(command) && wcspbrk(command,L" \t")) return opaque(p,out,cap);
    int count=0; wchar_t **args=CommandLineToArgvW(command,&count);
    if (!args || count<1 || count>(int)SP_INSPECT_ARGS) { if(args)LocalFree(args); return opaque(p,out,cap); }
    bool ok=inspect(p,args[0],(const wchar_t *const *)args,(size_t)count,elevated,cwd_known,depth+1,out,cap);
    LocalFree(args); return ok;
}
static bool help_only(const wchar_t *const *args,size_t n) {
    return n==2 && (same(args[1],L"--help")||same(args[1],L"-h")||same(args[1],L"/?")||same(args[1],L"--version"));
}
static bool broker(const SpProfile *p,const wchar_t *exe,const wchar_t *const *args,size_t n,bool inherited,bool cwd_known,unsigned depth,wchar_t *out,size_t cap) {
    bool runas=program(exe,L"runas");
    if (help_only(args,n)) return true;
    if (sp_never_elevate_all(p)) return reject(p,L"all",NULL,out,cap);
    const wchar_t *rule=NULL; int match=matching(p,exe,args,1,cwd_known,depth==0,&rule);
    if (match) return match>0?reject(p,rule,NULL,out,cap):opaque(p,out,cap);
    size_t i=1;
    if (runas) {
        for (;i<n && args[i][0]==L'/';++i) {
            if (!(starts(args[i],L"/user:")||starts(args[i],L"/trustlevel:")||same(args[i],L"/profile")||
                  same(args[i],L"/noprofile")||same(args[i],L"/env")||same(args[i],L"/netonly")||
                  same(args[i],L"/savecred")||same(args[i],L"/smartcard"))) return opaque(p,out,cap);
        }
        if (i+1!=n) return opaque(p,out,cap);
        return packed(p,args[i],true,cwd_known,depth,out,cap);
    }
    /* Only argv-transparent execution forms are accepted as known targets.
       Unknown broker options, shell/cache modes and missing targets fail closed. */
    for (;i<n;++i) {
        if (same(args[i],L"--")) { ++i; break; }
        if (args[i][0]!=L'-') break;
        if (!(same(args[i],L"-n")||same(args[i],L"--new")||same(args[i],L"-w")||same(args[i],L"--wait")||
              same(args[i],L"-d")||same(args[i],L"--direct")||same(args[i],L"--inline")||
              same(args[i],L"--new-window")||same(args[i],L"--disable-input"))) return opaque(p,out,cap);
    }
    if (i<n && program(exe,L"sudo") && same(args[i],L"run")) ++i;
    if (i>=n || same(args[i],L"cache")||same(args[i],L"config")||same(args[i],L"!!")) return opaque(p,out,cap);
    (void)inherited;
    /* Absolute targets can still be packed shell commands or ambiguous path
       prefixes. Their final basename is not proof of an unrelated executable. */
    if (wcspbrk(args[i],L" \t")) {
        if (i+1!=n) return opaque(p,out,cap);
        return packed(p,args[i],true,cwd_known,depth,out,cap);
    }
    return inspect(p,args[i],args+i,n-i,true,cwd_known,depth+1,out,cap);
}
/* Literal token character classes from PowerShell's parser/CharTraits.cs.
   Do not use the C locale's narrower whitespace/quote classifications. */
static bool ps_space(wchar_t c) {
    return c && (wcschr(L" \t\v\f\r\n\x0085\u00a0\u1680\u2028\u2029\u202f\u205f\u3000",c) ||
                 (c>=L'\u2000' && c<=L'\u200a'));
}
static bool ps_dash(wchar_t c) {
    return c==L'-' || c==L'\u2013' || c==L'\u2014' || c==L'\u2015';
}
static wchar_t ps_quote(wchar_t c) {
    switch (c) {
    case L'\'': case L'\u2018': case L'\u2019': case L'\u201a': case L'\u201b': return L'\'';
    case L'"': case L'\u201c': case L'\u201d': case L'\u201e': return L'"';
    default: return 0;
    }
}
/* Classify the already-decoded literal Verb exactly once. Retained quotes,
   expressions and malformed/empty values cannot prove a harmless verb. */
static int literal_verb(const wchar_t *value) {
    size_t n=wcslen(value);
    if (!n) return -1;
    for (size_t i=0;i<n;++i) if (ps_quote(value[i]) || ps_space(value[i]) || wcschr(L"`$;&|(){}<>,",value[i])) return -1;
    return n==5 && CompareStringOrdinal(value,5,L"RunAs",5,TRUE)==CSTR_EQUAL;
}
/* Native ConsoleHost GetSwitchKey/MatchSwitch prefix rules. File/default-file
   and uninspected language forms end this scan; their operands are not flags.
   Minimum spellings are explicit, rather than inferred across host versions. */
static size_t ps_command_start(const wchar_t *const *args,size_t n,bool legacy,bool *cwd_known) {
    enum { PS_FLAG,PS_VALUE,PS_CWD,PS_COMMAND,PS_STOP,PS_VERSION,PS_CONSOLE };
    static const struct { const wchar_t *full,*minimum; int kind; } options[]={
        {L"version",L"v",PS_VERSION},
        {L"help",L"h",PS_STOP},{L"?",L"?",PS_STOP},{L"login",L"l",PS_FLAG},
        {L"noexit",L"noe",PS_FLAG},{L"noprofile",L"nop",PS_FLAG},
        {L"nologo",L"nol",PS_FLAG},{L"noninteractive",L"noni",PS_FLAG},
        {L"socketservermode",L"so",PS_FLAG},{L"v2socketservermode",L"v2so",PS_FLAG},
        {L"servermode",L"s",PS_FLAG},{L"namedpipeservermode",L"nam",PS_FLAG},
        {L"sshservermode",L"sshs",PS_FLAG},{L"noprofileloadtime",L"noprofileloadtime",PS_FLAG},
        {L"interactive",L"i",PS_FLAG},
        {L"configurationfile",L"configurationfile",PS_VALUE},
        {L"configurationname",L"config",PS_VALUE},{L"custompipename",L"cus",PS_VALUE},
        {L"commandwithargs",L"commandwithargs",PS_STOP},{L"cwa",L"cwa",PS_STOP},
        {L"command",L"c",PS_COMMAND},{L"windowstyle",L"w",PS_VALUE},
        {L"file",L"f",PS_STOP},
        {L"outputformat",L"o",PS_VALUE},{L"of",L"o",PS_VALUE},
        {L"inputformat",L"inp",PS_VALUE},{L"if",L"if",PS_VALUE},
        {L"executionpolicy",L"ex",PS_VALUE},{L"ep",L"ep",PS_VALUE},
        {L"encodedcommand",L"e",PS_STOP},{L"ec",L"e",PS_STOP},
        {L"encodedarguments",L"encodeda",PS_VALUE},{L"ea",L"ea",PS_VALUE},
        {L"settingsfile",L"settings",PS_VALUE},{L"sta",L"sta",PS_FLAG},{L"mta",L"mta",PS_FLAG},
        {L"workingdirectory",L"wo",PS_CWD},{L"wd",L"wd",PS_CWD},
        {L"removeworkingdirectorytrailingcharacter",L"removeworkingdirectorytrailingcharacter",PS_FLAG},
        {L"token",L"to",PS_VALUE},{L"utctimestamp",L"utc",PS_VALUE},
        {L"psconsolefile",L"p",PS_CONSOLE}
    };
    for (size_t i=1;i<n;++i) {
        const wchar_t *key=args[i]; size_t length=wcslen(key);
        while (length && ps_space(*key)) { ++key; --length; }
        while (length && ps_space(key[length-1])) --length;
        /* Keep scanning invalid/empty prefixes conservatively; installed pwsh
           rejects an empty file operand rather than evaluating a later -c. */
        if (!length) continue;
        /* Legacy defaults to Command; pwsh defaults to File. */
        if (!ps_dash(*key) && *key!=L'/') return legacy ? i : n;
        wchar_t dash=*key++; --length;
        if (length && ps_dash(dash) && *key==dash) { ++key; --length; }
        bool found=false;
        for (size_t j=0;j<sizeof(options)/sizeof(*options);++j) {
            if (length<wcslen(options[j].minimum) || length>wcslen(options[j].full) ||
                CompareStringOrdinal(key,(int)length,options[j].full,(int)length,TRUE)!=CSTR_EQUAL) continue;
            found=true;
            int kind=options[j].kind;
            if (kind==PS_COMMAND) return i+1;
            if (kind==PS_STOP || (!legacy && (kind==PS_VERSION || kind==PS_CONSOLE))) return n;
            /* Native valued options consume even a parameter-looking value. */
            if (kind!=PS_FLAG) {
                if (++i>=n) return n;
                if (kind==PS_CWD) *cwd_known=false;
            }
            break;
        }
        if (!found) return legacy ? i : n; /* Same host-dependent default for unknown switches. */
    }
    return n;
}
/* Deliberately small *literal* PowerShell command recognizer, not a PS parser.
   It detects flat Start-Process -Verb RunAs, including a quoted command token.
   Expressions/script bodies remain trusted code, not an enforceable sandbox. */
static bool powershell(const SpProfile *p,const wchar_t *exe,const wchar_t *const *args,size_t n,bool elevated,bool cwd_known,unsigned depth,wchar_t *out,size_t cap) {
    size_t command=ps_command_start(args,n,program(exe,L"powershell"),&cwd_known);
    if (elevated) {
        /* Native prefix metadata can change where -File operands resolve.
           Classify that context before matching any inherited elevated args. */
        const wchar_t *rule=NULL; int match=matching(p,exe,args,n,cwd_known,depth==0,&rule);
        if (match) return match>0?reject(p,rule,NULL,out,cap):opaque(p,out,cap);
    }
    if (command==n) return true;
    /* Native PowerShell joins ALL arguments after -Command with spaces before
       parsing. Native argv grouping must not change our literal classification. */
    size_t length=0;
    for (size_t i=command;i<n;++i) {
        size_t part=wcslen(args[i]);
        if (part>=SP_COMMAND_CAP-length) return opaque(p,out,cap);
        length+=part;
        if (i+1<n) { if (length+1>=SP_COMMAND_CAP) return opaque(p,out,cap); ++length; }
    }
    wchar_t *storage=calloc(length+1,sizeof(*storage));
    if (!storage) return opaque(p,out,cap);
    wchar_t *end=storage;
    for (size_t i=command;i<n;++i) {
        size_t part=wcslen(args[i]); memcpy(end,args[i],part*sizeof(*end)); end+=part;
        if (i+1<n) *end++=L' ';
    }
    const wchar_t *words[SP_INSPECT_ARGS]; bool parameters[SP_INSPECT_ARGS];
    size_t count=0; bool malformed=false,start=false;
    wchar_t *s=storage,*word=storage;
    while (*s) {
        while (*s && ps_space(*s)) ++s;
        if (!*s) break;
        if (count==SP_INSPECT_ARGS) { free(storage); return opaque(p,out,cap); }
        wchar_t *token=word; words[count++]=token; wchar_t quote=0;
        bool parameter=ps_dash(*s),parameter_name=true;
        while (*s) {
            wchar_t c=*s++;
            if (quote) {
                if (ps_quote(c)==quote) {
                    /* A doubled quote retains its second glyph, even when
                       the two glyphs differ within the same quote class. */
                    if (ps_quote(*s)==quote) *word++=*s++;
                    else quote=0;
                } else *word++=c;
            }
            else if (ps_quote(c)) {
                /* Quotes in a name make the entire token positional data;
                   quotes after its raw colon only delimit the value. */
                if (parameter_name) parameter=false;
                quote=ps_quote(c);
            }
            else if (ps_space(c)) break;
            else { if (c==L':') parameter_name=false; *word++=c; }
        }
        *word++=0;
        parameters[count-1]=parameter;
        if (parameter) token[0]=L'-';
        if (count==1) {
            start=same(words[0],L"Start-Process")||same(words[0],L"saps")||same(words[0],L"start");
            if (!start && !elevated) { free(storage); return true; }
        }
        if (quote) { malformed=true; break; }
    }
    bool runas=false,verb_seen=false,invalid_verb=false; const wchar_t *target=NULL; bool unknown=malformed;
    if (start) for (size_t i=1;i<count;++i) {
        if (!parameters[i]) {
            if (target) unknown=true;
            else target=words[i];
        }
        else if (same(words[i],L"-Verb") || starts(words[i],L"-Verb:")) {
            const wchar_t *value=L""; verb_seen=true;
            if (!same(words[i],L"-Verb")) value=words[i]+6;
            else if (i+1<count && !parameters[i+1]) value=words[++i];
            int kind=literal_verb(value); runas|=kind==1; invalid_verb|=kind<0;
        }
        else if (same(words[i],L"-FilePath")) {
            if (i+1>=count || parameters[i+1]) unknown=true;
            else { ++i; if (!target) target=words[i]; else unknown=true; }
        }
        else if (same(words[i],L"-Wait")||same(words[i],L"-PassThru")) continue;
        else if (same(words[i],L"-ArgumentList")||same(words[i],L"-WorkingDirectory")||same(words[i],L"-WindowStyle")) {
            if (same(words[i],L"-WorkingDirectory")) cwd_known=false;
            if (i+1>=count || parameters[i+1]) unknown=true;
            else if (wcspbrk(words[++i],L" \t,\"'`$;&|(){}")) unknown=true;
        } else unknown=true;
    }
    bool ok=true;
    if (invalid_verb || (malformed && verb_seen)) ok=opaque(p,out,cap);
    else if (runas) {
        if (unknown || !target || wcspbrk(target,L"\"'`$;&|(){}")) ok=opaque(p,out,cap);
        else ok=inspect(p,target,words,count,true,cwd_known,depth+1,out,cap);
    } else if (elevated) ok=opaque(p,out,cap);
    free(storage); return ok;
}
static bool terminal(const SpProfile *p,const wchar_t *const *args,size_t n,bool elevated,unsigned depth,wchar_t *out,size_t cap) {
    bool requested=false; size_t child=n;
    /* Terminal scans action separators even beyond -- and the first child.
       Validate the whole vector before any early authorization or metadata skip. */
    for (size_t i=0;i<n;++i) if (sp_terminal_value_error(args[i],false)) return opaque(p,out,cap);
    if (help_only(args,n)) return true;
    for (size_t i=1;i<n;++i) {
        const wchar_t *v=args[i];
        if (same(v,L"--")) { child=i+1; break; }
        if (same(v,L"--elevate")||same(v,L"--elevate=true")) { requested=true; continue; }
        if (same(v,L"--elevate=false")) continue;
        if (same(v,L"-p")||same(v,L"--profile")||same(v,L"-w")||same(v,L"--window")||same(v,L"-d")||
            same(v,L"--startingDirectory")||same(v,L"--title")||same(v,L"--tabColor")||same(v,L"--colorScheme")||
            same(v,L"--pos")||same(v,L"--size")) {
            if (++i>=n) return opaque(p,out,cap);
            continue;
        }
        if (same(v,L"nt")||same(v,L"new-tab")||same(v,L"sp")||same(v,L"split-pane")||
            same(v,L"--suppressApplicationTitle")||same(v,L"--useApplicationTitle")||
            same(v,L"-H")||same(v,L"-V")||same(v,L"-M")||same(v,L"-F")||same(v,L"-f")||
            same(v,L"--maximized")||same(v,L"--fullscreen")||same(v,L"--focus")) continue;
        if (v[0]==L'-' || v[0]==L'/') return opaque(p,out,cap);
        child=i; break;
    }
    /* Terminal reconstructs every child token, even for process.exec. Quotes,
       controls and unsafe trailing backslashes cannot prove a literal target. */
    for (size_t i=child;i<n;++i) if (sp_terminal_value_error(args[i],true)) return opaque(p,out,cap);
    if (requested && sp_never_elevate_all(p)) return reject(p,L"all",NULL,out,cap);
    if (child<n) return inspect(p,args[child],args+child,n-child,elevated||requested,false,depth+1,out,cap);
    return requested?opaque(p,out,cap):true;
}
static bool inspect(const SpProfile *p,const wchar_t *exe,const wchar_t *const *args,size_t n,bool elevated,bool cwd_known,unsigned depth,wchar_t *out,size_t cap) {
    if (depth>=SP_INSPECT_DEPTH || !exe || !*exe || !args || !n || n>SP_INSPECT_ARGS) return opaque(p,out,cap);
    for (size_t i=0;i<n;++i) if (!args[i]) return opaque(p,out,cap);
    bool is_broker=program(exe,L"runas")||program(exe,L"sudo")||program(exe,L"gsudo");
    bool is_terminal=program(exe,L"wt")||program(exe,L"wtd")||program(exe,L"WindowsTerminal");
    bool is_powershell=program(exe,L"pwsh")||program(exe,L"powershell");
    if (elevated) {
        if (sp_never_elevate_all(p)) return reject(p,L"all",NULL,out,cap);
        /* Dispatchers must classify their metadata/child CWD before operands
           are compared. Their own executable path can be checked immediately. */
        const wchar_t *rule=NULL; int match=matching(p,exe,args,is_broker||is_terminal||is_powershell?1:n,cwd_known,depth==0,&rule);
        if (match) return match>0?reject(p,rule,NULL,out,cap):opaque(p,out,cap);
        /* Reconstructed child targets can contain shell syntax. The top-level
           native executable instead has an explicit lpApplicationName. Packed
           runas quoting is removed by packed() before this check. */
        if (depth>0 && wcspbrk(exe,L"\"'&|<>;`$%\r\n")) return opaque(p,out,cap);
    }
    if (is_broker)
        return broker(p,exe,args,n,elevated,cwd_known,depth,out,cap);
    if (is_terminal)
        return terminal(p,args,n,elevated,depth,out,cap);
    if (is_powershell)
        return powershell(p,exe,args,n,elevated,cwd_known,depth,out,cap);
    if (elevated && (program(exe,L"cmd")||program(exe,L"bash")||program(exe,L"sh"))) return opaque(p,out,cap);
    return true;
}
bool sp_elevation_guard(const SpProfile *p,const wchar_t *exe,const wchar_t *const *args,size_t n,wchar_t *out,size_t cap) {
    if (!p) return reject(p,NULL,L"no profile is available to check launch policy",out,cap);
    if (!sp_never_elevate_all(p) && !sp_never_elevate_count(p)) return true;
    bool elevated=false;
    if (!elevated_token(&elevated)) return reject(p,NULL,L"cannot determine the launch token's elevation",out,cap);
    return inspect(p,exe,args,n,elevated,true,0,out,cap);
}
