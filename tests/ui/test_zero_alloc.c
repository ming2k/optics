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
#include <optics/ui.h>

#include <stdatomic.h>
#include <stddef.h>

#if defined(__has_feature)
#if __has_feature(address_sanitizer)
#define OT_HAS_ASAN 1
#endif
#endif
#if defined(__SANITIZE_ADDRESS__)
#define OT_HAS_ASAN 1
#endif

#if defined(__GLIBC__) && !defined(OT_HAS_ASAN)

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
    optics_gfx_rect ok_rect; /* where the Apply button was laid out last frame */
} state;

static void build(optics_ui *ui, state *st) {
    optics_ui_column_begin(ui,
                           &(optics_ui_layout_opts){.box = {.id = "root"}, .gap = 8, .pad = 12});
    (void)optics_ui_label(ui, &(optics_ui_label_opts){.box = {.id = "title"}, .text = "Settings"});
    optics_ui_row_begin(ui, &(optics_ui_layout_opts){.box = {.id = "row"}, .gap = 8});
    const optics_ui_response ok =
        optics_ui_button(ui, &(optics_ui_button_opts){.box = {.id = "ok"}, .label = "Apply"});
    st->ok_rect = ok.rect;
    if (ok.clicked)
        st->clicks++;
    (void)optics_ui_button(ui,
                           &(optics_ui_button_opts){.box = {.id = "cancel"}, .label = "Cancel"});
    optics_ui_row_end(ui);
    (void)optics_ui_checkbox(
        ui,
        &(optics_ui_checkbox_opts){.box = {.id = "chk"}, .label = "Enabled", .value = &st->check});
    (void)optics_ui_slider(
        ui, &(optics_ui_slider_opts){
                .box = {.id = "vol"}, .label = "Volume", .value = &st->volume, .min = 0, .max = 1});
    for (int i = 0; i < 24; i++) {
        optics_ui_push_id_int(ui, i);
        (void)optics_ui_label(ui,
                              &(optics_ui_label_opts){.box = {.id = "item"}, .text = "List entry"});
        optics_ui_pop_id(ui);
    }
    optics_ui_column_end(ui);
}

static optics_ui_input input_for(int frame, const state *st) {
    optics_ui_input in = {
        .size = sizeof in, .display_size = {480, 800}, .dt_seconds = 1.0f / 60.0f};
    /* Step the pointer down the column, holding still for one six-frame click
     * cycle at each stop, so hover, press, release, focus and animation paths
     * all run under the counter. */
    in.cursor = (optics_gfx_point){40.0f, 16.0f + (float)((frame / 6) % 40) * 12.0f};
    /* Every other cycle lands on the Apply button, so a full click completes. */
    if ((frame / 6) % 2 == 0 && st->ok_rect.w > 0.0f)
        in.cursor = (optics_gfx_point){st->ok_rect.x + st->ok_rect.w * 0.5f,
                                       st->ok_rect.y + st->ok_rect.h * 0.5f};
    const int phase = frame % 6;
    in.mouse_down[OPTICS_UI_MOUSE_LEFT] = phase == 2 || phase == 3;
    in.mouse_pressed[OPTICS_UI_MOUSE_LEFT] = phase == 2;
    in.mouse_released[OPTICS_UI_MOUSE_LEFT] = phase == 4;
    return in;
}

int main(void) {
    optics_ui *ui = nullptr;
    OT_REQUIRE(optics_ui_create(&(optics_ui_desc){.size = sizeof(optics_ui_desc)}, &ui) ==
               OPTICS_GFX_OK);
    state st = {.volume = 0.5f};

    enum { WARMUP = 700, MEASURED = 600 };
    for (int f = 0; f < WARMUP + MEASURED; f++) {
        const optics_ui_input in = input_for(f, &st);
        const bool measured = f >= WARMUP;

        atomic_store(&g_counting, measured);
        optics_ui_begin(ui, &in);
        build(ui, &st);
        optics_ui_end(ui);
        atomic_store(&g_counting, false);

        OT_EXPECT(!optics_ui_overflowed(ui));

        /* Publication is not the frame walk: a snapshot owns its captured
         * visuals and may allocate. It still has to happen, because input
         * hit-tests against the activated snapshot's geometry. */
        optics_ui_scene_snapshot *snapshot = nullptr;
        if (optics_ui_snapshot_create(ui, &snapshot) == OPTICS_GFX_OK)
            OT_EXPECT(optics_ui_snapshot_activate(ui, snapshot) == OPTICS_GFX_OK);
        optics_ui_snapshot_release(snapshot);
    }

    OT_EXPECT(st.clicks > 0); /* the interaction paths really ran */
    OT_EXPECT(atomic_load(&g_allocs) == 0);
    OT_EXPECT(atomic_load(&g_frees) == 0);
    if (atomic_load(&g_allocs) || atomic_load(&g_frees))
        fprintf(stderr, "frame walk heap traffic over %d frames: %u allocations, %u frees\n",
                MEASURED, atomic_load(&g_allocs), atomic_load(&g_frees));

    optics_ui_release(ui);
    return OT_REPORT();
}

#else

int main(void) {
    OT_SKIP("allocator interposition needs glibc");
}

#endif
