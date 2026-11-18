#ifndef SP_ELEVATION_H
#define SP_ELEVATION_H
#include "config.h"
#include "process.h"

/* Preflight decoded executable+argv, before resolution/discovery or CreateProcess.
   Reads the process primary token (the token CreateProcess inherits). Recognizes
   explicit runas/sudo/gsudo, flat PowerShell Start-Process -Verb RunAs, and
   Terminal child commands. Unknown targets of recognized elevation requests
   fail closed with an active policy. No elevation/de-elevation is performed.

   This is launch policy, not a sandbox for trusted interpreter code, script
   contents, renamed brokers or external Terminal profile settings. Ordinary
   unelevated commands are not prohibited merely because their names match. */
bool sp_elevation_guard(const SpProfile *profile,const wchar_t *exe,
                         const wchar_t *const *argv,size_t argc,
                         wchar_t *message,size_t capacity);
#endif
