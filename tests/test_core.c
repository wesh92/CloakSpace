#include "scratchpad.h"
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned checks;
#define CHECK(x) do { ++checks; if (!(x)) { \
    fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); exit(1); } } while (0)
static void rect_eq(SpRect a, SpRect b) {
    CHECK(a.x == b.x); CHECK(a.y == b.y); CHECK(a.w == b.w); CHECK(a.h == b.h);
}
static const SpRect work = {0, 0, 1920, 1080};

static void geometry(void) {
    SpRect out[SP_CAPACITY], before[SP_CAPACITY];
    memset(out, 0x5a, sizeof(out)); memcpy(before, out, sizeof(out));
    CHECK(sp_layout((SpRect){0}, 0, NULL, 0) == SP_OK);
    CHECK(sp_layout(work, 1, out, SP_CAPACITY) == SP_OK);
    rect_eq(out[0], (SpRect){288, 135, 1344, 810});
    CHECK(sp_layout(work, 2, out, SP_CAPACITY) == SP_OK);
    rect_eq(out[0], (SpRect){96, 81, 858, 918});
    rect_eq(out[1], (SpRect){966, 81, 858, 918});
    CHECK(sp_layout(work, 3, out, SP_CAPACITY) == SP_OK);
    rect_eq(out[2], (SpRect){966, 546, 858, 453});
    CHECK(sp_layout((SpRect){-1921, -101, 1921, 1081}, 1, out, SP_CAPACITY) == SP_OK);
    rect_eq(out[0], (SpRect){-1633, 34, 1345, 811});
    CHECK(sp_layout((SpRect){-400, -300, 100, 100}, 2, out, SP_CAPACITY) == SP_OK);
    rect_eq(out[0], (SpRect){-395, -293, 39, 85});
    rect_eq(out[1], (SpRect){-344, -293, 39, 85});
    CHECK(sp_layout((SpRect){0, 0, 100, 200}, 2, out, SP_CAPACITY) == SP_OK);
    rect_eq(out[0], (SpRect){5, 15, 90, 79});
    memcpy(out, before, sizeof(out));
    CHECK(sp_layout(work, SP_CAPACITY + 1, out, SP_CAPACITY) == SP_TOO_MANY);
    CHECK(sp_layout(work, 2, out, 1) == SP_BAD_ARGUMENT);
    CHECK(sp_layout((SpRect){0,0,0,10}, 1, out, SP_CAPACITY) == SP_BAD_ARGUMENT);
    CHECK(sp_layout((SpRect){INT32_MAX,0,50,50}, 1, out, SP_CAPACITY) == SP_BAD_ARGUMENT);
    CHECK(sp_layout((SpRect){0,0,1,1}, 2, out, SP_CAPACITY) == SP_TOO_SMALL);
    CHECK(sp_layout(work, SP_CAPACITY, out, SP_CAPACITY) == SP_TOO_SMALL);
    CHECK(memcmp(out, before, sizeof(out)) == 0);
    /* Deterministic property sweep: positive, contained, disjoint rectangles. */
    for (int w = 13; w < 2048; w += 37) {
        for (int h = 13; h < 1300; h += 73) {
            for (size_t n = 1; n <= 18; ++n) {
                SpRect area = {-500, -700, w, h};
                SpStatus status = sp_layout(area, n, out, SP_CAPACITY);
                CHECK(status == SP_OK || status == SP_TOO_SMALL);
                if (status != SP_OK) continue;
                for (size_t i = 0; i < n; ++i) {
                    CHECK(out[i].w > 0 && out[i].h > 0);
                    CHECK(out[i].x >= area.x && out[i].y >= area.y);
                    CHECK(out[i].x + out[i].w <= area.x + area.w);
                    CHECK(out[i].y + out[i].h <= area.y + area.h);
                    for (size_t j = 0; j < i; ++j)
                        CHECK(out[i].x >= out[j].x + out[j].w || out[j].x >= out[i].x + out[i].w ||
                              out[i].y >= out[j].y + out[j].h || out[j].y >= out[i].y + out[i].h);
                }
            }
        }
    }
}

typedef struct {
    bool alive, shown, topmost, claimed;
    bool fail_claim, fail_hide, fail_tile, fail_release;
    uintptr_t cookie;
    SpRect rect;
    unsigned releases, tiles, hides;
} FakeWindow;
typedef struct {
    FakeWindow w[SP_CAPACITY + 2]; uintptr_t focus; unsigned focuses;
    bool surface,fail_surface,fail_deactivate; unsigned surface_calls; size_t surface_members;
} Fake;
static bool workspace(void *ctx,const SpState *s,bool active,SpRect area) {
    Fake *f=ctx; (void)area; ++f->surface_calls; f->surface_members=s->count;
    if ((active && f->fail_surface) || (!active && f->fail_deactivate)) return false;
    f->surface=active; return true;
}
static FakeWindow *fw(void *ctx, SpWindow w) { return &((Fake *)ctx)->w[w.handle]; }
static bool claim(void *ctx, uintptr_t h, SpWindow *out) {
    FakeWindow *w = &((Fake *)ctx)->w[h];
    if (!w->alive || w->fail_claim) return false;
    w->claimed = true; ++w->cookie;
    *out = (SpWindow){h, w->cookie, 1, 2}; return true;
}
static bool alive(void *ctx, SpWindow w) {
    FakeWindow *f = fw(ctx,w); return f->alive && f->claimed && f->cookie == w.cookie;
}
static bool hide(void *ctx, SpWindow w) {
    FakeWindow *f = fw(ctx,w); ++f->hides; if (f->fail_hide) return false;
    f->shown = f->topmost = false; return true;
}
static bool tile(void *ctx, SpWindow w, SpRect r) {
    FakeWindow *f = fw(ctx,w); ++f->tiles; if (f->fail_tile) return false;
    f->shown = f->topmost = true; f->rect = r; return true;
}
static bool release(void *ctx, SpWindow w) {
    FakeWindow *f = fw(ctx,w); ++f->releases; if (f->fail_release) return false;
    f->shown = true; f->topmost = f->claimed = false; return true;
}
static bool focus(void *ctx, SpWindow w) {
    Fake *f = ctx; f->focus = w.handle; ++f->focuses; return true;
}
static SpWindowOps ops = {claim,alive,hide,tile,release,focus};
static void setup(Fake *f, SpState *s) {
    memset(f, 0, sizeof(*f));
    for (size_t i=1;i<SP_CAPACITY+2;++i) f->w[i].alive=f->w[i].shown=true;
    CHECK(sp_init(s, ops, f));
    s->workspace=workspace; s->workspace_ctx=f;
}
static void lifecycle(void) {
    Fake f; SpState s; setup(&f,&s);
    CHECK(!sp_init(&s,(SpWindowOps){0},&f)); setup(&f,&s);
    CHECK(sp_toggle(&s,work).status == SP_OK && s.visible && f.surface);
    CHECK(sp_toggle(&s,work).status == SP_OK && !s.visible && !f.surface);
    CHECK(sp_toggle_member(&s,1,work).status == SP_OK);
    CHECK(s.count==1 && !f.w[1].shown && !s.visible);
    CHECK(sp_toggle_member(&s,2,work).status == SP_OK);
    CHECK(sp_promote(&s,2,work).status == SP_NOOP);
    CHECK(sp_toggle(&s,work).status == SP_OK);
    CHECK(s.visible && f.w[1].shown && f.w[2].topmost && f.focus==2);
    rect_eq(f.w[1].rect, (SpRect){96,81,858,918});
    CHECK(sp_promote(&s,2,work).status == SP_OK);
    CHECK(s.windows[0].handle==2 && f.focus==2);
    CHECK(sp_toggle(&s,work).status==SP_OK);CHECK(sp_toggle(&s,work).status==SP_OK);
    CHECK(f.focus==1); /* Focus the current-order last item, not insertion recency. */
    CHECK(sp_promote(&s,2,work).status == SP_NOOP);
    CHECK(sp_toggle_member(&s,3,work).status == SP_OK && f.focus==3);
    CHECK(sp_toggle_member(&s,2,work).status == SP_OK);
    CHECK(s.count==2 && f.w[2].shown && !f.w[2].topmost && !f.w[2].claimed);
    CHECK(sp_toggle(&s,work).status == SP_OK);
    CHECK(!s.visible && !f.w[1].shown && !f.w[3].shown && !f.w[1].topmost);
    CHECK(sp_release_all(&s).status == SP_OK);
    CHECK(!s.visible && s.count==0 && f.w[1].shown && f.w[3].shown);
    CHECK(sp_release_all(&s).status == SP_NOOP);
}
static void failures(void) {
    Fake f; SpState s; setup(&f,&s);
    f.w[1].fail_claim=true;
    CHECK(sp_toggle_member(&s,1,work).status==SP_REJECTED && s.count==0);
    f.w[1].fail_claim=false; f.w[1].fail_hide=true;
    CHECK(sp_toggle_member(&s,1,work).status==SP_WINDOW_ERROR && s.count==1);
    CHECK(f.w[1].releases==0); /* Never forget a possibly pending asynchronous hide. */
    f.w[1].fail_release=true;
    CHECK(sp_toggle_member(&s,1,work).status==SP_WINDOW_ERROR && s.count==1);
    f.w[1].fail_hide=false;
    CHECK(sp_toggle_member(&s,2,work).status==SP_OK);
    SpResult r=sp_release_all(&s);
    CHECK(r.status==SP_WINDOW_ERROR && r.failed==1 && s.count==1);
    CHECK(f.w[2].shown && f.w[2].releases==1 && s.windows[0].handle==1);
    CHECK(sp_toggle_member(&s,1,work).status==SP_WINDOW_ERROR && s.count==1);
    f.w[1].fail_release=false;
    CHECK(sp_release_all(&s).status==SP_OK && s.count==0);
    CHECK(sp_toggle_member(&s,1,work).status==SP_OK);
    CHECK(sp_toggle_member(&s,2,work).status==SP_OK);
    f.w[1].fail_tile=true;
    CHECK(sp_toggle(&s,work).status==SP_WINDOW_ERROR);
    CHECK(f.w[2].shown && f.w[2].tiles==1 && s.count==2);
    f.w[1].fail_hide=true;
    CHECK(sp_toggle(&s,work).status==SP_WINDOW_ERROR && s.count==2 && !s.visible);
    CHECK(!f.w[2].shown);
    f.w[1].alive=false;
    CHECK(sp_prune(&s)==1 && s.count==1);
    ++f.w[2].cookie; /* HWND reused by the same process/thread: do not touch it. */
    CHECK(sp_prune(&s)==1 && s.count==0 && !s.visible);
}
static void empty_workspace(void) {
    Fake f; SpState s; setup(&f,&s);
    CHECK(sp_toggle(&s,(SpRect){0,0,0,100}).status==SP_BAD_ARGUMENT);
    CHECK(!s.visible && !f.surface && !f.surface_calls);
    f.fail_surface=true;
    CHECK(sp_toggle(&s,work).status==SP_WINDOW_ERROR);
    CHECK(!s.visible && !f.surface);
    f.fail_surface=false;
    CHECK(sp_toggle(&s,work).status==SP_OK && s.visible && f.surface);
    CHECK(sp_toggle_member(&s,1,work).status==SP_OK);
    CHECK(f.surface && f.surface_members==1 && f.w[1].shown);
    CHECK(sp_toggle_member(&s,1,work).status==SP_OK);
    CHECK(s.count==0 && s.visible && f.surface && f.surface_members==0);
    CHECK(sp_toggle_member(&s,2,work).status==SP_OK);
    f.w[2].alive=false;
    CHECK(sp_prune(&s)==1 && s.count==0 && s.visible && f.surface);
    CHECK(sp_release_all(&s).status==SP_OK);
    CHECK(!s.visible && !f.surface && !s.count);
    CHECK(sp_release_all(&s).status==SP_NOOP);
    CHECK(sp_toggle_member(&s,3,work).status==SP_OK);
    CHECK(sp_toggle(&s,work).status==SP_OK);
    f.fail_surface=true;
    CHECK(sp_arrange(&s,work,0).status==SP_WINDOW_ERROR);
    CHECK(!s.visible && !f.surface && !f.w[3].shown);
    f.fail_surface=false;
    CHECK(sp_toggle(&s,work).status==SP_OK);
    f.w[3].fail_hide=true;
    CHECK(sp_toggle(&s,work).status==SP_WINDOW_ERROR);
    CHECK(!s.visible && !f.surface); /* Never trap input behind a failed member hide. */
    f.w[3].fail_release=true;
    CHECK(sp_release_all(&s).status==SP_WINDOW_ERROR);
    CHECK(!s.visible && !f.surface && s.count==1);
    f.w[3].fail_release=false;
    CHECK(sp_release_all(&s).status==SP_OK && !s.count);
}
static void deactivation_failures(void) {
    Fake f; SpState s; setup(&f,&s);
    CHECK(sp_toggle(&s,work).status==SP_OK);
    f.fail_deactivate=true;
    SpResult r=sp_release_all(&s);
    CHECK(r.status==SP_WINDOW_ERROR && r.failed==1);
    CHECK(s.visible && f.surface && !s.count); /* Empty does not imply restored. */
    CHECK(sp_release_all(&s).status==SP_WINDOW_ERROR && s.visible);
    f.fail_deactivate=false;
    CHECK(sp_release_all(&s).status==SP_OK && !s.visible && !f.surface);

    CHECK(sp_toggle_member(&s,1,work).status==SP_OK);
    CHECK(sp_toggle_member(&s,2,work).status==SP_OK);
    CHECK(sp_toggle(&s,work).status==SP_OK);
    unsigned hides=f.w[1].hides, releases=f.w[1].releases;
    f.fail_deactivate=true;
    r=sp_toggle(&s,work);
    CHECK(r.status==SP_WINDOW_ERROR && r.failed==1 && s.visible && f.surface);
    CHECK(f.w[1].hides==hides && f.w[2].hides==hides);
    CHECK(f.w[1].shown && f.w[2].shown && s.count==2);
    r=sp_release_all(&s);
    CHECK(r.status==SP_WINDOW_ERROR && r.failed==1 && s.visible && s.count==2);
    CHECK(f.w[1].releases==releases && f.w[2].releases==releases);
    CHECK(f.w[1].claimed && f.w[2].claimed && f.surface);
    f.fail_deactivate=false;
    CHECK(sp_toggle(&s,work).status==SP_OK && !s.visible && !f.surface);
    CHECK(f.w[1].hides==hides+1 && f.w[2].hides==hides+1);
    CHECK(sp_release_all(&s).status==SP_OK && !s.count);

    CHECK(sp_toggle_member(&s,1,work).status==SP_OK);
    CHECK(sp_toggle(&s,work).status==SP_OK);
    hides=f.w[1].hides;
    f.fail_surface=f.fail_deactivate=true;
    r=sp_arrange(&s,work,0);
    CHECK(r.status==SP_WINDOW_ERROR && r.failed==2 && s.visible && f.surface);
    CHECK(f.w[1].hides==hides && f.w[1].shown);
    f.fail_deactivate=false;
    CHECK(sp_arrange(&s,work,0).status==SP_WINDOW_ERROR);
    CHECK(!s.visible && !f.surface && !f.w[1].shown);
    CHECK(sp_release_all(&s).status==SP_OK && !s.count);
}
static void entry_rollback_failures(void) {
    for (size_t members=0;members<=2;members+=2) {
        Fake f; SpState s; setup(&f,&s);
        for (uintptr_t i=1;i<=members;++i) CHECK(sp_toggle_member(&s,i,work).status==SP_OK);
        Fake before=f;
        f.fail_surface=true;
        SpResult r=sp_toggle(&s,work);
        CHECK(r.status==SP_WINDOW_ERROR && r.failed==1 && !s.visible && !f.surface);
        CHECK(f.surface_calls==before.surface_calls+2);
        CHECK(f.focuses==before.focuses && s.count==members);
        for (size_t i=1;i<=2;++i) {
            CHECK(f.w[i].hides==before.w[i].hides && f.w[i].tiles==before.w[i].tiles);
            CHECK(f.w[i].releases==before.w[i].releases && f.w[i].shown==before.w[i].shown);
        }
        unsigned calls=f.surface_calls;
        f.fail_deactivate=true;
        r=sp_toggle(&s,work);
        CHECK(r.status==SP_WINDOW_ERROR && r.failed==2 && s.visible);
        CHECK(f.surface_calls==calls+2 && s.count==members && f.focuses==before.focuses);
        for (size_t i=1;i<=2;++i) {
            CHECK(f.w[i].hides==before.w[i].hides && f.w[i].tiles==before.w[i].tiles);
            CHECK(f.w[i].releases==before.w[i].releases && f.w[i].shown==before.w[i].shown);
        }
        f.fail_deactivate=false;calls=f.surface_calls; /* Entry still fails: this must remove, not show. */
        r=sp_toggle(&s,work);
        CHECK(r.status==SP_OK && !r.failed && !s.visible && !f.surface);
        CHECK(f.surface_calls==calls+1 && s.count==members && f.focuses==before.focuses);
        for (size_t i=1;i<=2;++i) {
            CHECK(f.w[i].hides==before.w[i].hides+(i<=members?1u:0u));
            CHECK(f.w[i].tiles==before.w[i].tiles && f.w[i].releases==before.w[i].releases);
        }
        CHECK(sp_release_all(&s).status==(members?SP_OK:SP_NOOP) && !s.count);
    }
}
static void limits(void) {
    Fake f; SpState s; setup(&f,&s);
    for(uintptr_t i=1;i<=SP_CAPACITY;++i) CHECK(sp_toggle_member(&s,i,work).status==SP_OK);
    CHECK(sp_toggle_member(&s,SP_CAPACITY+1,work).status==SP_TOO_MANY);
    CHECK(sp_toggle(&s,work).status==SP_TOO_SMALL && !s.visible);
    CHECK(!f.w[1].shown && f.w[1].tiles==0);
    CHECK(sp_release_all(&s).status==SP_OK && s.count==0);
    CHECK(sp_toggle_member(&s,1,work).status==SP_OK);
    CHECK(sp_toggle(&s,(SpRect){0,0,1,1}).status==SP_OK);
    CHECK(sp_toggle_member(&s,2,(SpRect){0,0,1,1}).status==SP_TOO_SMALL);
    CHECK(s.count==1 && f.w[2].shown && !f.w[2].claimed);
    CHECK(sp_release_all(&s).status==SP_OK);
}
int main(void) {
    geometry(); lifecycle(); failures(); limits(); empty_workspace(); deactivation_failures(); entry_rollback_failures();
    printf("core: %u checks passed\n", checks); return 0;
}
