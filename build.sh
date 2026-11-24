#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "$0")"
if [[ $# -gt 1 || ( $# -eq 1 && $1 != test && $1 != release ) ]]; then printf 'Usage: %s [test|release]\n' "$0" >&2; exit 2; fi
if [[ -z ${ZIG:-} ]]; then
    if command -v zig >/dev/null 2>&1; then ZIG=zig
    else
        for candidate in "$HOME"/AppData/Local/Microsoft/WinGet/Packages/zig.zig_*/zig-*/zig.exe; do
            if [[ -f $candidate ]]; then ZIG=$candidate; fi
        done
    fi
fi
if [[ -z ${ZIG:-} ]]; then printf 'Set ZIG to zig.exe, or install zig.zig with winget.\n' >&2; exit 1; fi
outdir=build
build_flags=(-g)
if [[ ${1:-} == release ]]; then outdir=build/release; build_flags=(-DNDEBUG -s); fi
mkdir -p "$outdir"
# Native tools require native paths: /c/... would otherwise become C:/c/....
root=$(pwd -W)
export ZIG_GLOBAL_CACHE_DIR="$root/build/zig-global-cache" ZIG_LOCAL_CACHE_DIR="$root/build/zig-cache"
common=(-target x86_64-windows-gnu -std=c17 -Wall -Wextra -Werror -O2 "${build_flags[@]}" -DUNICODE -D_UNICODE -Isrc)
winlibs=(-luser32 -ladvapi32 -ldwmapi -lgdi32)
core=(src/layout.c src/scratchpad.c)
all=(src/main.c "${core[@]}" src/win32.c src/overlay.c src/process.c src/elevation.c src/config.c src/config_file.c src/input.c src/hotkeys.c src/modules.c src/terminals.c)
"$ZIG" version
"$ZIG" rc /:auto-includes none /fo "$outdir/app.res" -- src/app.rc
"$ZIG" cc "${common[@]}" -municode -Wl,--subsystem,windows "${all[@]}" "$outdir/app.res" "${winlibs[@]}" -lshell32 -o "$outdir/cloakspace.exe"
printf 'Built %s/cloakspace.exe\n' "$outdir"
if [[ ${1:-} == test ]]; then
    "$ZIG" cc "${common[@]}" tests/test_core.c "${core[@]}" -o build/test_core.exe
    "$ZIG" cc "${common[@]}" tests/test_screen.c "${core[@]}" src/overlay.c src/process.c src/elevation.c src/config.c src/config_file.c src/input.c src/hotkeys.c src/modules.c src/terminals.c "${winlibs[@]}" -lshell32 -o build/test_screen.exe
    "$ZIG" cc "${common[@]}" tests/test_config.c src/config.c src/layout.c -o build/test_config.exe
    "$ZIG" cc "${common[@]}" tests/test_config_file.c src/config_file.c src/config.c src/layout.c -o build/test_config_file.exe
    "$ZIG" cc "${common[@]}" tests/test_win32.c "${core[@]}" src/win32.c src/modules.c examples/window_report.c "${winlibs[@]}" -o build/test_win32.exe
    "$ZIG" cc "${common[@]}" tests/test_foreground.c "${core[@]}" src/win32.c src/overlay.c src/process.c src/elevation.c src/config.c src/config_file.c src/input.c src/hotkeys.c src/modules.c src/terminals.c "${winlibs[@]}" -lshell32 -o build/test_foreground.exe
    "$ZIG" cc "${common[@]}" tests/test_async.c "${core[@]}" src/win32.c src/modules.c "${winlibs[@]}" -o build/test_async.exe
    "$ZIG" cc "${common[@]}" tests/test_overlay.c "${core[@]}" src/win32.c src/config.c "${winlibs[@]}" -o build/test_overlay.exe
    "$ZIG" cc "${common[@]}" -municode tests/test_deactivation.c "${core[@]}" src/win32.c src/process.c src/elevation.c src/config.c src/config_file.c src/input.c src/hotkeys.c src/modules.c src/terminals.c "${winlibs[@]}" -lshell32 -o build/test_deactivation.exe
    "$ZIG" cc "${common[@]}" -municode tests/test_process.c src/process.c src/elevation.c src/config.c src/layout.c -ladvapi32 -lshell32 -o build/test_process.exe
    "$ZIG" cc "${common[@]}" tests/test_terminals.c src/process.c src/elevation.c src/config.c src/layout.c -ladvapi32 -lshell32 -o build/test_terminals.exe
    "$ZIG" cc "${common[@]}" tests/test_elevation.c src/process.c src/elevation.c src/config.c src/config_file.c src/layout.c -ladvapi32 -lshell32 -o build/test_elevation.exe
    "$ZIG" cc "${common[@]}" tests/test_input.c "${core[@]}" src/win32.c src/config.c src/input.c src/hotkeys.c src/modules.c "${winlibs[@]}" -o build/test_input.exe
    "$ZIG" cc "${common[@]}" -municode tests/test_host.c src/process.c -luser32 -o build/test_host.exe
    for suite in core screen config config_file win32 foreground async overlay deactivation process terminals elevation input; do "./build/test_${suite}.exe"; done
    ./build/test_host.exe build/cloakspace.exe
    ./build/cloakspace.exe --config scratchpad.conf --check-config
    ./build/cloakspace.exe --config scratchpad.conf --profile portable --check-config
    ./build/cloakspace.exe --config examples/never-elevate.conf --check-config
    ./build/cloakspace.exe --config examples/never-elevate.conf --profile other --check-config
    ./build/cloakspace.exe --config examples/imports/main.conf --check-config
    ./build/cloakspace.exe --config examples/imports/main.conf --profile work --check-config
fi
