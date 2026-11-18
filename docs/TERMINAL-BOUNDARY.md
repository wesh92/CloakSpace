# Windows Terminal argument boundary

`process.exec` is a direct executable/argv operation. `terminal.*` is not: Windows Terminal parses its own command language, then reconstructs a command line for the selected profile's child. Ordinary Windows CRT quoting alone does not make that second language literal.

## Supported subset (enforced, not silently rewritten)

All profiles are checked before the keyboard hook starts. The worker checks again before launching. Unsupported values produce a diagnostic; no `wt.exe` launch is attempted for them.

| Value | Rules |
|---|---|
| `quake-window`, supplied `terminal-profile` / `container-profile`, and the first `terminal.quake` argument | Nonempty when supplied; no literal semicolon `;`, U+0001..U+001F, or U+007F. Omitted profile settings select Terminal's default rather than passing an empty name. Spaces, Unicode, literal double quotes, trailing backslashes and leading `-` are allowed as metadata, not reconstructed child argv. |
| Optional `terminal.quake` child executable/arguments, `terminal.container` command/arguments, and supplied `container-directory` | Same restrictions, **plus** no empty argument, embedded double quote, or argument that contains an ordinary space and ends in **any** backslash. Both odd and even trailing-backslash runs are unsupported. |
| Resolved paths passed to Terminal | The `wt.exe` path itself is checked for semicolons/control characters. A resolved Podman child path also obeys the child-argument restrictions. A path can therefore resolve successfully yet be refused at this boundary. |
| `container-filter` | Not subject to Terminal restrictions: it is one argument in the separate, directly captured `podman ps` query. It retains the configuration grammar's normal rules. |
| `process.exec` | Only its required executable name must be nonempty. Later arguments may intentionally be empty or contain quotes, semicolons, spaces, tabs and trailing backslashes; the generic CRT serializer is unchanged. Normal DSL lexical limits still apply. |

The DSL already forbids embedded NUL and most control characters. The launch checks also protect directly constructed C-module arguments. A backslash before a semicolon does **not** opt into Terminal syntax: this implementation deliberately rejects even `\;` instead of depending on Terminal-version-specific escaping/de-escaping. Use a semicolon-free profile/window name. For exact arbitrary child argv, bind `process.exec` to the executable or an explicitly chosen interpreter/module, rather than routing it through Terminal. Interpreter command strings still belong to that interpreter's language; no shell sandbox is promised.

A literal space-containing Windows child argument ending in `\` can often be expressed with forward slashes if the child supports them. Do not substitute automatically: changing an argument's contents is the caller's decision.

When a child command is supplied, the builder inserts Terminal's `--` option terminator immediately before it, after any `-p` and `--title` options. With no configured profile, `terminal.open` omits `-p` and lets Terminal select its own default. A profile-only action emits no child or terminator and keeps the profile's own command. Additional child values are **not** a channel for adding Terminal options or extra Terminal commands.

## Why these restrictions exist

The reference inspected for this correction is Microsoft's [AppCommandlineArgs.cpp](https://github.com/microsoft/terminal/blob/main/src/cascadia/TerminalApp/AppCommandlineArgs.cpp). The review's fetched snapshot has SHA256:

```text
df67676044efbbb662e4775eaa38f2e1289b49b6d4157b24af8552ffe1052973
```

The mutable upstream URL may change; the hash identifies the actual reviewed snapshot, not an asserted current release.

- `BuildCommands` / `_addCommandsForArg` split each argv element, including argv[0], using the unescaped-semicolon condition `^;|[^\\];`. Quoting an entire argument for `CreateProcessW` does not protect `Dev;Tools` after the first argv decode.
- `_getNewTerminalArgs` reconstructs child arguments by separating them with spaces and wrapping an argument in double quotes only when it contains an ordinary space. It does not use the generic CRT serializer here. Empty arguments can disappear, embedded quotes can change token boundaries, tabs can split arguments, and trailing backslashes before its inserted closing quote can escape that quote or lose slashes.
- Profile/window metadata is consumed separately as scalar option values rather than going through this child reconstruction. That is why metadata and child arguments have different restrictions.

The application does not copy or link upstream Terminal implementation code. These are explicit compatibility constraints, not a claim to fix every Terminal version's parser.

## Permanent automated checks

Run `build.cmd test` or `bash build.sh test` from the project root.

- `tests/test_input.c` checks parsed configuration and direct dispatch: empty required executable/profile arguments and unsupported Terminal values fail; later empty/quoted/semicolon `process.exec` arguments remain valid.
- `tests/test_host.c` invokes the actual GUI executable with `--check-config`, checking exit 2 and stderr for rejected profiles/window names/child values, and exit 0 for a valid generic-exec configuration. No hooks or terminals are started by those checks.
- `tests/test_terminals.c` injects only executable discovery, process capture and the final spawn. Every accepted spawn first runs the **production** `sp_commandline` serializer and Windows' real `CommandLineToArgvW` decoder, checking exact first-layer argv preservation.
- `tests/terminal_boundary.h` then applies small **C projections** of the reviewed delimiter condition and child reconstruction to that decoded argv. It checks one Terminal command and exact child-argv round trips for the supported subset, including Unicode/spaces/backslashes and the `--` boundary. Separate witnesses demonstrate how raw profile/window/child semicolons split commands, and how empty/quoted/tab/trailing-slash child arguments do not round-trip without the new restrictions. Refusal tests require no spawn, including for unsafe resolved paths.

The projections are intentionally limited boundary fixtures, **not** the full CLI11/Windows Terminal executable, its entire option parser, a real Terminal profile launch or a universal shell-argument guarantee. The controlled-child process suite exercises the generic serializer against real child executables. Manually verify optional Terminal/Podman bindings with your own installed profiles and a disposable container, including profile selection, spaces/Unicode in supported arguments, filter selection and the configured container command/directory. Automation does not start those integrations.
