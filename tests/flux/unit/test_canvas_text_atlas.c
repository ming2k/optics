/*
 * Atlas-clear invalidation test (CPU backend, no Vulkan device).
 *
 * flux_text's glyph atlas is rearranged wholesale by atlas_clear when the
 * shelf packer exhausts the texture. A display-list segment recorded
 * before the clear carries glyph UVs into the pre-clear layout; replaying
 * it against rearranged texels would silently mis-sample. The canvas
 * tracks the producer's atlas generation (flux_glyph_run_host_atlas_desc)
 * and must REFUSE such a replay (return false, draw nothing), after which
 * a live re-draw reproduces the recorded pixels exactly.
 *
 * The clear is forced deterministically: at scale 8 and size_px 64 the
 * raster size is 512 device px, so ~49 glyph cells fill the 4096x4096
 * atlas; drawing the printable-ASCII set at a few fractional x offsets
 * (distinct subpixel phases are distinct atlas entries) clears it
 * several times over. Skips gracefully when no shaping backend is
 * present (same gate as test_canvas_cpu_text).
 */
#include "test_helpers.h"
#include <flux/canvas.h>
#include <flux/canvas_cpu.h>
#include <flux/math.h>
#include <flux-text/text.h>

#include <string.h>

/* 112 logical px at scale 8 = 896 physical px: one 512px glyph row fits
 * with room for the baseline. */
#define SCALE 8.0f
#define W 112
#define H 112

static uint8_t *snapshot(flux_canvas *c) {
    uint32_t w = 0, h = 0, stride = 0;
    const uint8_t *fb = flux_canvas_cpu_pixels(c, &w, &h, &stride);
    EXPECT(fb != nullptr && w == W && h == H && stride == W * 4);
    uint8_t *out = malloc((size_t)h * stride);
    EXPECT(out != nullptr);
    if (out)
        memcpy(out, fb, (size_t)h * stride);
    return out;
}

static void begin_clear(flux_canvas *c) {
    flux_color clear = flux_color_rgba_premul(0, 0, 0, 255);
    EXPECT(flux_canvas_cpu_begin(c, &clear) == FLUX_OK);
}

int main(void) {
    flux_text *t = nullptr;
    EXPECT(flux_text_create(&(flux_text_desc){.device = nullptr, .scale = 1.0f}, &t) == FLUX_OK);
    EXPECT(t != nullptr);

    /* No shaping backend → measure-only context → nothing to check. */
    flux_text_metrics m = flux_text_measure(t, "A", 1, nullptr);
    if (m.width <= 0.0f) {
        flux_text_destroy(t);
        TEST_SUMMARY();
    }

    flux_canvas *c = nullptr;
    EXPECT(flux_canvas_create_cpu(W, H, SCALE, &c) == FLUX_OK);

    flux_text_style style = {0};
    style.size_px = 64.0f;
    style.color = flux_color_rgba_premul(255, 255, 255, 255);

    flux_text_stats st_before, st_after;
    flux_text_get_stats(t, &st_before);

    /* Frame 1: record two glyphs. */
    begin_clear(c);
    EXPECT(flux_canvas_begin_record(c));
    flux_text_draw(t, c, nullptr, 4.0f, 4.0f, "Hg", 2, &style);
    flux_canvas_record rec = flux_canvas_end_record(c);
    EXPECT(rec.slot != nullptr);
    flux_canvas_cpu_end(c);
    uint8_t *live = snapshot(c);

    /* Frame 2: replay is faithful while the atlas is untouched. */
    begin_clear(c);
    EXPECT(flux_canvas_replay(c, rec));
    flux_canvas_cpu_end(c);
    uint8_t *replayed = snapshot(c);
    EXPECT(memcmp(live, replayed, (size_t)W * H * 4) == 0);
    free(replayed);

    /* Force atlas_clear at least once: printable ASCII at a handful of
     * fractional x offsets (distinct subpixel phases are distinct atlas
     * entries), each glyph a 512px cell in a 4096px atlas. */
    char filler[95];
    for (int i = 0; i < 94; ++i)
        filler[i] = (char)(0x21 + i);
    filler[94] = '\0';
    begin_clear(c);
    for (int k = 0; k < 8; ++k) {
        flux_text_get_stats(t, &st_after);
        if (st_after.atlas_clears > st_before.atlas_clears)
            break;
        flux_text_draw(t, c, nullptr, 0.13f * (float)k, 4.0f, filler, 94, &style);
    }
    flux_canvas_cpu_end(c);
    flux_text_get_stats(t, &st_after);
    /* atlas_clears keeps its counting semantics (consumers poll it as a
     * generation signal). */
    EXPECT(st_after.atlas_clears > st_before.atlas_clears);

    /* The canvas learns a producer's newest generation from the runs it
     * draws; draw once more so the refusal below keys on the post-clear
     * generation even when the final filler draw triggered the clear. */
    begin_clear(c);
    flux_text_draw(t, c, nullptr, 4.0f, 4.0f, "x", 1, &style);
    flux_canvas_cpu_end(c);

    /* Frame 3: the pre-clear segment must be REFUSED — its baked UVs name
     * rearranged texels. Nothing is drawn. */
    begin_clear(c);
    EXPECT(!flux_canvas_replay(c, rec));
    flux_canvas_cpu_end(c);
    uint8_t *blank = snapshot(c);
    uint8_t all_black[W * 4];
    for (uint32_t x = 0; x < W; ++x) {
        all_black[x * 4 + 0] = 0;
        all_black[x * 4 + 1] = 0;
        all_black[x * 4 + 2] = 0;
        all_black[x * 4 + 3] = 255; /* opaque black clear */
    }
    for (uint32_t y = 0; y < H; ++y)
        EXPECT(memcmp(blank + (size_t)y * W * 4, all_black, sizeof all_black) == 0);
    free(blank);

    /* Frame 4: live re-draw re-rasterises into the new atlas generation
     * and reproduces the recorded pixels exactly. */
    begin_clear(c);
    flux_text_draw(t, c, nullptr, 4.0f, 4.0f, "Hg", 2, &style);
    flux_canvas_cpu_end(c);
    uint8_t *redrawn = snapshot(c);
    EXPECT(memcmp(live, redrawn, (size_t)W * H * 4) == 0);
    free(redrawn);
    free(live);

    /* A segment recorded after the clear replays normally. */
    begin_clear(c);
    EXPECT(flux_canvas_begin_record(c));
    flux_text_draw(t, c, nullptr, 4.0f, 4.0f, "Hg", 2, &style);
    flux_canvas_record rec2 = flux_canvas_end_record(c);
    EXPECT(rec2.slot != nullptr);
    flux_canvas_cpu_end(c);
    begin_clear(c);
    EXPECT(flux_canvas_replay(c, rec2));
    flux_canvas_cpu_end(c);

    flux_canvas_record_release(c, rec);
    flux_canvas_record_release(c, rec2);
    flux_canvas_destroy(c);
    flux_text_destroy(t);
    TEST_SUMMARY();
}
