#include "scratchpad.h"
#include <string.h>

static SpResult result(SpStatus status, size_t failed) {
    return (SpResult){status,failed};
}
static SpResult operation_result(size_t failed) {
    return result(failed ? SP_WINDOW_ERROR : SP_OK, failed);
}
static void remove_at(SpState *s, size_t i) {
    --s->count;
    memmove(&s->windows[i], &s->windows[i+1], (s->count-i)*sizeof(s->windows[0]));
}
static size_t index_of(const SpState *s, uintptr_t h) {
    for (size_t i=0;i<s->count;++i) if (s->windows[i].handle==h) return i;
    return s->count;
}
bool sp_init(SpState *s, SpWindowOps ops, void *ctx) {
    if (!s || !ops.claim || !ops.alive || !ops.hide || !ops.tile || !ops.release || !ops.focus)
        return false;
    memset(s,0,sizeof(*s)); s->ops=ops; s->layout=sp_default_layout(); s->ctx=ctx; return true;
}
size_t sp_prune(SpState *s) {
    size_t before=s->count;
    for (size_t i=0;i<s->count;)
        if (!s->ops.alive(s->ctx,s->windows[i])) remove_at(s,i); else ++i;
    return before-s->count;
}
static bool surface(SpState *s,bool active,SpRect work) {
    return !s->workspace || s->workspace(s->workspace_ctx,s,active,work);
}
static SpLayout effective_layout(const SpState *s) {
    SpLayout layout=s->layout;
    /* Override only a valid copy: preserve configuration and fail-closed validation. */
    if (s->vertical_screen && sp_valid_layout(layout)) layout.split=SP_SPLIT_VERTICAL;
    return layout;
}
static SpStatus preflight(SpState *s,SpRect work,SpRect *rects) {
    /* Validate work/layout even when the surface has no members. */
    return sp_layout_with(work,s->count?s->count:1,rects,SP_CAPACITY,effective_layout(s));
}
static SpResult arrange_members(SpState *s,const SpRect *rects,uintptr_t focus) {
    if (!s->count) return result(SP_OK,0);
    size_t failed=0;
    for (size_t i=0;i<s->count;++i)
        if (!s->ops.tile(s->ctx,s->windows[i],rects[i])) ++failed;
    size_t i=focus ? index_of(s,focus) : s->count-1;
    if (i<s->count && s->ops.alive(s->ctx,s->windows[i]) &&
        !s->ops.focus(s->ctx,s->windows[i])) ++failed;
    return operation_result(failed);
}
SpResult sp_arrange(SpState *s, SpRect work, uintptr_t focus) {
    sp_prune(s);
    SpRect rects[SP_CAPACITY];
    SpStatus status=preflight(s,work,rects);
    if (status!=SP_OK) return result(status,0);
    if (s->visible && !surface(s,true,work)) {
        /* Do not wait for members, or claim inactive, until removal is known.
           A timed-out surface may still be intercepting input; retain retry state. */
        if (!surface(s,false,work)) return operation_result(2);
        s->visible=false;size_t failed=1;
        for (size_t i=0;i<s->count;++i) if (!s->ops.hide(s->ctx,s->windows[i])) ++failed;
        return operation_result(failed);
    }
    return arrange_members(s,rects,focus);
}
SpResult sp_toggle_member(SpState *s, uintptr_t handle, SpRect work) {
    sp_prune(s);
    if (!handle) return result(SP_NOOP,0);
    size_t i=index_of(s,handle);
    if (i<s->count) {
        if (!s->ops.release(s->ctx,s->windows[i])) {
            sp_prune(s); return result(SP_WINDOW_ERROR,1);
        }
        remove_at(s,i);
        if (s->visible) return sp_arrange(s,work,0);
        return result(SP_OK,0);
    }
    if (s->count==SP_CAPACITY) return result(SP_TOO_MANY,0);
    if (s->visible) {
        SpRect rects[SP_CAPACITY];
        SpStatus status=sp_layout_with(work,s->count+1,rects,SP_CAPACITY,effective_layout(s));
        if (status!=SP_OK) return result(status,0);
    }
    SpWindow w;
    if (!s->ops.claim(s->ctx,handle,&w)) return result(SP_REJECTED,1);
    s->windows[s->count++]=w;
    if (!s->ops.hide(s->ctx,w)) {
        /* A timed-out asynchronous hide may still complete. Keep the claim for an explicit retry. */
        sp_prune(s); return result(SP_WINDOW_ERROR,1);
    }
    if (s->visible) return sp_arrange(s,work,0);
    return result(SP_OK,0);
}
SpResult sp_toggle(SpState *s, SpRect work) {
    sp_prune(s);
    if (!s->visible) {
        SpRect rects[SP_CAPACITY];
        SpStatus status=preflight(s,work,rects);
        if (status!=SP_OK) return result(status,0);
        if (!surface(s,true,work)) {
            /* Entry may have shown a shield before failing. Confirm rollback
               before claiming inactive, or retain removal-retry state. */
            bool removed=surface(s,false,work);
            s->visible=!removed;return operation_result(removed?1:2);
        }
        s->visible=true; return arrange_members(s,rects,0);
    }
    if (!surface(s,false,work)) return result(SP_WINDOW_ERROR,1);
    s->visible=false;
    size_t failed=0;
    for (size_t i=0;i<s->count;++i)
        if (!s->ops.hide(s->ctx,s->windows[i])) ++failed;
    return operation_result(failed);
}
SpResult sp_promote(SpState *s, uintptr_t handle, SpRect work) {
    sp_prune(s);
    size_t i=index_of(s,handle);
    if (!s->visible || !i || i==s->count) return result(SP_NOOP,0);
    SpRect rects[SP_CAPACITY];
    SpStatus status=sp_layout_with(work,s->count,rects,SP_CAPACITY,effective_layout(s));
    if (status!=SP_OK) return result(status,0);
    SpWindow w=s->windows[i];
    memmove(&s->windows[1],&s->windows[0],i*sizeof(w)); s->windows[0]=w;
    return sp_arrange(s,work,handle);
}
SpResult sp_release_all(SpState *s) {
    sp_prune(s);
    bool active=s->visible;
    if (!surface(s,false,(SpRect){0})) return result(SP_WINDOW_ERROR,1);
    s->visible=false;
    size_t failed=0;
    if (!s->count) return result(active?SP_OK:SP_NOOP,0);
    for (size_t i=0;i<s->count;) {
        SpWindow w=s->windows[i];
        if (s->ops.release(s->ctx,w) || !s->ops.alive(s->ctx,w)) remove_at(s,i);
        else { ++failed; ++i; }
    }
    return operation_result(failed);
}
