# Portrait-screen tiling override

## Behavior

Scratchpad activation forces **vertical, top/bottom recursive splits** when the selected monitor is taller than it is wide. This also covers portrait-flipped displays. Horizontal and horizontal-flipped displays keep the configured split; square displays do too.

The existing pointer-monitor lookup supplies both `rcWork` for placement and `rcMonitor` for orientation in one `GetMonitorInfoW` call. There was no separate screen-type detector in this project. Reusing monitor bounds avoids a second monitor selection, rotation-query dependencies, and false portrait detection caused by taskbars or configured area percentages.

Only the effective `split` changes. Gaps, anchors/mirroring and single/multiple-window fractions still apply. Neither the immutable profile nor `SpState.layout` is rewritten. Commands re-read the selected monitor, so returning to a horizontal display restores the configured behavior without restarting. There is still no continuous monitor/rotation watcher.

All built-in activation routes use the same lookup: the configured `scratch.toggle` shortcut, tray menu, and tray double-click. Active add/remove/promote arrangements use the same effective-layout policy as initial activation, including geometry preflight. Invalid configured layouts still fail validation rather than being made valid by the override.

### C extension seam

For an action that operates on the scratchpad, call `sp_win32_scratch_workarea(ctx->windows, ctx->scratch, &rect)` and check its result before invoking the core. It updates `SpState.vertical_screen` from the same monitor that supplies `rect`. Ordinary non-scratch work-area queries can continue using `sp_win32_workarea`. The generic `sp_layout_with` geometry function remains configuration-driven and has not changed.

## Automated coverage

`tests/test_screen.c` includes the production host, native monitor-selection code, module dispatch and core state machine. Monitor metadata and member window operations are injected; it does **not** rotate your screen, move your pointer, focus your windows, synthesize input, or install hooks. Other native/host suites independently exercise disposable windows and the built executable. Run `build.cmd test` or `bash build.sh test` from the project root.

Coverage includes:

- Vertical, vertical-flipped, horizontal and horizontal-flipped screen geometries.
- All four configured split modes and all four anchors; nondefault gaps and area percentages.
- Every shortcut/tray activation route, promotion, active member addition/removal, and repeated orientation changes without mutating configuration.
- Tall work areas on horizontal monitors and wide work areas on portrait monitors, negative desktop origins, square monitors and monitor lookup failure without partial activation.
- Cases where only the configured or only the effective split can fit: activation, add and promote preflight must follow the effective policy without partial placement, claiming or reordering. Invalid configured splits remain rejected. Their error diagnostics in the test logs are intentional fault injection.

## Manual physical-display check

Use saved/disposable applications. Configure `layout split horizontal` or `alternate`, take three members, and summon on a portrait screen: every split should be top/bottom. Repeat using tray-menu and double-click activation. On a horizontal and horizontal-flipped screen the configured split should remain in effect. Return to portrait and then landscape without restarting; verify the configuration file was not changed. Gaps, fractions and anchor direction should still match configuration.

Hardware rotation, mixed-DPI/multi-monitor behavior and physical shortcut interception require manual verification on your own hardware; they are not established by the automated suite.
