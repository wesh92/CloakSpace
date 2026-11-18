#ifndef SP_MODULES_H
#define SP_MODULES_H
#include "config.h"
#include "win32.h"

typedef struct {
    SpState *scratch;
    SpWin32 *windows;
    const SpProfile *profile;
    HWND target;
    SpRect work;
    void *host;
    void (*report)(void *host,SpResult result);
    void (*notify)(void *host,const wchar_t *message);
    bool (*background)(void *host,const SpBinding *binding);
} SpModuleContext;
typedef bool (*SpActionFn)(SpModuleContext *ctx,const SpBinding *binding);
typedef struct {
    const wchar_t *name;
    size_t min_args,max_args;
    SpActionFn run;
    const wchar_t *description;
} SpAction;
const SpAction *sp_actions(size_t *count);
bool sp_modules_validate(const SpProfile *profile,wchar_t *error,size_t capacity);
bool sp_modules_run(SpModuleContext *ctx,const SpBinding *binding);
#endif
