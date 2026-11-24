#ifndef SP_OVERLAY_H
#define SP_OVERLAY_H
#include "win32.h"

#define SP_OVERLAY_CLASS L"CloakSpace.Backdrop.v1"
#define SP_OVERLAY_HIDE_COMPLETE (WM_APP+71)

typedef struct SpOverlay SpOverlay;
/* One overlay per process. Its responsive UI/hook thread never calls foreign
   window procedures or waits for member placement. No foreign HWND is disabled.
   The legacy constructor selects zero fade duration. */
SpOverlay *sp_overlay_create(HWND host,const wchar_t *member_tag,BYTE alpha,wchar_t *error,size_t capacity);
/* Zero duration preserves immediate transitions. Native alpha is at least 1 to
   preserve hit testing; the fade endpoint and duration are fixed at creation. */
SpOverlay *sp_overlay_create_with_fade(HWND host,const wchar_t *member_tag,BYTE alpha,UINT fade_ms,
    wchar_t *error,size_t capacity);
void sp_overlay_destroy(SpOverlay *overlay);
/* SpWorkspaceFn adapter. Entry acknowledges the visible shield, not the end of
   the fade. Deactivation here cancels any fade and confirms immediate native
   hide, for Restore all, exit and rollback. Calls are serialized by the host. */
bool sp_overlay_workspace(void *ctx,const SpState *state,bool active,SpRect work);

typedef enum {
    SP_OVERLAY_HIDE_FAILED, SP_OVERLAY_HIDE_DONE, SP_OVERLAY_HIDE_PENDING
} SpOverlayHideResult;
/* Ordinary Hide. PENDING means the guard can still be active; do not commit
   deactivation or start member waits yet. The request output is required and
   receives a nonzero token only on PENDING.
   SP_OVERLAY_HIDE_COMPLETE is a best-effort wake-up to the host (wParam = token).
   Check the result below on notification and on a bounded host timeout in case
   posting fails; a missing wake-up is not a result. A newer request supersedes
   the token, even if a native hide already in progress finishes before it applies. */
SpOverlayHideResult sp_overlay_hide(SpOverlay *overlay,const SpState *state,uintptr_t *request);
/* Returns true only after that deferred hide finishes. On true, *hidden says
   whether native removal was confirmed; on failure keep the workspace retryable. */
bool sp_overlay_hide_completion(SpOverlay *overlay,uintptr_t request,bool *hidden);
const wchar_t *sp_overlay_error(const SpOverlay *overlay);

#endif
