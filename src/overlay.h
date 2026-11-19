#ifndef SP_OVERLAY_H
#define SP_OVERLAY_H
#include "win32.h"

#define SP_OVERLAY_CLASS L"CloakSpace.Backdrop.v1"

typedef struct SpOverlay SpOverlay;
/* One overlay per process. Its responsive UI/hook thread never calls foreign
   window procedures or waits for member placement. No foreign HWND is disabled.
   Alpha is immutable, copied before starting that thread; 0 is applied as 1
   because a fully transparent layered window loses native hit testing. */
SpOverlay *sp_overlay_create(HWND host,const wchar_t *member_tag,BYTE alpha,wchar_t *error,size_t capacity);
void sp_overlay_destroy(SpOverlay *overlay);
/* SpWorkspaceFn adapter; calls are serialized by the host UI thread. A false
   deactivation result is unconfirmed removal, not permission to forget the surface. */
bool sp_overlay_workspace(void *ctx,const SpState *state,bool active,SpRect work);
const wchar_t *sp_overlay_error(const SpOverlay *overlay);

#endif
