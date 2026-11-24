#ifndef SP_SCRATCHPAD_H
#define SP_SCRATCHPAD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SP_CAPACITY 64u
#define SP_GAP 12

typedef struct { int32_t x, y, w, h; } SpRect;
typedef enum { SP_SPLIT_LONGEST, SP_SPLIT_HORIZONTAL, SP_SPLIT_VERTICAL, SP_SPLIT_ALTERNATE } SpSplit;
typedef struct {
    int gap, single_w, single_h, multi_w, multi_h;
    SpSplit split;
    bool mirror_x, mirror_y;
} SpLayout;
SpLayout sp_default_layout(void);
bool sp_valid_layout(SpLayout layout);
typedef struct {
    uintptr_t handle, cookie;
    uint32_t process_id, thread_id;
} SpWindow;

typedef enum {
    SP_OK, SP_NOOP, SP_BAD_ARGUMENT, SP_TOO_MANY, SP_TOO_SMALL,
    SP_REJECTED, SP_WINDOW_ERROR
} SpStatus;

typedef struct { SpStatus status; size_t failed; } SpResult;

typedef struct {
    bool (*claim)(void *ctx, uintptr_t handle, SpWindow *window);
    bool (*alive)(void *ctx, SpWindow window);
    bool (*hide)(void *ctx, SpWindow window);
    bool (*tile)(void *ctx, SpWindow window, SpRect rect);
    bool (*release)(void *ctx, SpWindow window);
    bool (*focus)(void *ctx, SpWindow window);
} SpWindowOps;

typedef struct SpState SpState;
/* Optional UI surface, including an empty workspace. Cancel delayed entry on failure;
   the core then requests exit to confirm rollback. Exit returns true only after
   shielding is removed. On false (including unconfirmed entry rollback), the core
   retains active workspace state and skips member waits so removal can be retried. */
typedef bool (*SpWorkspaceFn)(void *ctx,const SpState *state,bool active,SpRect work);
/* Optional ordinary Hide adapter. False means removal was not confirmed.
   On true, zero means removed now; a nonzero token defers member hiding until
   sp_finish_hide. Show and immediate removal supersede an outstanding token. */
typedef bool (*SpWorkspaceHideFn)(void *ctx,const SpState *state,uintptr_t *request);

struct SpState {
    SpWindow windows[SP_CAPACITY];
    size_t count;
    bool visible; /* Workspace mode; retained until surface removal is acknowledged. */
    bool vertical_screen; /* Current monitor is taller than wide; layout stays configured. */
    SpWindowOps ops;
    SpLayout layout;
    void *ctx;
    SpWorkspaceFn workspace;
    void *workspace_ctx;
    SpWorkspaceHideFn workspace_hide; /* Uses workspace_ctx. */
    uintptr_t hide_request; /* Owner-thread token, zero when no Hide is pending. */
};

/* On failure, layout does not modify out. Zero members is a successful no-op. */
SpStatus sp_layout(SpRect work, size_t count, SpRect *out, size_t capacity);
SpStatus sp_layout_with(SpRect work, size_t count, SpRect *out, size_t capacity, SpLayout options);
bool sp_init(SpState *state, SpWindowOps ops, void *ctx);
size_t sp_prune(SpState *state);
SpResult sp_arrange(SpState *state, SpRect work, uintptr_t focus);
SpResult sp_toggle_member(SpState *state, uintptr_t handle, SpRect work);
SpResult sp_toggle(SpState *state, SpRect work);
/* Finish only the matching deferred Hide; removed must reflect native acknowledgment. */
SpResult sp_finish_hide(SpState *state, uintptr_t request, bool removed);
SpResult sp_promote(SpState *state, uintptr_t handle, SpRect work);
/* Failed releases stay owned for retry; all other members are still attempted. */
SpResult sp_release_all(SpState *state);

#endif
