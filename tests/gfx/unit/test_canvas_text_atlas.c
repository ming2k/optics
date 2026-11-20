/*
 * Atlas-clear invalidation test (CPU backend, no Vulkan device).
 *
 * glyph_ctx's glyph atlas is rearranged wholesale by atlas_clear when the
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
#include "optics_test.h"
#include <optics/gfx/canvas.h>
#include <optics/gfx/canvas_cpu.h>
#include <optics/gfx/math.h>
#include <optics/text.h>

#include <string.h>

/* 112 logical px at scale 8 = 896 physical px: one 512px glyph row fits
 * with room for the baseline. */
#define SCALE 8.0f
#define W 112
#define H 112

static optics_text_metrics measure(optics_text_ctx *t, const char *s, size_t len,
                                   const optics_text_style *style) {
    return optics_text_measure(t,
                               &(optics_text_measure_opts){.utf8 = s, .len = len, .style = style});
}

static void draw(optics_text_ctx *t, optics_gfx_canvas *canvas, optics_gfx_arena *arena, float x,
                 float y, const char *s, size_t len, const optics_text_style *style) {
    optics_text_draw(t, &(optics_text_draw_opts){.canvas = canvas,
                                                 .arena = arena,
                                                 .x = x,
                                                 .y = y,
                                                 .utf8 = s,
                                                 .len = len,
                                                 .style = style});
}

static uint8_t *snapshot(optics_gfx_canvas *c) {
    uint32_t w = 0, h = 0, stride = 0;
    const uint8_t *fb = optics_gfx_canvas_cpu_pixels(c, &w, &h, &stride);
    OT_EXPECT(fb != nullptr && w == W && h == H && stride == W * 4);
    uint8_t *out = malloc((size_t)h * stride);
    OT_EXPECT(out != nullptr);
    if (out)
        memcpy(out, fb, (size_t)h * stride);
    return out;
}

static void begin_clear(optics_gfx_canvas *c) {
    optics_gfx_color clear = optics_gfx_color_rgba_premul(0, 0, 0, 255);
    OT_EXPECT(optics_gfx_canvas_begin(
                  c, &(optics_gfx_canvas_pass_desc){.type = OPTICS_GFX_TYPE_CANVAS_PASS_DESC,
                                                    .clear_color = &clear}) == OPTICS_GFX_OK);
}

int main(void) {
    optics_text_ctx *t = nullptr;
    OT_EXPECT(optics_text_create(&(optics_text_desc){.device = nullptr, .scale = 1.0f}, &t) ==
              OPTICS_GFX_OK);
    OT_EXPECT(t != nullptr);

    /* No shaping backend → measure-only context → nothing to check. */
    optics_text_metrics m = measure(t, "A", 1, nullptr);
    if (m.width <= 0.0f) {
        optics_text_release(t);
        OT_SKIP("no shaping backend");
    }

    optics_gfx_canvas *c = nullptr;
    OT_EXPECT(
        optics_gfx_canvas_create(&(optics_gfx_canvas_desc){.type = OPTICS_GFX_TYPE_CANVAS_DESC,
                                                           .backend = OPTICS_GFX_CANVAS_BACKEND_CPU,
                                                           .scale = SCALE,
                                                           .width = W,
                                                           .height = H},
                                 &c) == OPTICS_GFX_OK);

    optics_text_style style = {};
    style.size_px = 64.0f;
    style.color = optics_gfx_color_rgba_premul(255, 255, 255, 255);

    optics_text_stats st_before, st_after;
    optics_text_get_stats(t, &st_before);

    /* Frame 1: record two glyphs. */
    begin_clear(c);
    OT_EXPECT(optics_gfx_canvas_cache_begin(c));
    draw(t, c, nullptr, 4.0f, 4.0f, "Hg", 2, &style);
    optics_gfx_canvas_cache_entry rec = optics_gfx_canvas_cache_end(c);
    OT_EXPECT(rec.slot != nullptr);
    OT_EXPECT(optics_gfx_canvas_end(c) == OPTICS_GFX_OK);
    uint8_t *live = snapshot(c);

    /* Frame 2: replay is faithful while the atlas is untouched. */
    begin_clear(c);
    OT_EXPECT(optics_gfx_canvas_cache_replay(c, rec));
    OT_EXPECT(optics_gfx_canvas_end(c) == OPTICS_GFX_OK);
    uint8_t *replayed = snapshot(c);
    OT_EXPECT(memcmp(live, replayed, (size_t)W * H * 4) == 0);
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
        optics_text_get_stats(t, &st_after);
        if (st_after.atlas_clears > st_before.atlas_clears)
            break;
        draw(t, c, nullptr, 0.13f * (float)k, 4.0f, filler, 94, &style);
    }
    OT_EXPECT(optics_gfx_canvas_end(c) == OPTICS_GFX_OK);
    optics_text_get_stats(t, &st_after);
    /* atlas_clears keeps its counting semantics (consumers poll it as a
     * generation signal). */
    OT_EXPECT(st_after.atlas_clears > st_before.atlas_clears);

    /* The canvas learns a producer's newest generation from the runs it
     * draws; draw once more so the refusal below keys on the post-clear
     * generation even when the final filler draw triggered the clear. */
    begin_clear(c);
    draw(t, c, nullptr, 4.0f, 4.0f, "x", 1, &style);
    OT_EXPECT(optics_gfx_canvas_end(c) == OPTICS_GFX_OK);

    /* Frame 3: the pre-clear segment must be REFUSED — its baked UVs name
     * rearranged texels. Nothing is drawn. */
    begin_clear(c);
    OT_EXPECT(!optics_gfx_canvas_cache_replay(c, rec));
    OT_EXPECT(optics_gfx_canvas_end(c) == OPTICS_GFX_OK);
    uint8_t *blank = snapshot(c);
    uint8_t all_black[W * 4];
    for (uint32_t x = 0; x < W; ++x) {
        all_black[x * 4 + 0] = 0;
        all_black[x * 4 + 1] = 0;
        all_black[x * 4 + 2] = 0;
        all_black[x * 4 + 3] = 255; /* opaque black clear */
    }
    for (uint32_t y = 0; y < H; ++y)
        OT_EXPECT(memcmp(blank + (size_t)y * W * 4, all_black, sizeof all_black) == 0);
    free(blank);

    /* Frame 4: live re-draw re-rasterises into the new atlas generation
     * and reproduces the recorded pixels exactly. */
    begin_clear(c);
    draw(t, c, nullptr, 4.0f, 4.0f, "Hg", 2, &style);
    OT_EXPECT(optics_gfx_canvas_end(c) == OPTICS_GFX_OK);
    uint8_t *redrawn = snapshot(c);
    OT_EXPECT(memcmp(live, redrawn, (size_t)W * H * 4) == 0);
    free(redrawn);
    free(live);

    /* A segment recorded after the clear replays normally. */
    begin_clear(c);
    OT_EXPECT(optics_gfx_canvas_cache_begin(c));
    draw(t, c, nullptr, 4.0f, 4.0f, "Hg", 2, &style);
    optics_gfx_canvas_cache_entry rec2 = optics_gfx_canvas_cache_end(c);
    OT_EXPECT(rec2.slot != nullptr);
    OT_EXPECT(optics_gfx_canvas_end(c) == OPTICS_GFX_OK);
    begin_clear(c);
    OT_EXPECT(optics_gfx_canvas_cache_replay(c, rec2));
    OT_EXPECT(optics_gfx_canvas_end(c) == OPTICS_GFX_OK);

    optics_gfx_canvas_cache_release(c, rec);
    optics_gfx_canvas_cache_release(c, rec2);
    optics_gfx_canvas_release(c);
    optics_text_release(t);
    return OT_REPORT();
}
