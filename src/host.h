#ifndef SP_HOST_H
#define SP_HOST_H
#include "win32.h"
#define SP_VERSION L"1.1.0"
#define SP_HOST_PREFIX L"WinScratchpad.Host.v1."
#define SP_CONTROL_NAME L"WinScratchpad.Control.v1.22D658AE"
#define SP_BIND_MESSAGE (WM_APP+10)
#define SP_TRAY_MESSAGE (WM_APP+11)
#define SP_JOB_MESSAGE (WM_APP+12)
#define SP_CONTROL_STATUS 1u
#define SP_CONTROL_RESTORE 2u
#define SP_CONTROL_QUIT 3u
#define SP_CONTROL_FAILED 0x400u
/* Status result: low byte = member count + 1; bit 8 = workspace active (also
   retained after unconfirmed removal); bit 9 = closing. A Restore reply also
   sets SP_CONTROL_FAILED when the operation failed, even with zero members. */
#endif
