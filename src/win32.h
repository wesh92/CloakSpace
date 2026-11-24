#ifndef SP_WIN32_H
#define SP_WIN32_H
#ifndef WINVER
#define WINVER 0x0A00
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "scratchpad.h"

typedef struct {
    COLORREF key;
    DWORD flags; /* Zero in a snapshot means the window was not layered. */
    BYTE alpha;
} SpWin32LayeredAttrs;
typedef struct {
    SpWindow member;
    SpWin32LayeredAttrs original;
    BYTE applied_alpha; /* Zero if no attributes written by CloakSpace remain. */
    bool added_layered, superseded, busy;
} SpWin32Opacity;
typedef struct {
    DWORD own_pid, integrity;
    bool allow_own; /* Test harness only. The application never claims its own host. */
    uintptr_t next_cookie;
    wchar_t tag[96], pending_tag[96], pending_tick_tag[96], error[256];
    HANDLE observer_thread,observer_ready,observer_stop;
    DWORD observer_error;
    SpWin32Opacity opacity[SP_CAPACITY];
} SpWin32;

bool sp_win32_init(SpWin32 *api, bool allow_own);
/* All API calls/lifecycle are owned by one UI thread. Dispose before its storage expires. */
void sp_win32_dispose(SpWin32 *api);
SpWindowOps sp_win32_ops(void);
/* Native visibility of a tracked member, excluding minimized and cloaked windows. */
bool sp_win32_member_visible(SpWin32 *api, SpWindow member);
/* Resolve a foreground snapshot to the nearest live, visible workspace member
   through its parent and owner chains; NULL if none. UI thread only. */
HWND sp_win32_foreground_member(SpWin32 *api, const SpState *state, HWND foreground);
/* Best effort, on the UI thread after resolving the foreground to a member.
   Pass the effective sp_member_alpha(profile, true/false).value (0 = native).
   NULL focus means all visible members are inactive. */
void sp_win32_apply_member_opacity(SpWin32 *api, const SpState *state, HWND focused_member,
    unsigned focused_alpha, unsigned inactive_alpha);
/* Inactive workspace only: undo our changes on members still or again visible
   after Hide. Keep claims and leave actually hidden members alone. */
void sp_win32_restore_exposed_member_opacity(SpWin32 *api, const SpState *state);
bool sp_win32_workarea(SpWin32 *api, SpRect *out);
/* Read work area and scratch orientation from the same pointer-selected monitor.
   Full monitor bounds (not taskbars/work-area fractions) determine portrait. */
bool sp_win32_scratch_workarea(SpWin32 *api, SpState *state, SpRect *out);
void sp_win32_dpi(void);

#endif
