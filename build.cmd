@echo off
setlocal EnableExtensions DisableDelayedExpansion
cd /d "%~dp0"
if not "%~1"=="" if not "%~1"=="test" if not "%~1"=="release" goto usage
if not "%~2"=="" goto usage
if defined ZIG goto compiler_ready
for /f "delims=" %%Z in ('where zig.exe 2^>nul') do if not defined ZIG set "ZIG=%%Z"
if defined ZIG goto compiler_ready
for /d %%D in ("%LOCALAPPDATA%\Microsoft\WinGet\Packages\zig.zig_*") do for /d %%V in ("%%~fD\zig-*") do if exist "%%~fV\zig.exe" set "ZIG=%%~fV\zig.exe"
:compiler_ready
if not defined ZIG (
  echo Set ZIG to zig.exe, or install zig.zig with winget. 1>&2
  exit /b 1
)
if not exist build mkdir build
set "OUTDIR=build"
set "BUILD_FLAGS=-g"
if "%~1"=="release" set "OUTDIR=build\release"
if "%~1"=="release" set "BUILD_FLAGS=-DNDEBUG -s"
if not exist "%OUTDIR%" mkdir "%OUTDIR%"
set "ZIG_GLOBAL_CACHE_DIR=%CD%\build\zig-global-cache"
set "ZIG_LOCAL_CACHE_DIR=%CD%\build\zig-cache"
set "COMMON=-target x86_64-windows-gnu -std=c17 -Wall -Wextra -Werror -O2 %BUILD_FLAGS% -DUNICODE -D_UNICODE -Isrc"
set "CORE=src/layout.c src/scratchpad.c"
set "WINLIBS=-luser32 -ladvapi32 -ldwmapi -lgdi32"
"%ZIG%" version || exit /b 1
"%ZIG%" rc /:auto-includes none /fo "%OUTDIR%\app.res" -- src/app.rc || exit /b 1
"%ZIG%" cc %COMMON% -municode -Wl,--subsystem,windows src/main.c %CORE% src/win32.c src/overlay.c src/process.c src/elevation.c src/config.c src/config_file.c src/input.c src/hotkeys.c src/modules.c src/terminals.c "%OUTDIR%\app.res" %WINLIBS% -lshell32 -o "%OUTDIR%\cloakspace.exe" || exit /b 1
echo Built %OUTDIR%\cloakspace.exe
if not "%~1"=="test" exit /b 0
"%ZIG%" cc %COMMON% tests/test_core.c %CORE% -o build/test_core.exe || exit /b 1
"%ZIG%" cc %COMMON% tests/test_screen.c %CORE% src/overlay.c src/process.c src/elevation.c src/config.c src/config_file.c src/input.c src/hotkeys.c src/modules.c src/terminals.c %WINLIBS% -lshell32 -o build/test_screen.exe || exit /b 1
"%ZIG%" cc %COMMON% tests/test_config.c src/config.c src/layout.c -o build/test_config.exe || exit /b 1
"%ZIG%" cc %COMMON% tests/test_config_file.c src/config_file.c src/config.c src/layout.c -o build/test_config_file.exe || exit /b 1
"%ZIG%" cc %COMMON% tests/test_win32.c %CORE% src/win32.c src/modules.c examples/window_report.c %WINLIBS% -o build/test_win32.exe || exit /b 1
"%ZIG%" cc %COMMON% tests/test_async.c %CORE% src/win32.c src/modules.c %WINLIBS% -o build/test_async.exe || exit /b 1
"%ZIG%" cc %COMMON% tests/test_overlay.c %CORE% src/win32.c src/config.c %WINLIBS% -o build/test_overlay.exe || exit /b 1
"%ZIG%" cc %COMMON% -municode tests/test_deactivation.c %CORE% src/win32.c src/process.c src/elevation.c src/config.c src/config_file.c src/input.c src/hotkeys.c src/modules.c src/terminals.c %WINLIBS% -lshell32 -o build/test_deactivation.exe || exit /b 1
"%ZIG%" cc %COMMON% -municode tests/test_process.c src/process.c src/elevation.c src/config.c src/layout.c -ladvapi32 -lshell32 -o build/test_process.exe || exit /b 1
"%ZIG%" cc %COMMON% tests/test_terminals.c src/process.c src/elevation.c src/config.c src/layout.c -ladvapi32 -lshell32 -o build/test_terminals.exe || exit /b 1
"%ZIG%" cc %COMMON% tests/test_elevation.c src/process.c src/elevation.c src/config.c src/config_file.c src/layout.c -ladvapi32 -lshell32 -o build/test_elevation.exe || exit /b 1
"%ZIG%" cc %COMMON% tests/test_input.c %CORE% src/win32.c src/config.c src/input.c src/hotkeys.c src/modules.c %WINLIBS% -o build/test_input.exe || exit /b 1
"%ZIG%" cc %COMMON% -municode tests/test_host.c src/process.c -luser32 -o build/test_host.exe || exit /b 1
for %%T in (core screen config config_file win32 async overlay deactivation process terminals elevation input) do (
  build\test_%%T.exe
  if errorlevel 1 exit /b 1
)
build\test_host.exe build/cloakspace.exe || exit /b 1
start "" /wait build\cloakspace.exe --config scratchpad.conf --check-config
if errorlevel 1 exit /b 1
start "" /wait build\cloakspace.exe --config scratchpad.conf --profile portable --check-config
if errorlevel 1 exit /b 1
start "" /wait build\cloakspace.exe --config examples/never-elevate.conf --check-config
if errorlevel 1 exit /b 1
start "" /wait build\cloakspace.exe --config examples/never-elevate.conf --profile other --check-config
if errorlevel 1 exit /b 1
start "" /wait build\cloakspace.exe --config examples/imports/main.conf --check-config
if errorlevel 1 exit /b 1
start "" /wait build\cloakspace.exe --config examples/imports/main.conf --profile work --check-config
if errorlevel 1 exit /b 1
exit /b 0
:usage
echo Usage: build.cmd [test^|release] 1>&2
exit /b 2
