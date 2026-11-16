/* test_zero_alloc.c — the frame walk never touches the heap.
 *
 * Invariant (docs/dev/standards/architecture.md §3.3): a steady-state frame —
 * the same widgets as the frame before, whatever the input — runs lens_begin
 * through lens_end on the per-frame arena alone and never calls libc malloc /
 * calloc / realloc / free. A comment cannot hold that line; this test does,
 * by interposing the allocator for the whole process and counting every call
 * made while a frame is open.
 *
 * Structural change is outside the invariant and outside the measured window:
 * a node entering or leaving the tree, and the node store resizing, use the
 * persistent allocator. The warm-up therefore runs past font loading, cache
 * fill and the store's one-off shrink dwell before counting starts.
 *
 * glibc-only: the wrappers forward to __libc_*. Other libcs skip the test.
 */

#include "optics_test.h"
#include <lens/lens.h>

#include <stdatomic.h>
#include <stddef.h>

#if defined(__GLIBC__)

extern void *__libc_malloc(size_t);
extern void *__libc_calloc(size_t, size_t);
extern void *__libc_realloc(void *, size_t);
extern void __libc_free(void *);

static atomic_bool g_counting;
static atomic_uint g_allocs, g_frees;

void *malloc(size_t n) {
    if (atomic_load(&g_counting))
        atomic_fetch_add(&g_allocs, 1);
    return __libc_malloc(n);
}
void *calloc(size_t n, size_t m) {
    if (atomic_load(&g_counting))
        atomic_fetch_add(&g_allocs, 1);
    return __libc_calloc(n, m);
}
void *realloc(void *p, size_t n) {
    if (atomic_load(&g_counting))
        atomic_fetch_add(&g_allocs, 1);
    return __libc_realloc(p, n);
}
void free(void *p) {
    if (p && atomic_load(&g_counting))
        atomic_fetch_add(&g_frees, 1);
    __libc_free(p);
}

typedef struct state {
    bool check;
    float volume;
    int clicks;
    flux_rect ok_rect; /* where the Apply button was laid out last frame */
} state;

static void build(lens *ui, state *st) {
    lens_column_begin(ui, &(lens_layout_opts){.box = {.id = "root"}, .gap = 8, .pad = 12});
    (void)lens_label(ui, &(lens_label_opts){.box = {.id = "title"}, .text = "Settings"});
    lens_row_begin(ui, &(lens_layout_opts){.box = {.id = "row"}, .gap = 8});
    const lens_response ok =
        lens_button(ui, &(lens_button_opts){.box = {.id = "ok"}, .label = "Apply"});
    st->ok_rect = ok.rect;
    if (ok.clicked)
        st->clicks++;
    (void)lens_button(ui, &(lens_button_opts){.box = {.id = "cancel"}, .label = "Cancel"});
    lens_row_end(ui);
    (void)lens_checkbox(
        ui, &(lens_checkbox_opts){.box = {.id = "chk"}, .label = "Enabled", .value = &st->check});
    (void)lens_slider(
        ui, &(lens_slider_opts){
                .box = {.id = "vol"}, .label = "Volume", .value = &st->volume, .min = 0, .max = 1});
    for (int i = 0; i < 24; i++) {
        lens_push_id_int(ui, i);
        (void)lens_label(ui, &(lens_label_opts){.box = {.id = "item"}, .text = "List entry"});
        lens_pop_id(ui);
    }
    lens_column_end(ui);
}

static lens_input input_for(int frame, const state *st) {
    lens_input in = {.size = sizeof in, .display_size = {480, 800}, .dt_seconds = 1.0f / 60.0f};
    /* Step the pointer down the column, holding still for one six-frame click
     * cycle at each stop, so hover, press, release, focus and animation paths
     * all run under the counter. */
    in.cursor = (flux_point){40.0f, 16.0f + (float)((frame / 6) % 40) * 12.0f};
    /* Every other cycle lands on the Apply button, so a full click completes. */
    if ((frame / 6) % 2 == 0 && st->ok_rect.w > 0.0f)
        in.cursor = (flux_point){st->ok_rect.x + st->ok_rect.w * 0.5f,
                                 st->ok_rect.y + st->ok_rect.h * 0.5f};
    const int phase = frame % 6;
    in.mouse_down[LENS_MOUSE_LEFT] = phase == 2 || phase == 3;
    in.mouse_pressed[LENS_MOUSE_LEFT] = phase == 2;
    in.mouse_released[LENS_MOUSE_LEFT] = phase == 4;
    return in;
}

int main(void) {
    lens *ui = nullptr;
    OT_REQUIRE(lens_create(&(lens_desc){.size = sizeof(lens_desc)}, &ui) == FLUX_OK);
    state st = {.volume = 0.5f};

    enum { WARMUP = 700, MEASURED = 600 };
    for (int f = 0; f < WARMUP + MEASURED; f++) {
        const lens_input in = input_for(f, &st);
        const bool measured = f >= WARMUP;

        atomic_store(&g_counting, measured);
        lens_begin(ui, &in);
        build(ui, &st);
        lens_end(ui);
        atomic_store(&g_counting, false);

        OT_EXPECT(!lens_overflowed(ui));

        /* Publication is not the frame walk: a snapshot owns its captured
         * visuals and may allocate. It still has to happen, because input
         * hit-tests against the activated snapshot's geometry. */
        lens_scene_snapshot *snapshot = nullptr;
        if (lens_snapshot_create(ui, &snapshot) == FLUX_OK)
            OT_EXPECT(lens_snapshot_activate(ui, snapshot) == FLUX_OK);
        lens_snapshot_release(snapshot);
    }

    OT_EXPECT(st.clicks > 0); /* the interaction paths really ran */
    OT_EXPECT(atomic_load(&g_allocs) == 0);
    OT_EXPECT(atomic_load(&g_frees) == 0);
    if (atomic_load(&g_allocs) || atomic_load(&g_frees))
        fprintf(stderr, "frame walk heap traffic over %d frames: %u allocations, %u frees\n",
                MEASURED, atomic_load(&g_allocs), atomic_load(&g_frees));

    lens_release(ui);
    return OT_REPORT();
}

#else

int main(void) {
    OT_SKIP("allocator interposition needs glibc");
}

#endif
