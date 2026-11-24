#ifndef SP_CONFIG_H
#define SP_CONFIG_H
#include "scratchpad.h"
#include <wchar.h>
#define SP_MAX_PROFILES 8u
#define SP_MAX_BINDINGS 64u
#define SP_MAX_ARGS 12u
#define SP_MAX_NEVER_ELEVATE_ENTRIES 128u
#define SP_CONFIG_CHARS 131072u /* Aggregate decoded UTF-16 units, excluding NULs. */
#define SP_CONFIG_FILE_BYTES 65536u
#define SP_CONFIG_MAX_DEPTH 8u /* Import edges; the entry file is depth zero. */
#define SP_CONFIG_MAX_FILES 32u /* Includes the entry file. */
#define SP_CONFIG_PATH_CHARS 32767u
#define SP_MOD_WIN 1u
#define SP_MOD_CTRL 2u
#define SP_MOD_SHIFT 4u
#define SP_MOD_ALT 8u

typedef struct { unsigned mods, vk, scan; bool physical; } SpChord;
typedef struct {
    SpChord chord;
    const wchar_t *action;
    const wchar_t *args[SP_MAX_ARGS];
    size_t argc;
} SpBinding;
typedef struct {
    const wchar_t *entries[SP_MAX_NEVER_ELEVATE_ENTRIES];
    size_t count;
    bool all;
} SpNeverElevate;
typedef struct {
    unsigned thousandths; /* Exact normalized decimal, 0..1000 when set. */
    bool set;
} SpTintOpacity;
typedef struct { unsigned value; bool set; } SpAppearanceValue;
typedef struct {
    SpAppearanceValue tint_fade_ms; /* 0..500 ms */
    SpAppearanceValue member_opacity, inactive_opacity; /* 0..1000 thousandths */
    SpAppearanceValue active_border_rgb; /* Packed 0xRRGGBB, not Win32 COLORREF. */
} SpAppearance;
typedef enum {
    SP_APPEARANCE_TINT_FADE_MS,
    SP_APPEARANCE_MEMBER_OPACITY,
    SP_APPEARANCE_INACTIVE_OPACITY,
    SP_APPEARANCE_ACTIVE_BORDER
} SpAppearanceSetting;
typedef struct {
    const wchar_t *name, *quake_window, *terminal_profile, *container_profile, *container_filter;
    /* NULL terminal_profile: Terminal default; NULL container_profile: inherit it. */
    const wchar_t *container_directory; /* NULL: keep the container's configured workdir. */
    SpLayout layout;
    SpBinding bindings[SP_MAX_BINDINGS];
    size_t binding_count;
    SpNeverElevate never_elevate; /* Profile-local entries, without global copies. */
    const SpNeverElevate *global_never_elevate;
    SpTintOpacity tint_opacity;
    const SpTintOpacity *global_tint_opacity;
    SpAppearance appearance;
    const SpAppearance *global_appearance;
} SpProfile;
typedef struct {
    wchar_t storage[SP_CONFIG_CHARS+SP_CONFIG_MAX_FILES];
    SpProfile profiles[SP_MAX_PROFILES];
    size_t count;
    SpNeverElevate never_elevate; /* Shared by every profile, regardless of file order. */
    SpTintOpacity tint_opacity; /* Shared scalar; profile-local values override it. */
    SpAppearance appearance; /* Shared scalars; resolved against the final global values. */
} SpConfig;

bool sp_chord(const wchar_t *text, SpChord *out);
bool sp_same_chord(SpChord a, SpChord b);
size_t sp_match_binding(const SpProfile *p, unsigned vk, unsigned scan, unsigned mods);
/* Configuration contains self-references, including defaults. Never copy/move
   it; retain it until shutdown. Parsed strings refer into this cfg's storage. */
bool sp_config_parse(SpConfig *cfg, const wchar_t *text, wchar_t *error, size_t cap);
/* A successful loader returns two malloc-owned, NUL-terminated strings. name
   must be the final canonical absolute path; resolve path against from's
   directory, not CWD. The parser takes ownership and frees both strings.
   On failure, leave the source zeroed and write a diagnostic if possible.
   No filesystem calls are made by the parser. Entry source_name must likewise
   be canonical when a loader is supplied; it is borrowed only for this call. */
typedef struct { wchar_t *text, *name; } SpConfigSource;
typedef bool (*SpConfigLoader)(void *context, const wchar_t *from, const wchar_t *path,
    SpConfigSource *source, wchar_t *error, size_t capacity);
bool sp_config_parse_ex(SpConfig *cfg, const wchar_t *text, const wchar_t *source_name,
    SpConfigLoader loader, void *loader_context, wchar_t *error, size_t capacity);
void sp_config_defaults(SpConfig *cfg);
const SpProfile *sp_config_profile(const SpConfig *cfg, const wchar_t *name);
/* Profile-local > global > original alpha 36. Round thousandths*255/1000
   half-up; NULL/standalone unset profiles also use 36. Returns 0..255;
   the native overlay applies a minimum of 1 to preserve hit testing. */
unsigned sp_tint_alpha(const SpProfile *profile);
/* Internal appearance contract: not DSL directives until the effects ship.
   Validate one decoded token and replace the setting in this scope on success.
   Invalid tokens leave the scope unchanged. */
bool sp_appearance_set(SpAppearance *scope, SpAppearanceSetting setting, const wchar_t *token);
/* Resolve each scalar local > global. Omitted fields are unset with value 0
   (zero duration for fade). A NULL profile has no overrides. */
SpAppearance sp_appearance_resolve(const SpProfile *profile);
/* Use effective inactive opacity (inactive members only), else effective base.
   Unset means leave native alpha alone; a set value is native alpha 1..255. */
SpAppearanceValue sp_member_alpha(const SpProfile *profile, bool active);
/* Additive policy view: global entries first, then local; no deduplication or
   path/name rewriting. `all` is separate from the ordinary entry count.
   entry() returns NULL out of range; a standalone profile may have no globals. */
bool sp_never_elevate_all(const SpProfile *profile);
size_t sp_never_elevate_count(const SpProfile *profile);
const wchar_t *sp_never_elevate_entry(const SpProfile *profile, size_t index);
#endif
