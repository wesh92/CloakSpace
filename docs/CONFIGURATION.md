# Configuration reference

`scratchpad.conf` is **Scratchpad DSL v1**: a line-oriented, case-sensitive language. It is deliberately not a TOML subset, YAML, JSON, a shell script, or an expression evaluator. Files are UTF-8 (optional UTF-8 BOM), at most **65536 bytes per file**, with an **aggregate 131072 decoded UTF-16 code-unit limit** across the entry and all imports. Embedded NUL, invalid UTF-8, malformed quotes and unknown directives fail closed with a diagnostic. Parsed configuration is bounded and immutable until restart.

## A complete profile

```scratchpad
never-elevate
    all
end-never-elevate

tint-opacity 0.14

profile default
    layout gap 16
    layout split alternate
    layout anchor top-right
    layout single 80 75
    layout multi 95 90
    quake-window "_quake"

    bind Ctrl+Alt+S scratch.toggle
    bind Ctrl+Alt+Shift+S scratch.member
    bind Ctrl+Alt+P scratch.promote
    bind Ctrl+Alt+R scratch.restore
    bind Ctrl+Alt+Scan029 terminal.open
    bind Ctrl+Alt+U terminal.quake "Ubuntu" "wsl.exe" "-d" "Ubuntu"
    bind Ctrl+Alt+N process.exec "notepad.exe"
end
```

Save that as a `.conf` file and validate with `cloakspace.exe --config PATH --check-config` before running it. `terminal.open` uses your Windows Terminal default profile; it does not require a specially named profile. The Ubuntu binding is an optional example; remove it or replace its profile/distribution names with ones you have installed. Configuration validation checks grammar, action names/counts, required nonempty arguments and the Terminal-safe value subset, not installation of external programs/profiles.

The no-file built-in key map and the shipped `default` profile use **Ctrl+Alt+S** to show/hide, **Ctrl+Alt+Shift+S** to take/release a member, **Ctrl+Alt+P** to promote, **Ctrl+Alt+R** to restore all, and **Ctrl+Alt+Scan029** to open Terminal. They leave Windows Search/Snipping shortcuts alone. Choose different chords if another tool or your keyboard's AltGr handling conflicts. The sample's `portable` profile adds window/process shortcuts and an alternative layout; select it with `--profile portable`. No container action is bound by default.

## Grammar

- `#` begins a comment outside double quotes. Blank lines are ignored.
- Tokens are separated by spaces/tabs. A quoted token may contain spaces or `#`. Required executable/profile names cannot be empty. Later `process.exec` arguments may intentionally be empty; Terminal child arguments may not.
- Inside double quotes, the only escapes are `\\` for a backslash and `\"` for a quote. Prefer Windows forward-slash paths such as `"C:/Tools/my module.exe"`; alternatively double every backslash. There is no variable interpolation, tilde expansion, command substitution, environment expansion or shell expansion.
- `profile NAME` starts a named profile; `end` finishes it. Nesting and duplicate names are errors. Names contain at most 63 UTF-16 code units. At most **8 profiles**, **64 bindings per profile**, and **12 action arguments per binding** are accepted.
- Each explicit profile begins with default layout/Terminal settings but an **empty key map**. Supply at least one binding before `end`. There is no implicit profile inheritance. `--profile NAME` chooses one; the default selection is `default`. All profiles are validated, including inactive ones.
- Settings may appear in any order within a profile. A repeated scalar setting uses its final value. Duplicate **identical** chords are errors, even if modifier order differs.
- `never-elevate` / `end-never-elevate` is a list block, allowed at top level or inside a profile. Its rules add together; unlike scalar settings, a later block never replaces an earlier one.
- `tint-opacity DECIMAL` is a scalar, allowed at top level or inside a profile (not in a `never-elevate` block). A profile-local value overrides the global one; the final value in each scope wins.
- `import PATH` includes another file at that point, at **top level only**, with exactly one nonempty quoted/unquoted path token. See the resolution, ordering and bounds below.

### Imports and multiple files

Use an entry file to collect globals and complete profiles kept in separate files. The tested [examples/imports/main.conf](../examples/imports/main.conf) is this exact entry point:

```scratchpad
# Entry point: every relative import is based on the file containing it.
import "globals.conf"
import "profiles/default.conf"
import "profiles/work.conf"

# Last global scalar wins, even for profiles already imported above.
# default inherits 0.14; work keeps its local 0.5. Restart after edits.
tint-opacity 0.14
```

Keep its sibling files together: [globals.conf](../examples/imports/globals.conf) itself imports [policy/never-elevate.conf](../examples/imports/policy/never-elevate.conf), while [profiles/default.conf](../examples/imports/profiles/default.conf) and [profiles/work.conf](../examples/imports/profiles/work.conf) each contain a complete profile. Validate with `cloakspace.exe --config examples/imports/main.conf --check-config`, or add `--profile work`. This example keeps global `never-elevate all`; neither profile can weaken it.

**Paths and sources**

- `import "profiles/work.conf"` resolves against the **directory of the file containing that directive**, not the process working directory or the original entry file's directory. Nested imports follow the same rule. `.` and `..` path components are supported; imports are not confined to the entry directory.
- Absolute drive paths (`C:/Config/work.conf`) and UNC paths (`//server/share/Config/work.conf`) are allowed. A single leading separator is rooted at the containing file's drive/UNC share, never the current process's drive. Relative imports from an aliased file use its final canonical directory.
- The normal lexer rules apply: quote spaces and `#`, prefer forward slashes, and double backslashes inside quotes. `import` with no argument, an empty path, or two arguments is an error.
- Device/namespace paths (`\\?\`, `\\.\`, including slash variants), drive-relative paths (`C:work.conf` or `C:`), globs/wildcards (`*`, `?`), control characters and alternate data streams are rejected. The target must be an **existing regular file**; missing files, directories, devices and named pipes are errors, not optional imports. Long paths must fit Win32's 32767-code-unit bound including the loader's internal long-path prefix.
- There is **no environment-variable expansion**: `%APPDATA%` in an import is a literal directory name. No tilde expansion, shell commands, URL fetching or remote download is implemented. Opening a UNC file is the only implied network access. Imported files are as trusted as the entry file; this is not an untrusted-config sandbox.

**Ordering and policy**

An import is processed immediately, in order, before processing resumes on the next line of the containing file. Imported files can import more files. Each file must be **self-contained**: `profile`/`end` and `never-elevate`/`end-never-elevate` must open and close in that same file. You cannot splice part of a profile/block into another file, and `import` is an error inside either scope. A globals-only, empty or BOM-only import is valid; the **whole aggregate** must contain at least one complete profile. The entry itself can consist solely of imports and globals.

All existing scope rules operate across the aggregate. Profile names must be unique across files and there are at most **8 profiles total**. Global `tint-opacity` uses its last value in processing order: a directive **after** an import overrides imported globals, even for profiles parsed earlier. A profile's local tint still wins over any global. There is no entry-file override of an imported profile's local settings; edit that profile's file. Bindings, profile-local policy and Terminal/layout settings remain owned by that complete profile.

`never-elevate` is **additive across files**, not last-wins. Imported blocks may add restrictions, never remove existing rules or `all`. Global `all` anywhere denies recognized elevation requests for every profile, even if later files contain empty global/local blocks. Profile-local `all` remains local to that profile. There is no reset, allow override or import-based policy reduction.

**Identity, limits and errors**

- Every real file, including the entry, is identified from its open handle using its **final canonical path**, compared ordinally case-insensitively. Re-importing any already loaded path is an error: cycles, repeated imports and diamond-shaped dependencies are all rejected, with the current import chain and the first-load chain. This is canonical-path identity, not a hard-link/file-ID policy.
- Maximum **8 nested import levels** (entry at depth 0, imported files at depths 1..8) and **32 files total including the entry**. Depth/count limits are checked before calling the loader for an additional file.
- Each file is at most **65536 UTF-8 bytes**, including an optional BOM. The **131072 aggregate decoded UTF-16 units** include comments/whitespace but exclude stripped BOMs and per-file NUL terminators. Exceeding either bound is an error, never silent truncation. Existing per-profile binding/argument and additive-policy entry limits still apply.
- Diagnostics identify the source and line as `<path>:<line>: <message>`. An unterminated imported profile/block names that file, not the entry. Import open/decode failures retain the importing location and, when known, the target source. Depth/duplicate errors include the import chain. Very long diagnostics are safely truncated to the caller's error buffer.
- The in-memory C parser stays free of filesystem I/O. `sp_config_parse_ex` accepts a source name and loader callback; `sp_config_parse` without a loader rejects imports with `import requires a file-backed configuration`. Parsed strings live in their owning `SpConfig`; keep that self-referential configuration in static/heap storage and **never copy or move it**.

Files are read only at startup. **Restart after changing the entry or any imported file**; no hot reload, file watcher or runtime mutation is added.

### Never-elevate launch policy

The supplied `scratchpad.conf` and the no-file built-in defaults contain a **global `all` policy**. It blocks elevation, not execution: a protected program or script may still run normally from a non-elevated scratchpad. No policy performs elevation, de-elevation, an automatic UAC retry, or an implicit interpreter launch. Windows error 740 (`ERROR_ELEVATION_REQUIRED`) is reported without an elevation retry.

**Syntax and entries**

- Open with `never-elevate` alone, put **one entry per line**, and close with `end-never-elevate` alone. Inside a profile, its separate `end` must follow the block's closing delimiter. `end`, `end never-elevate`, nested blocks and missing delimiters are errors.
- An entry is one nonempty decoded token: a program name such as `pwsh.exe` or `pwsh`, or a full absolute Windows drive/UNC file path such as `C:/Scripts/task.ps1` or `//server/share/task.py`. Relative file paths, drive-relative entries (`C:task.ps1`), folders ending in a separator, command lines, globs and regular expressions are not supported policy entries. An extensionless program name is allowed; script-path extensions remain significant.
- Use the ordinary token/quote/comment rules above. Quote spaces and `#`; prefer `"C:/Shared Ops/task.ps1"` to doubled quoted backslashes. Names/paths need not exist when the file is parsed. There is no environment-variable or shell expansion.
- Exact lowercase `all`, including `"all"`, is reserved: reject every elevation request the guard recognizes, regardless of target. `ALL` and `all.exe` are ordinary program-name entries, not the reserved flag.
- Each scope accepts up to **128 ordinary entries**, accumulated across all its blocks. `all` is a separate flag and does not use an entry slot. Blank/comment-only lines and empty blocks are valid. Duplicates are retained and count toward the limit.

**Global and profile scope**

A block outside every profile applies to **every profile in that configuration**, even profiles declared before it. A block inside a profile adds restrictions only to that profile. The effective policy is the union of global and local entries, with `all` active if either scope contains it. Repeated blocks append in their own scope; an empty local block cannot remove global rules or global `all`. There is no allow-list override, reset directive or profile-to-profile inheritance.

An existing explicit file without any block keeps an empty elevation policy for compatibility; built-in `all` is **not merged into files**. To adopt the default, add the global block shown in the complete example above. To choose selective rules instead, deliberately replace global `all` in that trusted file; adding local entries will not narrow it. Restart after edits.

The selective example below is also [examples/never-elevate.conf](../examples/never-elevate.conf), used by the file-loader integration tests. Its program names and absolute paths are **fictional placeholders**: replace them with your own installed tools/scripts before use. Unlike the deny-all default, it permits elevation of unlisted targets. The broker bindings illustrate policy checks, not a recommendation to run them; `--check-config` validates without executing any binding, and automated tests intercept their process boundaries.

```scratchpad
# Selective example: unlisted targets may elevate. Not the deny-all default.
never-elevate
    global-tool.exe
    "C:/Shared Ops/maintenance-雪.ps1"
end-never-elevate

profile default
    never-elevate
        local-tool
    end-never-elevate
    never-elevate
        C:/Work/locked.py
    end-never-elevate
    bind Ctrl+Alt+G process.exec gsudo.exe global-tool.exe
    bind Ctrl+Alt+L process.exec gsudo.exe local-tool.exe
    bind Ctrl+Alt+S process.exec gsudo.exe pwsh.exe -File "C:/Shared Ops/maintenance-雪.ps1"
    bind Ctrl+Alt+P process.exec gsudo.exe python.exe C:/Work/locked.py
    bind Ctrl+Alt+T process.exec gsudo.exe late-tool.exe
    bind Ctrl+Alt+N process.exec global-tool.exe
    bind Ctrl+Alt+O process.exec local-tool.exe
    bind Ctrl+Alt+F process.exec pwsh.exe -File "C:/Shared Ops/maintenance-雪.ps1"
    bind Ctrl+Alt+Y process.exec python.exe C:/Work/locked.py
end

profile other
    never-elevate
        # Empty local policy does not clear the globals.
    end-never-elevate
    bind Ctrl+Alt+G process.exec gsudo.exe global-tool.exe
    bind Ctrl+Alt+L process.exec gsudo.exe local-tool.exe
    bind Ctrl+Alt+S process.exec gsudo.exe pwsh.exe -File "C:/Shared Ops/maintenance-雪.ps1"
    bind Ctrl+Alt+P process.exec gsudo.exe python.exe C:/Work/locked.py
    bind Ctrl+Alt+T process.exec gsudo.exe late-tool.exe
    bind Ctrl+Alt+N process.exec global-tool.exe
    bind Ctrl+Alt+O process.exec local-tool.exe
    bind Ctrl+Alt+F process.exec pwsh.exe -File "C:/Shared Ops/maintenance-雪.ps1"
    bind Ctrl+Alt+Y process.exec python.exe C:/Work/locked.py
end

# Late global blocks still apply to both profiles.
never-elevate
    late-tool.exe
end-never-elevate
```

`global-tool.exe`, the shared PowerShell script and `late-tool.exe` cannot be elevated through recognized requests in either profile. `local-tool.exe` and `C:/Work/locked.py` are additionally protected in `default`, but not `other`. The ordinary N/O/F/Y bindings remain allowed in both profiles when scratchpad is non-elevated; running scratchpad elevated instead subjects those launches to the same name/path rules.

**Matching, refusals and the security boundary**

The worker checks decoded executable/argument arrays before subprocess capture or launch, and checks resolved executable paths again before use. Program entries compare executable basenames with ordinal case-insensitive matching and optional `.exe`; a name is not a substring match on arbitrary argument text. Full paths compare complete normalized paths in the executable/target arguments, not just script basenames. Comparison is lexical Windows normalization (slash/case, dot segments and ordinary trailing dots/spaces), not file identity or filesystem lookup. Where the inherited working directory is known, relative launch operands can be compared to absolute policy paths; a broker/Terminal child with an unknown directory may be refused rather than guessed. Policy **entries** themselves must still be absolute when they contain a path.

An active policy checks the process primary token, including high integrity even if its elevation flag is clear. It also recognizes bounded forms of direct `runas`/`sudo`/`gsudo`, literal flat PowerShell `Start-Process -Verb RunAs` commands, and supported Windows Terminal elevated-child requests. A matching elevated launch is refused before spawn/capture; raw matches are refused before executable lookup as well. Unknown token state and uninspectable targets/options of recognized elevation requests fail closed. Unsupported/ambiguous broker or Terminal syntax can therefore be refused even when the intended target is unrelated. The error identifies `never-elevate`, the active profile and the matched rule when known, and instructs you to remove the elevation request and run from a non-elevated scratchpad.

**This is a bounded launch policy, not a sandbox, an RCE defense or Windows access control.** Configuration remains trusted local code-equivalent input. The guard does not execute or fully parse arbitrary interpreter bodies, inspect script contents, resolve junctions/short names/hard links or other file aliases, recognize renamed brokers, control external Terminal profile elevation settings, or prevent a later child from self-elevating. A permitted non-elevated script may contain such behavior; `all` does not make untrusted code safe. Keep scripts and Terminal profiles trusted, run scratchpad without administrator rights, and use OS-enforced application control or isolation when a real security boundary is required. `--check-config` validates configuration, not future token state or downstream program behavior.

### Tint opacity

`tint-opacity DECIMAL` controls the strength of the black backdrop **behind the active scratchpad workspace**, not the opacity of its member windows. It takes exactly one decimal argument from **0.0 through 1.0 inclusive**, with at most **3 fractional digits**. Examples: `0`, `1`, `0.5`, `.5`, `1.0`, `0.25`, `1.000`. Use ASCII digits and `.` only; a decimal point must have at least one following digit. Signs (including `+` and negative zero), exponents, percentages, hex, commas/locale separators, NaN/inf, trailing junk, extra arguments and extra fractional digits are errors. Values are parsed exactly as integer thousandths, without locale-dependent floating point.

- **Scopes and precedence:** a value outside every profile applies to all profiles that do not set their own value, even profiles declared **before** that global directive. A local value always overrides the global one, including an explicit local `0`. This is scalar override behavior, unlike additive `never-elevate` rules.
- Repeating `tint-opacity` in the **same scope** uses the **last value**. Different profiles do not inherit each other's local settings.
- If neither scope sets it, the original alpha **36/255** is retained exactly (approximately **0.14**, or 14% opacity). `tint-opacity 0.14` also maps to alpha 36. The no-file built-in defaults have the same appearance.
- Conversion is `alpha = (thousandths * 255 + 500) / 1000` using integer division (round-half-up): `0.5` gives **128**, and `1.0` gives **255**, a fully opaque black backdrop. Scratch members remain above it.
- **Zero preserves interaction shielding:** `0.0` is accepted and normalizes to zero, but the applied native alpha has a minimum of **1/255** (also for `0.001`, which rounds to zero). Raw Win32 alpha zero makes the layered popup stop participating in native hit testing. The minimum retains its hit-testable focus surface, background input shielding and normal hide/Restore/exit behavior; zero is therefore almost, not completely, visually transparent. It does not turn off the workspace/input guard.
- Configuration is read at startup. **Restart after editing**; there is no hot reload.

For example, `default` uses 0.5 while `other` inherits the late global 0.25:

```scratchpad
profile default
    tint-opacity .5
    bind Ctrl+Alt+S scratch.toggle
end
profile other
    bind Ctrl+Alt+S scratch.toggle
end
tint-opacity 0.25
```

### Planned appearance settings (not supported yet)

`tint-opacity` is currently the only appearance directive. The names below describe
planned behavior, **not valid configuration syntax**. Do not add them to a config
file, including an unselected profile or an import: validation rejects them until
the corresponding effect is available.

| Proposed name | Planned behavior |
|---|---|
| `tint-fade-ms` | Integer 0–500 milliseconds. Omitted or zero means no fade. |
| `member-opacity` | Base opacity for scratch members, using the same decimal format as `tint-opacity`. Omit to leave native opacity alone unless an inactive override applies. |
| `inactive-opacity` | Overrides the base only for inactive members. If omitted, inactive members use the base if set. |
| `active-border` | Focused-member border color as exactly six hexadecimal RGB digits, without a leading `#` (the comment marker). Omit to leave the native border alone. |

The planned scope rules match `tint-opacity`: the last value in each scope wins,
including globals declared after imports. Each profile-local value overrides its
corresponding global. Changes would take effect after restart. Configured member
opacity rounds to native alpha as above, with a floor of **1/255** for hit testing.
No corner preference is defined yet. Today the omitted backdrop tint remains black
at **36/255**, with no fade and no changes to member opacity or borders.

### Layout

| Directive | Values / meaning |
|---|---|
| `layout gap N` | Integer 0..1000, physical pixels between tiles; default 12 |
| `layout split longest` | Choose the longer remaining side each split, left/right on ties; default |
| `layout split horizontal` | Always divide width into left/right pieces |
| `layout split vertical` | Always divide height into top/bottom pieces |
| `layout split alternate` | Alternate width, height, width, … starting with width |
| `layout anchor top-left` | First piece starts on the left/top; default |
| `layout anchor top-right` | Mirror all tiles horizontally |
| `layout anchor bottom-left` | Mirror all tiles vertically |
| `layout anchor bottom-right` | Mirror both axes |
| `layout single W H` | Integer percentage width/height, each 1..100; default 70 75 |
| `layout multi W H` | Integer percentage width/height, each 1..100; default 90 85 |

**Portrait override:** when the pointer's monitor is taller than it is wide, scratchpad activation and retile commands use `vertical` (top/bottom) splits regardless of this setting, including portrait-flipped screens. Horizontal and horizontal-flipped screens retain the configured split; square screens do too. Orientation comes from the same monitor lookup as placement, using full screen bounds rather than the taskbar-reduced work area or configured layout region. All other options still apply. This is a temporary effective layout: neither the loaded profile nor its stored layout is rewritten, and returning to a horizontal monitor uses its configured split again. Shortcut, tray-menu and tray-double-click activation all follow this policy; there is no continuous rotation/monitor watcher.

The anchor mirrors the **centered layout region**, not the whole monitor. Percentages use positive half-up rounding; centered offsets and split halves use integer floor. A 0 gap is allowed. If a chosen orientation/gap cannot produce positive-sized rectangles for every member, arrangement is rejected before any placement. There is no promise to override an application's minimum-size policy.

### Terminal settings

| Directive | Default / user-configurable behavior |
|---|---|
| `quake-window NAME` | `_quake`, Windows Terminal's dropdown window |
| `terminal-profile NAME` | Omitted: use Windows Terminal's configured default profile |
| `container-profile NAME` | Omitted: use `terminal-profile`, or Terminal's default if that too is omitted |
| `container-filter FILTER` | `label=scratchpad=1`; change it to select your running Podman container(s) |
| `container-directory PATH` | Omitted: keep the container's configured working directory; if set, use an existing absolute POSIX path (starting with `/`) **inside** that container; Podman checks existence |

When supplied, each setting takes one nonempty token, quoted if it contains spaces, with a 1024-code-unit limit. Omit an optional directive instead of setting it to an empty string. Names must match your own installed Terminal profiles; none are assumed to exist. Windows Terminal gives `_quake` its special dropdown behavior; another window name follows Terminal's own policy—the app does not implement a dropdown animation. `terminal.quake PROFILE [COMMAND ARGS...]` selects a named Terminal profile independently of `terminal-profile`.

**Optional dependencies:** only `terminal.*` actions require Windows Terminal (`wt.exe` on PATH with its app execution alias enabled). Only `terminal.container` additionally requires Podman (`podman.exe` on PATH), an available Podman backend, a matching running container and a command installed inside it. Scratch/window actions and configuration validation do not depend on those integrations. Nothing installs or starts a container for you.

`container-filter` is exactly **one** `podman ps --filter` argument, not a shell fragment. The default selects containers you deliberately label `scratchpad=1`; it is not tied to a particular image, container name or external agent. The first matching name in Podman's output is selected; multiple matches produce a notice, so narrow the filter if selection matters. No match or a discovery error launches nothing.

`terminal.container [COMMAND ARGS...]` runs `podman exec -it` through Terminal. With no arguments it opens `/bin/sh`, without login flags, in the container's existing working directory. Supply a different executable and separate arguments if that shell is unavailable or you want a different command. There is no implicit shell command string. For example:

```scratchpad
never-elevate
    all
end-never-elevate
profile default
    # Example only: choose a filter and an existing directory in your container.
    container-filter "label=scratchpad=1"
    container-directory "/app"
    bind Ctrl+Alt+K terminal.container
    # Optional alternative: /bin/bash must actually exist inside the container.
    bind Ctrl+Alt+B terminal.container "/bin/bash" "-l"
end
```

Remove `container-directory` to preserve the container image's working directory. Change `/app`, the filter, optional Terminal profile settings and command arguments to suit your environment; these are explicit configuration choices, not host-workspace paths. To use Docker or another runtime instead, bind `process.exec` or `terminal.quake` with that runtime's executable and separate arguments. No container runtime is a build dependency.

All Terminal window/profile names must be semicolon-free and contain no control characters. Terminal child arguments (including container commands and `container-directory`) additionally cannot be empty, contain a literal double quote, or both contain a space and end in a backslash. These restrictions are checked even in inactive profiles; unsupported values are refused, never silently escaped or rewritten. See [Terminal argument boundary](TERMINAL-BOUNDARY.md) for exact restrictions, parser rationale and alternatives. `container-filter` and generic `process.exec` arguments are not subject to Terminal's syntax restrictions.

### Shortcuts

`bind CHORD ACTION [ARG ...]` associates one exact modifier set with one registered action. The primary key must be last; modifiers are `Win`, `Ctrl`, `Shift`, `Alt`, in any order without duplicates. Extra held modifiers prevent a match. Chord spelling/case is exact.

Primary key forms:

- `A`..`Z`, `0`..`9` (Windows virtual keys, not case-sensitive typed characters).
- `F1`..`F24`.
- `Scan029` for a physical scan code: exactly 3 hexadecimal digits after `Scan`, from 001 through 1FF. Bit 0x100 denotes the extended-key flag. Scan029 is the physical backtick-position key on many keyboards; choose a different key if your layout/hardware needs one.
- `VK20` for an explicit nonmodifier virtual-key code: exactly 2 hexadecimal digits after `VK`, from 01 through FE.
- `Space`, `Enter`, `Tab`, `Escape`, `Backspace`, `Left`, `Up`, `Right`, `Down`, `Home`, `End`, `PageUp`, `PageDown`, `Insert`, `Delete`.

At least one of Win/Ctrl/Alt is required, except unmodified/Shift F13..F24 are allowed. Modifier-only primary virtual keys are rejected. Certain physical scan codes can themselves represent modifier keys; do not bind those or expect them to be portable. Windows/debugger/secure-attention reservations (including F12/Ctrl+Alt+Delete), AltGr behavior, keyboard firmware and competing hooks still impose OS limits.

A physical binding takes priority over a virtual-key binding that happens to map to the same actual key with the same modifiers. This allows layouts to be explicit, but avoid such aliases unless intended. After startup, shortcuts remain disarmed until **all physical keys are released**. The hook is installed before this neutral-key gate; no potentially stale startup VK snapshot is used. A key already visible to another application cannot become a shortcut because startup/layout changed while it was held. Only a **fresh physical down** can trigger a shortcut. Repeats/key-up are consumed only if that press's original down was consumed. Injected input does not change matcher state or trigger actions.

### Registered actions

| Action | Arguments | Behavior |
|---|---:|---|
| `scratch.toggle` | 0 | Show/hide and arrange |
| `scratch.member` | 0 | Take/release the hotkey-time foreground window |
| `scratch.promote` | 0 | Promote a visible member |
| `scratch.restore` | 0 | Show/release every member, preserving failed ownership |
| `window.center` | 0 | Center/resize a **nonmember** using single-window fractions, preserving its Z-order |
| `window.minimize` | 0 | Minimize a nonmember; never hide it |
| `window.toggle-topmost` | 0 | Toggle a nonmember's topmost state; report success only after bounded style acknowledgement |
| `terminal.open` | 0 | Open the configured or default Windows Terminal profile in the quake window |
| `terminal.container` | 0..12 | Discover a matching running Podman container; run `/bin/sh` or the supplied command/arguments |
| `terminal.quake` | 1..12 | First arg is Terminal profile; rest are optional command/arguments |
| `process.exec` | 1..12 | First arg is executable; rest are arguments |

Run `--list-actions` for the compiled registry. Unknown actions, invalid arity, empty required executable/profile names or unsupported Terminal values prevent startup before hooks are installed. Nonmember window utilities refuse scratch members, so they cannot accidentally replace an ownership cookie; release the member first. A topmost timeout reports that the request may still apply later: wait for the target to respond before retrying. Adding more bindings needs only configuration, not a rebuild.

`process.exec` searches a bare executable name through absolute PATH entries, or accepts an absolute drive/UNC path. Drive-relative (`C:tool.exe`), relative-directory paths and implicit current-directory searching are refused. Native `.exe` programs work directly; `.cmd`/`.bat`/PowerShell scripts require explicitly invoking their interpreter as the executable with separate arguments. That is a trusted user's deliberate interpreter invocation, not an implicit shell assembled by this app. Treat configuration containing launch actions as code-equivalent trusted input.

## Multiple instances / diagnostics

`--instance NAME` selects a separate per-session mutex/control window; names allow 1..48 ASCII letters, digits, `-`, `_`. It exists for isolated tests and deliberate advanced configurations, **not** automatic profile switching. Never let instances or another tiler own the same windows; give live instances disjoint hotkeys. Control commands need the matching `--instance`.

`--no-hotkeys` and `--no-tray` are diagnostic switches used by the disposable-window integration test. Do not disable both casually: only `--restore` / `--quit` will remain as convenient control paths. No privileged IPC bypass is installed; controls are ordinary same-session/same-integrity window messages.
