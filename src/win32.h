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
    DWORD own_pid, integrity;
    bool allow_own; /* Test harness only. The application never claims its own host. */
    uintptr_t next_cookie;
    wchar_t tag[96], pending_tag[96], pending_tick_tag[96], error[256];
    HANDLE observer_thread,observer_ready,observer_stop;
    DWORD observer_error;
} SpWin32;

bool sp_win32_init(SpWin32 *api, bool allow_own);
/* All API calls/lifecycle are owned by one UI thread. Dispose before its storage expires. */
void sp_win32_dispose(SpWin32 *api);
SpWindowOps sp_win32_ops(void);
bool sp_win32_workarea(SpWin32 *api, SpRect *out);
/* Read work area and scratch orientation from the same pointer-selected monitor.
   Full monitor bounds (not taskbars/work-area fractions) determine portrait. */
bool sp_win32_scratch_workarea(SpWin32 *api, SpState *state, SpRect *out);
void sp_win32_dpi(void);

#endif
