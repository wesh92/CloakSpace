# CloakSpace

A Hyprland-style scratchpad workspace for Windows, written in C17/Win32. Take windows into a scratchpad, show or hide them together, and tile them on the monitor under the pointer. The active workspace has a configurable tint and background-input shield.

The executable is **`cloakspace.exe`** and the per-user configuration namespace is **`cloakspace`**. This is not a Windows virtual desktop, a sandbox, or a security boundary.

> [!IMPORTANT]
> This is a **source-available** repository. This means the source is here for you to build and use on your own or fork in accordance with the license in the repository at the time of forking. HOWEVER, no external contributions will be accepted, nor will guarantees be given by the maintainer for any feature addition. **ISSUES** are for reporting possible security issues, to propose and justify features, or to report bugs. They should not and do not indicate any requirement of the maintainer(s) to act on them.

## AI Disclosure

AI played a pivotal role in the creation of this system as well as the documentation. All code, however, is the responsibility of the actual human who checked it in or who attempts to check it in. Every person should do their own due diligence. I, @wesh92 , am an engineer, however, and I have reviewed the code in full and use this on my home system.

If you have personal qualms or take issue with or about code generated in full or in part by AI then you will likely be unable to justify the use of this program. It is, and will continue to be, code and documentation generated, at least in part, by AI systems.

## Prerequisites

- **Windows 10 (1703 or newer) or Windows 11, x64.** Automated verification uses Windows 11. Tests need an interactive desktop session; they create disposable windows and may briefly take focus. Run native test suites one at a time. Leave the keyboard and mouse idle, with no held keys/buttons, open menus, or drags, until they finish; the startup input guard can otherwise refuse a test activation.
- **[Zig 0.17.1](https://ziglang.org/download/)**, the tested compiler version. Extract it and add the directory containing `zig.exe` to `PATH`; open a new shell and check `zig version`. Alternatively, set the `ZIG` environment variable to the full path of `zig.exe`. Both build scripts also look for standard winget installations.
- **Git** to clone the repository, or a downloaded source archive. **Git for Windows / Git Bash** is needed only for the Bash build alternative below.

Zig provides the C compiler, Windows headers/libraries, and resource compiler. No separate Visual Studio, Windows SDK, CMake, Python, Node.js, or dependency-install command is required. Windows Terminal and Podman are optional **runtime integrations**, not build dependencies.

## Build and test

Clone or download this repository and open a shell in its root (the directory containing `src`, `build.cmd`, and this README).

In **PowerShell or Command Prompt**:

```bat
.\build.cmd test
.\build.cmd release
```

Or in **Git Bash on Windows**:

```bash
bash build.sh test
bash build.sh release
```

The scripts build the same native x64 Windows application. `build.sh` requires a Windows Git Bash environment; it is not a Linux, macOS, or WSL build path.

| Mode | Result |
| --- | --- |
| No argument | `build/cloakspace.exe`, with debug information |
| `test` | The same application plus native test executables; runs all suites and validates the shipped configurations |
| `release` | `build/release/cloakspace.exe`, optimized with `NDEBUG` and stripped of debug symbols |

Outputs are relative to the repository root, regardless of the calling directory. The first build populates Zig's caches under `build/`. No installer is produced and nothing is copied to `dist/` automatically. Tests do not require or launch a real Terminal/Podman session. Physical shortcuts, installed applications, and monitor/DPI combinations still need manual checking with disposable windows.

## Validate and run

From the repository root in PowerShell or Command Prompt:

```bat
.\build\release\cloakspace.exe --version
.\build\release\cloakspace.exe --config .\scratchpad.conf --check-config
.\build\release\cloakspace.exe --config .\scratchpad.conf --profile portable --check-config
.\build\release\cloakspace.exe --list-actions
```

`--check-config` validates without starting the tray app, installing hooks, or executing bindings. It checks every profile, but does not check that external programs, Terminal profiles, or container commands are installed. Invalid configuration stops startup rather than silently falling back.

Start with the supplied default profile:

```bat
.\build\release\cloakspace.exe --config .\scratchpad.conf
```

This is a tray application, not a console window. **Run without administrator rights**, release all held keys after startup, and try disposable windows first. Select `--profile portable` to use the alternative profile. Exit any existing default instance before restarting or rebuilding its executable.

### Default shortcuts

| Shortcut | Action |
| --- | --- |
| `Ctrl+Alt+Shift+S` | Take/release the foreground window as a scratchpad member |
| `Ctrl+Alt+S` | Show/hide the scratchpad |
| `Ctrl+Alt+P` | Promote a visible member in the tiling order |
| `Ctrl+Alt+R` | Restore and release all members |
| `Ctrl+Alt+Scan029` | Open Windows Terminal in its quake window, if installed |

`Scan029` means the physical backtick-position key on many keyboards. Change conflicting shortcuts, especially on layouts with AltGr. The tray menu also provides Show / hide, Restore all, and Exit.

Control a running default instance from another shell:

```bat
.\build\release\cloakspace.exe --status
.\build\release\cloakspace.exe --restore
.\build\release\cloakspace.exe --quit
```

Prefer Restore all and normal Exit/`--quit` over ending the process: forced termination can leave member windows hidden, and crash recovery is not provided. If an operation reports failure, let the target respond and retry; do not assume restoration succeeded. Advanced instances created with `--instance NAME` need the same flag on control commands and disjoint hotkeys/windows; see the [configuration reference](docs/CONFIGURATION.md#multiple-instances--diagnostics).

## Configuration

The supplied `scratchpad.conf` is a **versioned sample**, not a generated file. Passing `--config PATH` selects that file explicitly. Without it, discovery uses this order:

1. `%APPDATA%\cloakspace\scratchpad.conf`.
2. `scratchpad.conf` beside the executable.
3. Built-in defaults, only if neither file exists.

The working directory is **not** searched, so a newly built executable does not automatically read the sample in the repository root. For a portable setup, put the EXE and your configuration together in a separate directory; an AppData configuration still takes precedence unless you pass `--config`. For per-user setup, create the AppData directory and copy/edit the sample there. Keep personal configurations outside the source checkout.

When upgrading from an earlier program name, use its tray menu to Restore all and exit first, then move your per-user configuration and imported files into `%APPDATA%\cloakspace\`. The file name remains `scratchpad.conf`; the previous configuration directory is not searched or migrated automatically. The renamed executable controls only CloakSpace instances.

Configuration is UTF-8 **Scratchpad DSL v1**, not TOML/YAML or a shell script. Choose your shortcuts, layout, and optional integrations, validate, then restart after every edit. Relative `import` paths are based on the file containing the directive; keep imported files with their entry point when copying a configuration. The sample's global `never-elevate all` blocks recognized elevation requests, but is a bounded launch policy, not a sandbox: load only trusted configurations and scripts.

Optional integrations:

- `terminal.*` actions need **Windows Terminal** (`wt.exe` on `PATH`, with its app execution alias enabled). `terminal.open` uses its configured default profile unless you set `terminal-profile`; no named profile is assumed.
- `terminal.container` additionally needs **Podman** (`podman.exe` on `PATH`), a running backend and a matching running container. It is not bound by default. Configure the filter (sample: `label=scratchpad=1`), an optional absolute in-container directory, and a command available in that container (default `/bin/sh`). Nothing installs or starts a container for you.
- Scratch/window actions work without either integration. See [Terminal settings](docs/CONFIGURATION.md#terminal-settings) for arguments, restrictions, and examples.

## Source and generated files

Keep `src/` (including `app.rc` and `app.manifest`), `tests/`, `examples/`, `docs/`, the sample configuration, both build scripts, and repository metadata files in version control. Tests read the shipped examples and documentation, so they are part of the source checkout. `.gitattributes` keeps text fixtures and Bash scripts LF, and CMD scripts CRLF, including on Windows clones with automatic line-ending conversion.

**Do not version** generated `build/` or `dist/` directories, Zig caches under `build/`, executables, object/resource outputs (`*.o`, `*.obj`, `*.res`), or debug symbols (`*.pdb`). The supplied `.gitignore` excludes these; `build.cmd` is explicitly exempted from its generic `*.cmd` rule. `dist/` is reserved for any distribution files you assemble yourself, not a required input.

## Reference

- [Configuration, actions, imports, and launch policy](docs/CONFIGURATION.md)
- [Active workspace behavior and recovery limits](docs/ACTIVE-WORKSPACE.md)
- [Portrait-aware tiling](docs/PORTRAIT-TILING.md)
- [Terminal argument compatibility](docs/TERMINAL-BOUNDARY.md)
- [Extending with executables or C modules](docs/EXTENDING.md)
- [Automated builds, release versions and downloadable artifacts](docs/RELEASING.md)

License: [GNU AGPL v3](LICENSE).
