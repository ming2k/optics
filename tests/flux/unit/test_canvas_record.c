/*
 * Display-list segments (canvas.h "Display-list segments"): record a
 * draw sequence once, replay it later without re-running the emitter.
 *
 * Rendered through the CPU backend (no Vulkan device):
 *
 *  - a replayed segment must produce a pixel-identical framebuffer to
 *    the live-recorded frame, and bump flux_canvas_records_replayed;
 *  - replay must REFUSE (return false, draw nothing) when the canvas
 *    state no longer matches the recording anchor: incoming scissor
 *    (clip), absolute transform (scale), or a released handle;
 *  - nested recordings must both capture: replaying the outer segment
 *    reproduces the inner draws too.
 */
#include "test_helpers.h"
#include <flux/canvas.h>
#include <flux/canvas_cpu.h>

#include <string.h>

#define W 64
#define H 64

static void snapshot(flux_canvas *c, uint8_t *out) {
    uint32_t w = 0, h = 0, stride = 0;
    const uint8_t *fb = flux_canvas_cpu_pixels(c, &w, &h, &stride);
    EXPECT(fb != nullptr && w == W && h == H && stride == W * 4);
    memcpy(out, fb, (size_t)h * stride);
}

static void begin_clear(flux_canvas *c) {
    flux_color clear = flux_color_rgba_premul(0, 0, 0, 255);
    EXPECT(flux_canvas_cpu_begin(c, &clear) == FLUX_OK);
}

/* Record two clipped rects: one at the top-level scissor, one inside a
 * narrower clip (save + clip + draw + restore). */
static flux_canvas_record record_ui(flux_canvas *c) {
    EXPECT(flux_canvas_begin_record(c));
    flux_canvas_fill_rect_color(c, (flux_rect){4, 4, 40, 24},
                                flux_color_rgba_premul(200, 40, 40, 255));
    flux_canvas_save(c);
    flux_canvas_clip_rect(c, (flux_rect){0, 0, 20, H});
    flux_canvas_fill_rect_color(c, (flux_rect){8, 32, 48, 24},
                                flux_color_rgba_premul(40, 200, 40, 255));
    flux_canvas_restore(c);
    flux_canvas_record rec = flux_canvas_end_record(c);
    EXPECT(rec.slot != nullptr);
    return rec;
}

static void test_replay_pixel_identical(void) {
    flux_canvas *c = nullptr;
    EXPECT(flux_canvas_create_cpu(W, H, 1.0f, &c) == FLUX_OK);

    uint8_t live[W * H * 4], replayed[W * H * 4];

    begin_clear(c);
    flux_canvas_record rec = record_ui(c);
    flux_canvas_cpu_end(c);
    snapshot(c, live);
    EXPECT(flux_canvas_records_created(c) == 1);

    /* Second frame: replay only. */
    begin_clear(c);
    EXPECT(flux_canvas_replay(c, rec));
    flux_canvas_cpu_end(c);
    snapshot(c, replayed);
    EXPECT(memcmp(live, replayed, sizeof live) == 0);
    EXPECT(flux_canvas_records_replayed(c) == 1);

    flux_canvas_destroy(c);
}

static void test_replay_refuses_clip_change(void) {
    flux_canvas *c = nullptr;
    EXPECT(flux_canvas_create_cpu(W, H, 1.0f, &c) == FLUX_OK);

    uint8_t blank[W * H * 4];

    begin_clear(c);
    flux_canvas_record rec = record_ui(c);
    flux_canvas_cpu_end(c);

    /* Same segment, but the incoming scissor is now narrower (e.g. an
     * ancestor viewport changed): replay must refuse and draw nothing. */
    begin_clear(c);
    flux_canvas_save(c);
    flux_canvas_clip_rect(c, (flux_rect){0, 0, 10, 10});
    EXPECT(!flux_canvas_replay(c, rec));
    flux_canvas_restore(c);
    flux_canvas_cpu_end(c);
    snapshot(c, blank);
    EXPECT(flux_canvas_records_replayed(c) == 0);
    for (size_t i = 0; i < sizeof blank; i += 4)
        EXPECT(blank[i] == 0 && blank[i + 1] == 0 && blank[i + 2] == 0);

    /* Back under the original scissor the segment is still valid. */
    begin_clear(c);
    EXPECT(flux_canvas_replay(c, rec));
    flux_canvas_cpu_end(c);
    EXPECT(flux_canvas_records_replayed(c) == 1);

    flux_canvas_destroy(c);
}

static void test_replay_refuses_transform_change(void) {
    flux_canvas *c = nullptr;
    EXPECT(flux_canvas_create_cpu(W, H, 1.0f, &c) == FLUX_OK);

    begin_clear(c);
    flux_canvas_record rec = record_ui(c);
    flux_canvas_cpu_end(c);

    /* Vertices are baked in physical pixels: a different content scale
     * invalidates the recording. */
    flux_canvas_set_scale(c, 2.0f);
    begin_clear(c);
    EXPECT(!flux_canvas_replay(c, rec));
    flux_canvas_cpu_end(c);
    EXPECT(flux_canvas_records_replayed(c) == 0);

    flux_canvas_set_scale(c, 1.0f);
    begin_clear(c);
    EXPECT(flux_canvas_replay(c, rec));
    flux_canvas_cpu_end(c);
    EXPECT(flux_canvas_records_replayed(c) == 1);

    flux_canvas_destroy(c);
}

static void test_replay_refuses_released_handle(void) {
    flux_canvas *c = nullptr;
    EXPECT(flux_canvas_create_cpu(W, H, 1.0f, &c) == FLUX_OK);

    begin_clear(c);
    flux_canvas_record rec = record_ui(c);
    flux_canvas_cpu_end(c);

    flux_canvas_record_release(c, rec);
    begin_clear(c);
    EXPECT(!flux_canvas_replay(c, rec)); /* stale generation */
    flux_canvas_cpu_end(c);

    /* Re-record into (likely) the same slot: the old handle must stay
     * invalid, the new one must work. */
    begin_clear(c);
    flux_canvas_record rec2 = record_ui(c);
    EXPECT(!flux_canvas_replay(c, rec));
    EXPECT(flux_canvas_replay(c, rec2));
    flux_canvas_cpu_end(c);

    flux_canvas_destroy(c);
}

static void test_nested_recording_captures_replay(void) {
    flux_canvas *c = nullptr;
    EXPECT(flux_canvas_create_cpu(W, H, 1.0f, &c) == FLUX_OK);

    uint8_t live[W * H * 4], replayed[W * H * 4];

    /* Inner segment: one rect. */
    begin_clear(c);
    EXPECT(flux_canvas_begin_record(c));
    flux_canvas_fill_rect_color(c, (flux_rect){8, 8, 16, 16},
                                flux_color_rgba_premul(40, 40, 220, 255));
    flux_canvas_record inner = flux_canvas_end_record(c);
    EXPECT(inner.slot != nullptr);
    flux_canvas_cpu_end(c);

    /* Outer segment records while the inner REPLAYS: the outer must
     * capture the replayed batch so its own replay is complete. */
    begin_clear(c);
    EXPECT(flux_canvas_begin_record(c));
    flux_canvas_fill_rect_color(c, (flux_rect){32, 32, 16, 16},
                                flux_color_rgba_premul(220, 180, 40, 255));
    EXPECT(flux_canvas_replay(c, inner));
    flux_canvas_record outer = flux_canvas_end_record(c);
    EXPECT(outer.slot != nullptr);
    flux_canvas_cpu_end(c);
    snapshot(c, live);

    begin_clear(c);
    EXPECT(flux_canvas_replay(c, outer));
    flux_canvas_cpu_end(c);
    snapshot(c, replayed);
    EXPECT(memcmp(live, replayed, sizeof live) == 0);

    flux_canvas_destroy(c);
}

int main(void) {
    test_replay_pixel_identical();
    test_replay_refuses_clip_change();
    test_replay_refuses_transform_change();
    test_replay_refuses_released_handle();
    test_nested_recording_captures_replay();
    TEST_SUMMARY();
}
