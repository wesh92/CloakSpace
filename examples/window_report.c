#include "window_report.h"
#include <stdio.h>

/* Read-only example C module. It never claims/hides a window or launches a process. */
bool example_window_report(SpModuleContext *ctx,const SpBinding *binding) {
    (void)binding;
    RECT r;
    if (!ctx->target || !GetWindowRect(ctx->target,&r)) {
        ctx->notify(ctx->host,L"No readable foreground window rectangle.");return false;
    }
    wchar_t message[160];
    swprintf(message,160,L"Window rectangle: x=%ld y=%ld width=%ld height=%ld.",
        (long)r.left,(long)r.top,(long)(r.right-r.left),(long)(r.bottom-r.top));
    ctx->notify(ctx->host,message);return true;
}
