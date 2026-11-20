/*
 * Display-list segments (canvas.h "Display-list segments"): record a
 * draw sequence once, replay it later without re-running the emitter.
 *
 * Rendered through the CPU backend (no Vulkan device):
 *
 *  - a replayed segment must produce a pixel-identical framebuffer to
 *    the live-recorded frame, and bump flux_canvas_cache_entries_replayed;
 *  - replay must REFUSE (return false, draw nothing) when the canvas
 *    state no longer matches the recording anchor: incoming scissor
 *    (clip), absolute transform (scale), or a released handle;
 *  - nested recordings must both capture: replaying the outer segment
 *    reproduces the inner draws too.
 */
#include "optics_test.h"
#include <optics/gfx/canvas.h>
#include <optics/gfx/canvas_cpu.h>

#include <string.h>

#define W 64
#define H 64

static void snapshot(optics_gfx_canvas *c, uint8_t *out) {
    uint32_t w = 0, h = 0, stride = 0;
    const uint8_t *fb = optics_gfx_canvas_cpu_pixels(c, &w, &h, &stride);
    OT_EXPECT(fb != nullptr && w == W && h == H && stride == W * 4);
    memcpy(out, fb, (size_t)h * stride);
}

static void begin_clear(optics_gfx_canvas *c) {
    optics_gfx_color clear = optics_gfx_color_rgba_premul(0, 0, 0, 255);
    OT_EXPECT(optics_gfx_canvas_begin(
                  c, &(optics_gfx_canvas_pass_desc){.type = OPTICS_GFX_TYPE_CANVAS_PASS_DESC,
                                                    .clear_color = &clear}) == OPTICS_GFX_OK);
}

/* Record two clipped rects: one at the top-level scissor, one inside a
 * narrower clip (save + clip + draw + restore). */
static optics_gfx_canvas_cache_entry record_ui(optics_gfx_canvas *c) {
    OT_EXPECT(optics_gfx_canvas_cache_begin(c));
    optics_gfx_canvas_fill_rect_color(c, (optics_gfx_rect){4, 4, 40, 24},
                                      optics_gfx_color_rgba_premul(200, 40, 40, 255));
    optics_gfx_canvas_save(c);
    optics_gfx_canvas_clip_rect(c, (optics_gfx_rect){0, 0, 20, H});
    optics_gfx_canvas_fill_rect_color(c, (optics_gfx_rect){8, 32, 48, 24},
                                      optics_gfx_color_rgba_premul(40, 200, 40, 255));
    optics_gfx_canvas_restore(c);
    optics_gfx_canvas_cache_entry rec = optics_gfx_canvas_cache_end(c);
    OT_EXPECT(rec.slot != nullptr);
    return rec;
}

static void test_replay_pixel_identical(void) {
    optics_gfx_canvas *c = nullptr;
    OT_EXPECT(
        optics_gfx_canvas_create(&(optics_gfx_canvas_desc){.type = OPTICS_GFX_TYPE_CANVAS_DESC,
                                                           .backend = OPTICS_GFX_CANVAS_BACKEND_CPU,
                                                           .scale = 1.0f,
                                                           .width = W,
                                                           .height = H},
                                 &c) == OPTICS_GFX_OK);

    uint8_t live[W * H * 4], replayed[W * H * 4];

    begin_clear(c);
    optics_gfx_canvas_cache_entry rec = record_ui(c);
    OT_EXPECT(optics_gfx_canvas_end(c) == OPTICS_GFX_OK);
    snapshot(c, live);
    OT_EXPECT(optics_gfx_canvas_cache_entries_created(c) == 1);

    /* Second frame: replay only. */
    begin_clear(c);
    OT_EXPECT(optics_gfx_canvas_cache_replay(c, rec));
    OT_EXPECT(optics_gfx_canvas_end(c) == OPTICS_GFX_OK);
    snapshot(c, replayed);
    OT_EXPECT(memcmp(live, replayed, sizeof live) == 0);
    OT_EXPECT(optics_gfx_canvas_cache_entries_replayed(c) == 1);

    optics_gfx_canvas_release(c);
}

static void test_replay_refuses_clip_change(void) {
    optics_gfx_canvas *c = nullptr;
    OT_EXPECT(
        optics_gfx_canvas_create(&(optics_gfx_canvas_desc){.type = OPTICS_GFX_TYPE_CANVAS_DESC,
                                                           .backend = OPTICS_GFX_CANVAS_BACKEND_CPU,
                                                           .scale = 1.0f,
                                                           .width = W,
                                                           .height = H},
                                 &c) == OPTICS_GFX_OK);

    uint8_t blank[W * H * 4];

    begin_clear(c);
    optics_gfx_canvas_cache_entry rec = record_ui(c);
    OT_EXPECT(optics_gfx_canvas_end(c) == OPTICS_GFX_OK);

    /* Same segment, but the incoming scissor is now narrower (e.g. an
     * ancestor viewport changed): replay must refuse and draw nothing. */
    begin_clear(c);
    optics_gfx_canvas_save(c);
    optics_gfx_canvas_clip_rect(c, (optics_gfx_rect){0, 0, 10, 10});
    OT_EXPECT(!optics_gfx_canvas_cache_replay(c, rec));
    optics_gfx_canvas_restore(c);
    OT_EXPECT(optics_gfx_canvas_end(c) == OPTICS_GFX_OK);
    snapshot(c, blank);
    OT_EXPECT(optics_gfx_canvas_cache_entries_replayed(c) == 0);
    for (size_t i = 0; i < sizeof blank; i += 4)
        OT_EXPECT(blank[i] == 0 && blank[i + 1] == 0 && blank[i + 2] == 0);

    /* Back under the original scissor the segment is still valid. */
    begin_clear(c);
    OT_EXPECT(optics_gfx_canvas_cache_replay(c, rec));
    OT_EXPECT(optics_gfx_canvas_end(c) == OPTICS_GFX_OK);
    OT_EXPECT(optics_gfx_canvas_cache_entries_replayed(c) == 1);

    optics_gfx_canvas_release(c);
}

static void test_replay_refuses_transform_change(void) {
    optics_gfx_canvas *c = nullptr;
    OT_EXPECT(
        optics_gfx_canvas_create(&(optics_gfx_canvas_desc){.type = OPTICS_GFX_TYPE_CANVAS_DESC,
                                                           .backend = OPTICS_GFX_CANVAS_BACKEND_CPU,
                                                           .scale = 1.0f,
                                                           .width = W,
                                                           .height = H},
                                 &c) == OPTICS_GFX_OK);

    begin_clear(c);
    optics_gfx_canvas_cache_entry rec = record_ui(c);
    OT_EXPECT(optics_gfx_canvas_end(c) == OPTICS_GFX_OK);

    /* Vertices are baked in physical pixels: a different content scale
     * invalidates the recording. */
    optics_gfx_canvas_set_scale(c, 2.0f);
    begin_clear(c);
    OT_EXPECT(!optics_gfx_canvas_cache_replay(c, rec));
    OT_EXPECT(optics_gfx_canvas_end(c) == OPTICS_GFX_OK);
    OT_EXPECT(optics_gfx_canvas_cache_entries_replayed(c) == 0);

    optics_gfx_canvas_set_scale(c, 1.0f);
    begin_clear(c);
    OT_EXPECT(optics_gfx_canvas_cache_replay(c, rec));
    OT_EXPECT(optics_gfx_canvas_end(c) == OPTICS_GFX_OK);
    OT_EXPECT(optics_gfx_canvas_cache_entries_replayed(c) == 1);

    optics_gfx_canvas_release(c);
}

static void test_replay_refuses_released_handle(void) {
    optics_gfx_canvas *c = nullptr;
    OT_EXPECT(
        optics_gfx_canvas_create(&(optics_gfx_canvas_desc){.type = OPTICS_GFX_TYPE_CANVAS_DESC,
                                                           .backend = OPTICS_GFX_CANVAS_BACKEND_CPU,
                                                           .scale = 1.0f,
                                                           .width = W,
                                                           .height = H},
                                 &c) == OPTICS_GFX_OK);

    begin_clear(c);
    optics_gfx_canvas_cache_entry rec = record_ui(c);
    OT_EXPECT(optics_gfx_canvas_end(c) == OPTICS_GFX_OK);

    optics_gfx_canvas_cache_release(c, rec);
    begin_clear(c);
    OT_EXPECT(!optics_gfx_canvas_cache_replay(c, rec)); /* stale generation */
    OT_EXPECT(optics_gfx_canvas_end(c) == OPTICS_GFX_OK);

    /* Re-record into (likely) the same slot: the old handle must stay
     * invalid, the new one must work. */
    begin_clear(c);
    optics_gfx_canvas_cache_entry rec2 = record_ui(c);
    OT_EXPECT(!optics_gfx_canvas_cache_replay(c, rec));
    OT_EXPECT(optics_gfx_canvas_cache_replay(c, rec2));
    OT_EXPECT(optics_gfx_canvas_end(c) == OPTICS_GFX_OK);

    optics_gfx_canvas_release(c);
}

static void test_nested_recording_captures_replay(void) {
    optics_gfx_canvas *c = nullptr;
    OT_EXPECT(
        optics_gfx_canvas_create(&(optics_gfx_canvas_desc){.type = OPTICS_GFX_TYPE_CANVAS_DESC,
                                                           .backend = OPTICS_GFX_CANVAS_BACKEND_CPU,
                                                           .scale = 1.0f,
                                                           .width = W,
                                                           .height = H},
                                 &c) == OPTICS_GFX_OK);

    uint8_t live[W * H * 4], replayed[W * H * 4];

    /* Inner segment: one rect. */
    begin_clear(c);
    OT_EXPECT(optics_gfx_canvas_cache_begin(c));
    optics_gfx_canvas_fill_rect_color(c, (optics_gfx_rect){8, 8, 16, 16},
                                      optics_gfx_color_rgba_premul(40, 40, 220, 255));
    optics_gfx_canvas_cache_entry inner = optics_gfx_canvas_cache_end(c);
    OT_EXPECT(inner.slot != nullptr);
    OT_EXPECT(optics_gfx_canvas_end(c) == OPTICS_GFX_OK);

    /* Outer segment records while the inner REPLAYS: the outer must
     * capture the replayed batch so its own replay is complete. */
    begin_clear(c);
    OT_EXPECT(optics_gfx_canvas_cache_begin(c));
    optics_gfx_canvas_fill_rect_color(c, (optics_gfx_rect){32, 32, 16, 16},
                                      optics_gfx_color_rgba_premul(220, 180, 40, 255));
    OT_EXPECT(optics_gfx_canvas_cache_replay(c, inner));
    optics_gfx_canvas_cache_entry outer = optics_gfx_canvas_cache_end(c);
    OT_EXPECT(outer.slot != nullptr);
    OT_EXPECT(optics_gfx_canvas_end(c) == OPTICS_GFX_OK);
    snapshot(c, live);

    begin_clear(c);
    OT_EXPECT(optics_gfx_canvas_cache_replay(c, outer));
    OT_EXPECT(optics_gfx_canvas_end(c) == OPTICS_GFX_OK);
    snapshot(c, replayed);
    OT_EXPECT(memcmp(live, replayed, sizeof live) == 0);

    optics_gfx_canvas_release(c);
}

int main(void) {
    test_replay_pixel_identical();
    test_replay_refuses_clip_change();
    test_replay_refuses_transform_change();
    test_replay_refuses_released_handle();
    test_nested_recording_captures_replay();
    return OT_REPORT();
}
