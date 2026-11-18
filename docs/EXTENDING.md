# Extending the action registry

There are two extension paths. Neither changes the configuration parser, input matcher, or core layout state format.

## 1. External module: no rebuild

Bind `process.exec` to an existing executable plus separate arguments, or `terminal.quake` to one of your installed Terminal profiles plus its command. `terminal.open` uses Terminal's default profile without requiring a particular name. Configure the optional `terminal.container` integration when you want labeled Podman-container discovery; no particular image or workspace layout is required.

```scratchpad
profile default
    bind Ctrl+Alt+N process.exec "notepad.exe"
    bind Ctrl+Alt+T terminal.open
end
```

Use an absolute path for a custom module, for example `"C:/Tools/window-module.exe"`. Such a program can use Win32 APIs itself. It runs with the scratchpad's ordinary token, not extra authority. No shell is inserted, no plugin DLL is loaded, and no sandbox/permission isolation is claimed. There are at most 12 arguments including the executable/profile token. External processes launched this way are **not** managed scratch members or lifetime-owned children; quitting the scratchpad does not terminate the user's launched tools.

## 2. In-process C module: rebuild

The public interface is [src/modules.h](../src/modules.h):

- `SpAction` gives an action name, minimum/maximum argument count, callback and description.
- `SpModuleContext` supplies the ordered scratch state, Win32 adapter, immutable selected profile, hotkey-time target HWND, current pointer-monitor work area (only for existing built-ins that request it), host callbacks and a background-launch callback.
- `SpBinding` supplies immutable, already tokenized wide-string arguments. It and the profile remain valid until shutdown. Do not free/edit them.
- `sp_modules_validate` checks the entire configured registry/arity, built-in required argument semantics and Terminal value restrictions before starting the hook. `src/launch_args.h` shares launch checks with runtime dispatch/the worker. New action-specific semantic checks belong alongside the registry validation rather than only in a callback after startup.

A complete, compiled/tested read-only example is [examples/window_report.c](../examples/window_report.c) with [its header](../examples/window_report.h). It reads the foreground window's outer rectangle and sends a notification; it never hides or launches anything.

To enable that example:

1. Include `../examples/window_report.h` from `src/modules.c`.
2. Add an entry to its `actions` table with name `L"window.report"`, `min_args=0`, `max_args=0`, callback `example_window_report`, and a short wide-string description. Match the existing entries' `SpAction` field order.
3. Add `examples/window_report.c` **exactly once to every compiler command that links `src/modules.c`**, in both `build.cmd` and `build.sh`: the application and `test_screen`, `test_win32`, `test_async`, `test_deactivation`, and `test_input`. The `test_win32` command already includes it; leave that occurrence in place. Registering the callback makes all of these binaries need its definition, even when a test does not invoke the action. (The shipping registry intentionally does not expose the example by default.)
4. Bind the newly registered name in a profile and validate that file. `--list-actions` should include it; `--check-config` should accept it.
5. Extend a C test, run `build.cmd test`, and manually try it with a disposable window before using real work.

### Module rules

- Registry names are case-sensitive. Keep names unique and namespace them (`vendor.action`). Use explicit arity, not unchecked indexing into arguments.
- Callbacks run on the **host UI thread**, never the low-level hook thread. Keep them short and bounded. Queue slow work through an explicitly owned worker; never block keyboard interception on subprocesses or network calls.
- Do not mutate the selected config/profile after input starts. If a callback needs the work area and is not one of the current built-ins, call `sp_win32_workarea(ctx->windows, &rect)` itself and check failure; `ctx->work` is not implicitly populated for arbitrary new names.
- Prefer `sp_toggle_member`, `sp_toggle`, `sp_promote`, `sp_release_all` for scratchpad ownership. The core assumes one owning UI thread and an initialized adapter. Do not call `sp_init` on live state.
- Workspace visibility is independent of member count. Preserve the host-installed `SpState.workspace` / `workspace_ctx` callback when adding actions: it supplies the native tint/shield for empty and populated states. The core invokes it before member operations; the adapter copies identities into its own thread-owned snapshot rather than sharing live `SpState` with hooks.
- A surface-deactivation callback returns true only after native shielding removal is confirmed. On false, retain the active state and all live member claims and skip member hide/release waits; allow a later retry. Do not convert a zero member count into success or discard `SpResult` in a host/control response. `SP_CONTROL_FAILED` marks a failed Restore reply independently of member count/visibility.
- Never hide a window without retaining recoverable ownership. A timed-out asynchronous hide may still complete. Never delete a member or its property merely because an API call failed.
- Standalone `window.*` helpers only operate on nonmembers, validate identity/access using a transient claim, never hide, and drop the claim after their request. Topmost toggles additionally observe the requested style before reporting success; a 400-ms timeout explicitly warns that the queued request may still apply. They are utility requests, not transactional geometry management. Core-owned tiling additionally verifies exact geometry.
- A `SpWindow` is valid only while its process/thread and unique HWND property cookie match. Recheck through `ops.alive` before acting; do not trust an HWND cached by a previous operation.
- Do not overwrite an existing scratch member's property with a second claim. The example is intentionally read-only. For mutating modules, follow the nonmember check/access validation pattern in `window_action`, or deliberately extend the core with tests.
- Hiding does not create a security boundary; modules must not claim secret isolation, UIAccess, rollback of original geometry, or crash recovery that does not exist.

## Tests and seams

- `tests/test_core.c`: pure core with an injected fake window API; lifecycle/error ownership invariants.
- `tests/test_config.c`: parser/chord/directional-layout behavior.
- `tests/test_config_file.c`: injected APPDATA/adjacent discovery errors and genuine-absence fallback; never reads the operator's configuration.
- `tests/test_input.c`: pure input reducer plus hook startup/cancellation tests; hook installation uses an **empty** keymap.
- `tests/test_win32.c`, `tests/test_async.c`: disposable windows only, including direct example-module calls and delayed/hostile behavior.
- `tests/test_overlay.c`: native tint pixels/geometry/hit-testing/Z-order, independent surface lifecycle and production input-guard callback replay without global synthesized input. The real host test separately installs/removes the actual guard hooks.
- `tests/test_deactivation.c`: combined production core/native surface/host/control-client fault regression, including delayed/failed removal and retry, native visibility/guard state, skipped member waits, empty/populated Restore status/exit codes and refused empty exit. Hooks/foreground/member operations are fixtures; no production fault-injection switch or real user input.
- `tests/test_process.c`: real controlled child executables for quoting/capture/errors/timeouts, including final-output-race regression.
- `tests/test_terminals.c`, `tests/terminal_boundary.h`: injected process API plus actual CRT serialization/Windows argv decode and isolated Terminal boundary projections. Verify refusal of unsupported values and supported child round trips, not just a pre-serialization vector. See [the compatibility subset](TERMINAL-BOUNDARY.md); never run real Podman/Terminal during automation.
- `tests/test_host.c`: actual GUI executable, config/control/single-instance/tray/normal-exit behavior against disposable windows.

There is no runtime DLL ABI or binary-compatibility promise. A C registry is a deliberate, auditable first extension layer; use external executable modules when independent deployment matters.
