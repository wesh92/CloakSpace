#include "config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wctype.h>
#ifdef _WIN32
#include <windows.h> /* Ordinal, in-memory path comparison; no filesystem I/O. */
#endif

static bool number(const wchar_t *s, unsigned base, unsigned max, unsigned *out) {
    if (!s || !*s) return false;
    unsigned n=0;
    for (;*s;++s) {
        unsigned d=*s>=L'0'&&*s<=L'9'?(unsigned)(*s-L'0'):
            *s>=L'a'&&*s<=L'f'?(unsigned)(*s-L'a'+10):
            *s>=L'A'&&*s<=L'F'?(unsigned)(*s-L'A'+10):base;
        if (d>=base || d>max || n>(max-d)/base) return false;
        n=n*base+d;
    }
    *out=n; return true;
}
bool sp_chord(const wchar_t *text, SpChord *out) {
    if (!text || !out || !*text || wcslen(text)>=96) return false;
    wchar_t copy[96]; wcscpy(copy,text); SpChord chord={0};
    wchar_t *p=copy;
    for (;;) {
        wchar_t *plus=wcschr(p,L'+'); if (!plus) break;
        *plus=0; unsigned bit=0;
        if (!wcscmp(p,L"Win")) bit=SP_MOD_WIN;
        else if (!wcscmp(p,L"Ctrl")) bit=SP_MOD_CTRL;
        else if (!wcscmp(p,L"Shift")) bit=SP_MOD_SHIFT;
        else if (!wcscmp(p,L"Alt")) bit=SP_MOD_ALT;
        if (!bit || (chord.mods&bit)) return false;
        chord.mods|=bit; p=plus+1;
    }
    size_t len=wcslen(p); unsigned n=0;
    if (len==1 && ((*p>=L'A'&&*p<=L'Z')||(*p>=L'0'&&*p<=L'9'))) chord.vk=(unsigned)*p;
    else if (len==7 && !wcsncmp(p,L"Scan",4) && number(p+4,16,0x1ff,&n) && n) {
        chord.physical=true; chord.scan=n;
    } else if (len==4 && !wcsncmp(p,L"VK",2) && number(p+2,16,0xfe,&n) && n) chord.vk=n;
    else if (*p==L'F' && number(p+1,10,24,&n) && n>=1) chord.vk=0x70+n-1;
    else {
        static const struct { const wchar_t *name; unsigned vk; } names[]={
            {L"Space",0x20},{L"Enter",0x0d},{L"Tab",0x09},{L"Escape",0x1b},
            {L"Left",0x25},{L"Up",0x26},{L"Right",0x27},{L"Down",0x28},
            {L"Home",0x24},{L"End",0x23},{L"PageUp",0x21},{L"PageDown",0x22},
            {L"Insert",0x2d},{L"Delete",0x2e},{L"Backspace",0x08}};
        for (size_t i=0;i<sizeof(names)/sizeof(names[0]);++i)
            if (!wcscmp(p,names[i].name)) chord.vk=names[i].vk;
        if (!chord.vk) return false;
    }
    if (chord.vk==0x10||chord.vk==0x11||chord.vk==0x12||chord.vk==0x5b||chord.vk==0x5c||
        (chord.vk>=0xa0&&chord.vk<=0xa5)) return false;
    if (!(chord.mods&(SP_MOD_WIN|SP_MOD_CTRL|SP_MOD_ALT)) &&
        (chord.physical || chord.vk<0x7c || chord.vk>0x87)) return false;
    *out=chord; return true;
}
bool sp_same_chord(SpChord a, SpChord b) {
    return a.mods==b.mods && a.physical==b.physical && (a.physical?a.scan==b.scan:a.vk==b.vk);
}
size_t sp_match_binding(const SpProfile *p, unsigned vk, unsigned scan, unsigned mods) {
    /* Physical bindings win over a virtual-key alias for the same key. */
    for (unsigned physical=1;;--physical) {
        for(size_t i=0;i<p->binding_count;++i) {
            SpChord c=p->bindings[i].chord;
            if (c.mods==mods && c.physical==!!physical && (c.physical?c.scan==scan:c.vk==vk)) return i;
        }
        if (!physical) break;
    }
    return SIZE_MAX;
}
static SpProfile profile_defaults(const wchar_t *name,const SpConfig *config) {
    SpProfile p={0}; p.name=name; p.layout=sp_default_layout();
    p.global_never_elevate=&config->never_elevate;
    p.global_tint_opacity=&config->tint_opacity;
    p.global_appearance=&config->appearance;
    /* Omitted profile names use Windows Terminal's own configured default. */
    p.quake_window=L"_quake"; p.container_filter=L"label=scratchpad=1"; return p;
}
void sp_config_defaults(SpConfig *c) {
    memset(c,0,sizeof(*c)); c->never_elevate.all=true; c->count=1;
    c->profiles[0]=profile_defaults(L"default",c);
    static const wchar_t *const keys[]={L"Ctrl+Alt+S",L"Ctrl+Alt+Shift+S",L"Ctrl+Alt+P",L"Ctrl+Alt+R",L"Ctrl+Alt+Scan029"};
    static const wchar_t *const actions[]={L"scratch.toggle",L"scratch.member",L"scratch.promote",L"scratch.restore",L"terminal.open"};
    for(size_t i=0;i<sizeof(keys)/sizeof(*keys);++i) {
        SpBinding *b=&c->profiles[0].bindings[i]; sp_chord(keys[i],&b->chord); b->action=actions[i];
    }
    c->profiles[0].binding_count=sizeof(keys)/sizeof(*keys);
}
static bool error_at(wchar_t *error,size_t cap,const wchar_t *source,size_t line,const wchar_t *message) {
    if (error && cap) {
        error[0]=0;
        if (source) swprintf(error,cap,L"%ls:%zu: %ls",source,line,message);
        else swprintf(error,cap,L"line %zu: %ls",line,message);
        error[cap-1]=0;
    }
    return false;
}
/* Tokenize one line in-place. Only quoted \\ and \" escapes are accepted. */
static int lex(wchar_t *p, wchar_t **tokens, size_t max) {
    size_t n=0;
    while (*p) {
        while (*p==L' '||*p==L'\t') ++p;
        if (!*p||*p==L'#') break;
        if (n==max) return -1;
        if (*p==L'"') {
            wchar_t *w=++p; tokens[n++]=w; bool closed=false;
            while (*p) {
                if (*p==L'"') { ++p; closed=true; break; }
                if (*p==L'\\') { ++p; if (*p!=L'\\' && *p!=L'"') return -1; }
                if (*p<32) return -1;
                *w++=*p++;
            }
            if (!closed || (*p && *p!=L' ' && *p!=L'\t' && *p!=L'#')) return -1;
            *w=0;
        } else {
            tokens[n++]=p;
            while (*p && *p!=L' ' && *p!=L'\t' && *p!=L'#') {
                if (*p==L'"'||*p<32) return -1;
                ++p;
            }
            wchar_t ch=*p; if (ch) *p++=0;
            if (ch==L'#') break;
        }
    }
    return (int)n;
}
const SpProfile *sp_config_profile(const SpConfig *c,const wchar_t *name) {
    for (size_t i=0;i<c->count;++i) if (!wcscmp(c->profiles[i].name,name)) return &c->profiles[i];
    return NULL;
}
static unsigned opacity_alpha(unsigned thousandths) {
    return (thousandths*255u+500u)/1000u;
}
unsigned sp_tint_alpha(const SpProfile *p) {
    const SpTintOpacity *tint=p?&p->tint_opacity:NULL;
    if (tint && !tint->set) tint=p->global_tint_opacity;
    if (!tint || !tint->set) return 36u;
    return opacity_alpha(tint->thousandths);
}
/* Locale-independent decimal grammar; never accumulate an unbounded integer. */
static bool opacity_thousandths(const wchar_t *s,unsigned *out) {
    unsigned whole=0,fraction=0,places=0;bool digits=false;
    while (*s>=L'0' && *s<=L'9') {
        whole=whole*10u+(unsigned)(*s++-L'0');
        if (whole>1) return false;
        digits=true;
    }
    if (*s==L'.') {
        ++s;
        while (*s>=L'0' && *s<=L'9') {
            if (places==3) return false;
            fraction=fraction*10u+(unsigned)(*s++-L'0');++places;
        }
        if (!places) return false;
        digits=true;
    }
    if (!digits || *s || (whole==1 && fraction)) return false;
    while (places<3) {fraction*=10u;++places;}
    *out=whole*1000u+fraction;return true;
}
bool sp_appearance_set(SpAppearance *scope,SpAppearanceSetting setting,const wchar_t *token) {
    if (!scope || !token) return false;
    unsigned value;
    SpAppearanceValue *slot;
    switch (setting) {
    case SP_APPEARANCE_TINT_FADE_MS:
        if (!number(token,10,500,&value)) return false;
        slot=&scope->tint_fade_ms;
        break;
    case SP_APPEARANCE_MEMBER_OPACITY:
    case SP_APPEARANCE_INACTIVE_OPACITY:
        if (!opacity_thousandths(token,&value)) return false;
        slot=setting==SP_APPEARANCE_MEMBER_OPACITY?&scope->member_opacity:&scope->inactive_opacity;
        break;
    case SP_APPEARANCE_ACTIVE_BORDER:
        if (wcslen(token)!=6 || !number(token,16,0xffffffu,&value)) return false;
        slot=&scope->active_border_rgb;
        break;
    default:
        return false;
    }
    *slot=(SpAppearanceValue){value,true};
    return true;
}
static SpAppearanceValue resolve_appearance_value(SpAppearanceValue local,SpAppearanceValue global) {
    if (local.set) return local;
    return global.set?global:(SpAppearanceValue){0};
}
SpAppearance sp_appearance_resolve(const SpProfile *p) {
    if (!p) return (SpAppearance){0};
    const SpAppearance *local=&p->appearance;
    const SpAppearance global=p->global_appearance?*p->global_appearance:(SpAppearance){0};
    return (SpAppearance){
        .tint_fade_ms=resolve_appearance_value(local->tint_fade_ms,global.tint_fade_ms),
        .member_opacity=resolve_appearance_value(local->member_opacity,global.member_opacity),
        .inactive_opacity=resolve_appearance_value(local->inactive_opacity,global.inactive_opacity),
        .active_border_rgb=resolve_appearance_value(local->active_border_rgb,global.active_border_rgb)
    };
}
SpAppearanceValue sp_member_alpha(const SpProfile *p,bool active) {
    SpAppearance a=sp_appearance_resolve(p);
    SpAppearanceValue opacity=!active && a.inactive_opacity.set?a.inactive_opacity:a.member_opacity;
    if (!opacity.set) return (SpAppearanceValue){0};
    unsigned alpha=opacity_alpha(opacity.value);
    return (SpAppearanceValue){alpha?alpha:1u,true};
}
bool sp_never_elevate_all(const SpProfile *p) {
    return p && (p->never_elevate.all || (p->global_never_elevate && p->global_never_elevate->all));
}
size_t sp_never_elevate_count(const SpProfile *p) {
    return p?p->never_elevate.count+(p->global_never_elevate?p->global_never_elevate->count:0):0;
}
const wchar_t *sp_never_elevate_entry(const SpProfile *p,size_t index) {
    if (!p) return NULL;
    size_t global=p->global_never_elevate?p->global_never_elevate->count:0;
    if (index<global) return p->global_never_elevate->entries[index];
    index-=global;
    return index<p->never_elevate.count?p->never_elevate.entries[index]:NULL;
}
static bool path_separator(wchar_t c) { return c==L'/' || c==L'\\'; }
/* Syntactic targets only: no filesystem access, expansion or canonicalization. */
static bool never_elevate_target(const wchar_t *s) {
    if (!*s || wcspbrk(s,L"\"<>|?*")) return false;
    bool nonspace=false;
    for (const wchar_t *q=s;*q;++q) {
        if (*q<32 || *q==127) return false;
        if (*q!=L' ') nonspace=true;
    }
    if (!nonspace) return false;
    const wchar_t *tail=s;
    if (wcspbrk(s,L"\\/:")) {
        size_t length=wcslen(s);
        bool drive=(*s>=L'A'&&*s<=L'Z')||(*s>=L'a'&&*s<=L'z');
        if (length>=4 && drive && s[1]==L':' && path_separator(s[2])) tail=s+3;
        else if (length>2 && path_separator(s[0]) && s[1]==s[0] && !wcschr(s,L':')) {
            tail=s+2;
            /* UNC requires a nonempty server, share and final file component. */
            for (unsigned i=0;i<2;++i) {
                const wchar_t *start=tail;
                while (*tail && !path_separator(*tail)) ++tail;
                if (tail==start || !*tail) return false;
                ++tail;
            }
        } else return false;
        if (wcschr(tail,L':')) return false;
    }
    const wchar_t *base=tail;
    for (const wchar_t *q=tail;*q;++q) if (path_separator(*q)) base=q+1;
    return *base && wcscmp(base,L".") && wcscmp(base,L"..");
}
typedef struct {
    SpConfig *config;
    SpConfigLoader loader;
    void *loader_context;
    wchar_t *names[SP_CONFIG_MAX_FILES];
    size_t parents[SP_CONFIG_MAX_FILES],stack[SP_CONFIG_MAX_DEPTH+1];
    size_t files,chars,used;
} ParseState;

static bool same_source(const wchar_t *a,const wchar_t *b) {
#ifdef _WIN32
    int result=CompareStringOrdinal(a,-1,b,-1,TRUE);
    return result==0 || result==CSTR_EQUAL; /* Fail closed if comparison fails. */
#else
    while (*a && *b && towlower(*a)==towlower(*b)) { ++a; ++b; }
    return *a==*b;
#endif
}
static void append_error(wchar_t *error,size_t cap,const wchar_t *text) {
    if (!error || !cap) return;
    size_t used=wcslen(error);
    while (*text && used+1<cap) error[used++]=*text++;
    error[used]=0;
}
static bool import_error(const ParseState *state,size_t file,size_t depth,size_t line,
        const wchar_t *message,const wchar_t *target,size_t previous,wchar_t *error,size_t cap) {
    error_at(error,cap,state->names[file],line,message);
    append_error(error,cap,L"; import chain: ");
    for (size_t i=0;i<=depth;++i) {
        if (i) append_error(error,cap,L" -> ");
        append_error(error,cap,state->names[state->stack[i]]);
    }
    append_error(error,cap,L" -> ");append_error(error,cap,target);
    if (previous!=SIZE_MAX) {
        size_t chain[SP_CONFIG_MAX_DEPTH+1],count=0;
        for (size_t i=previous;i!=SIZE_MAX && count<SP_CONFIG_MAX_DEPTH+1;i=state->parents[i]) chain[count++]=i;
        append_error(error,cap,L"; first loaded via: ");
        while (count) {
            append_error(error,cap,state->names[chain[--count]]);
            if (count) append_error(error,cap,L" -> ");
        }
    }
    return false;
}
static wchar_t *store_source(ParseState *state,const wchar_t *text,const wchar_t *source,
        wchar_t *error,size_t cap) {
    size_t length=0;
    while (length<=SP_CONFIG_CHARS && text[length]) ++length;
    if (length>SP_CONFIG_CHARS-state->chars) {
        error_at(error,cap,source,1,L"aggregate configuration exceeds 131072 UTF-16 code units");
        return NULL;
    }
    wchar_t *stored=state->config->storage+state->used;
    memcpy(stored,text,(length+1)*sizeof(*text));
    state->chars+=length;state->used+=length+1;
    return stored;
}
static bool parse_file(ParseState *state,wchar_t *cursor,size_t file,size_t depth,wchar_t *error,size_t cap);
static bool parse_import(ParseState *state,const wchar_t *path,size_t file,size_t depth,size_t line,
        wchar_t *error,size_t cap) {
    if (depth==SP_CONFIG_MAX_DEPTH)
        return import_error(state,file,depth,line,L"maximum import nesting depth is 8",path,SIZE_MAX,error,cap);
    if (state->files==SP_CONFIG_MAX_FILES)
        return import_error(state,file,depth,line,L"maximum configuration file count is 32 (including entry)",path,SIZE_MAX,error,cap);
    SpConfigSource loaded={0};wchar_t detail[512]={0};
    bool ok=state->loader(state->loader_context,state->names[file],path,&loaded,detail,512);
    detail[511]=0;
    if (!ok || !loaded.text || !loaded.name || !*loaded.name || wcslen(loaded.name)>SP_CONFIG_PATH_CHARS) {
        free(loaded.text);free(loaded.name);
        return error_at(error,cap,state->names[file],line,*detail?detail:L"configuration loader failed to return a valid source");
    }
    for (size_t i=0;i<state->files;++i) if (same_source(state->names[i],loaded.name)) {
        import_error(state,file,depth,line,L"duplicate import (cycle or file already loaded)",loaded.name,i,error,cap);
        free(loaded.text);free(loaded.name);return false;
    }
    wchar_t *stored=store_source(state,loaded.text,loaded.name,error,cap);
    free(loaded.text);
    if (!stored) { free(loaded.name);return false; }
    size_t imported=state->files++;
    state->names[imported]=loaded.name;state->parents[imported]=file;
    return parse_file(state,stored,imported,depth+1,error,cap);
}
static bool parse_file(ParseState *state,wchar_t *cursor,size_t file,size_t depth,wchar_t *error,size_t cap) {
    SpConfig *c=state->config;const wchar_t *source=state->names[file];
    state->stack[depth]=file;
    SpProfile *p=NULL; SpNeverElevate *block=NULL; size_t line=0,block_line=0,profile_line=0;
    for (;*cursor;) {
        ++line; wchar_t *next=wcspbrk(cursor,L"\r\n");
        if (next) { wchar_t ch=*next; *next++=0; if(ch==L'\r'&&*next==L'\n') ++next; }
        wchar_t *t[SP_MAX_ARGS+3]; int n=lex(cursor,t,SP_MAX_ARGS+3);
        if (n<0) return error_at(error,cap,source,line,L"invalid quoting/escape or too many arguments");
        if (n) {
            if (!wcscmp(t[0],L"import")) {
                if (p || block) return error_at(error,cap,source,line,L"import is allowed only at top level, outside profiles and never-elevate blocks");
                if (n!=2 || !*t[1]) return error_at(error,cap,source,line,L"import requires exactly one nonempty path argument");
                if (!state->loader) return error_at(error,cap,source,line,L"import requires a file-backed configuration");
                if (!parse_import(state,t[1],file,depth,line,error,cap)) return false;
            } else if (block) {
                if (!wcscmp(t[0],L"end-never-elevate")) {
                    if (n!=1) return error_at(error,cap,source,line,L"end-never-elevate takes no arguments");
                    block=NULL;
                } else if (!wcscmp(t[0],L"never-elevate") || !wcscmp(t[0],L"end") || !wcscmp(t[0],L"profile")) {
                    return error_at(error,cap,source,line,L"expected end-never-elevate before another block/profile delimiter");
                } else {
                    if (n!=1 || !never_elevate_target(t[0]))
                        return error_at(error,cap,source,line,L"never-elevate needs one program name or absolute script path per line (or all)");
                    if (!wcscmp(t[0],L"all")) block->all=true;
                    else {
                        if (block->count==SP_MAX_NEVER_ELEVATE_ENTRIES)
                            return error_at(error,cap,source,line,L"too many never-elevate entries (maximum 128 per scope)");
                        block->entries[block->count++]=t[0];
                    }
                }
            } else if (!wcscmp(t[0],L"never-elevate")) {
                if (n!=1) return error_at(error,cap,source,line,L"never-elevate takes no arguments; put entries on following lines");
                block=p?&p->never_elevate:&c->never_elevate; block_line=line;
            } else if (!wcscmp(t[0],L"end-never-elevate")) {
                return error_at(error,cap,source,line,L"end-never-elevate requires an open never-elevate block");
            } else if (!wcscmp(t[0],L"tint-opacity")) {
                unsigned value=0;
                if (n!=2 || !opacity_thousandths(t[1],&value))
                    return error_at(error,cap,source,line,L"tint-opacity must be a decimal from 0.0 to 1.0 (up to 3 decimal places)");
                SpTintOpacity *tint=p?&p->tint_opacity:&c->tint_opacity;
                *tint=(SpTintOpacity){value,true};
            } else if (!wcscmp(t[0],L"tint-fade-ms")) {
                SpAppearance *appearance=p?&p->appearance:&c->appearance;
                if (n!=2 || !sp_appearance_set(appearance,SP_APPEARANCE_TINT_FADE_MS,t[1]))
                    return error_at(error,cap,source,line,L"tint-fade-ms requires one ASCII decimal integer from 0 to 500");
            } else if (!wcscmp(t[0],L"active-border")) {
                SpAppearance *appearance=p?&p->appearance:&c->appearance;
                if (n!=2 || !sp_appearance_set(appearance,SP_APPEARANCE_ACTIVE_BORDER,t[1]))
                    return error_at(error,cap,source,line,L"active-border requires exactly six ASCII hexadecimal digits (RRGGBB, no #)");
            } else if (!wcscmp(t[0],L"profile")) {
                if (n!=2||p||!*t[1]||wcslen(t[1])>63||c->count==SP_MAX_PROFILES||sp_config_profile(c,t[1]))
                    return error_at(error,cap,source,line,L"invalid/duplicate/nested profile (maximum 8)");
                p=&c->profiles[c->count++]; *p=profile_defaults(t[1],c);profile_line=line;
            } else if (!wcscmp(t[0],L"end")) {
                if (n!=1||!p||!p->binding_count) return error_at(error,cap,source,line,L"end requires an open profile with at least one binding");
                p=NULL;
            } else if (!p) return error_at(error,cap,source,line,L"directive must be inside a profile");
            else if (!wcscmp(t[0],L"layout")) {
                if (n<3) return error_at(error,cap,source,line,L"incomplete layout directive");
                unsigned a=0,b=0;
                if (!wcscmp(t[1],L"gap") && n==3 && number(t[2],10,1000,&a)) p->layout.gap=(int)a;
                else if ((!wcscmp(t[1],L"single")||!wcscmp(t[1],L"multi")) && n==4 &&
                    number(t[2],10,100,&a) && a && number(t[3],10,100,&b) && b) {
                    if (!wcscmp(t[1],L"single")) {p->layout.single_w=(int)a;p->layout.single_h=(int)b;}
                    else {p->layout.multi_w=(int)a;p->layout.multi_h=(int)b;}
                } else if (!wcscmp(t[1],L"split") && n==3) {
                    if (!wcscmp(t[2],L"longest")) p->layout.split=SP_SPLIT_LONGEST;
                    else if (!wcscmp(t[2],L"horizontal")) p->layout.split=SP_SPLIT_HORIZONTAL;
                    else if (!wcscmp(t[2],L"vertical")) p->layout.split=SP_SPLIT_VERTICAL;
                    else if (!wcscmp(t[2],L"alternate")) p->layout.split=SP_SPLIT_ALTERNATE;
                    else return error_at(error,cap,source,line,L"split must be longest/horizontal/vertical/alternate");
                } else if (!wcscmp(t[1],L"anchor") && n==3) {
                    if (!wcscmp(t[2],L"top-left")) {p->layout.mirror_x=false;p->layout.mirror_y=false;}
                    else if (!wcscmp(t[2],L"top-right")) {p->layout.mirror_x=true;p->layout.mirror_y=false;}
                    else if (!wcscmp(t[2],L"bottom-left")) {p->layout.mirror_x=false;p->layout.mirror_y=true;}
                    else if (!wcscmp(t[2],L"bottom-right")) {p->layout.mirror_x=true;p->layout.mirror_y=true;}
                    else return error_at(error,cap,source,line,L"invalid layout anchor");
                } else return error_at(error,cap,source,line,L"invalid layout directive/value");
            } else if (!wcscmp(t[0],L"quake-window") || !wcscmp(t[0],L"terminal-profile") ||
                       !wcscmp(t[0],L"container-profile") || !wcscmp(t[0],L"container-filter") ||
                       !wcscmp(t[0],L"container-directory")) {
                if (n!=2||!*t[1]||wcslen(t[1])>1024) return error_at(error,cap,source,line,L"setting needs one nonempty value (up to 1024 characters)");
                if (!wcscmp(t[0],L"quake-window")) p->quake_window=t[1];
                else if (!wcscmp(t[0],L"terminal-profile")) p->terminal_profile=t[1];
                else if (!wcscmp(t[0],L"container-profile")) p->container_profile=t[1];
                else if (!wcscmp(t[0],L"container-filter")) p->container_filter=t[1];
                else p->container_directory=t[1];
            } else if (!wcscmp(t[0],L"bind")) {
                if (n<3 || p->binding_count==SP_MAX_BINDINGS || !*t[2]) return error_at(error,cap,source,line,L"invalid binding (maximum 64/profile)");
                SpBinding *binding=&p->bindings[p->binding_count];
                if (!sp_chord(t[1],&binding->chord)) return error_at(error,cap,source,line,L"invalid chord; use exact modifier/key names and a nonmodifier primary key");
                for(size_t i=0;i<p->binding_count;++i) if(sp_same_chord(binding->chord,p->bindings[i].chord))
                    return error_at(error,cap,source,line,L"duplicate chord");
                binding->action=t[2]; binding->argc=(size_t)n-3;
                for (size_t i=0;i<binding->argc;++i) binding->args[i]=t[i+3];
                ++p->binding_count;
            } else return error_at(error,cap,source,line,L"unknown directive");
        }
        if (!next) break;
        cursor=next;
    }
    if (block) return error_at(error,cap,source,block_line,L"unterminated never-elevate block; expected end-never-elevate");
    if (p) return error_at(error,cap,source,profile_line,L"missing end for profile; each file must be self-contained");
    return true;
}
bool sp_config_parse_ex(SpConfig *c,const wchar_t *text,const wchar_t *source_name,
        SpConfigLoader loader,void *loader_context,wchar_t *error,size_t cap) {
    if (error && cap) error[0]=0;
    if (!c || !text) return error_at(error,cap,source_name,0,L"missing configuration");
    if ((loader && (!source_name || !*source_name)) ||
            (source_name && wcslen(source_name)>SP_CONFIG_PATH_CHARS))
        return error_at(error,cap,NULL,0,L"file-backed configuration requires a valid canonical source name");
    memset(c,0,sizeof(*c));
    ParseState state={0};state.config=c;state.loader=loader;state.loader_context=loader_context;
    state.files=1;state.parents[0]=SIZE_MAX;
    if (source_name) {
        size_t length=wcslen(source_name)+1;
        state.names[0]=malloc(length*sizeof(*source_name));
        if (!state.names[0]) return error_at(error,cap,source_name,0,L"out of memory storing configuration source");
        memcpy(state.names[0],source_name,length*sizeof(*source_name));
    }
    wchar_t *stored=store_source(&state,text,source_name,error,cap);
    bool ok=stored && parse_file(&state,stored,0,0,error,cap);
    if (ok && !c->count) ok=error_at(error,cap,source_name,1,L"no profiles in aggregate configuration");
    for (size_t i=0;i<state.files;++i) free(state.names[i]);
    if (ok && error && cap) error[0]=0;
    return ok;
}
bool sp_config_parse(SpConfig *c,const wchar_t *text,wchar_t *error,size_t cap) {
    return sp_config_parse_ex(c,text,NULL,NULL,NULL,error,cap);
}
