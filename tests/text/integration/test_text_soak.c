/*
 * Long-session text-engine soak (glyph cache + atlas under sustained churn).
 *
 * The resource-churn soak (test_soak.c) covers create/release cycling but
 * has no time dimension and never touches the text engine — the exact
 * place where the v0.0.24 "long-session churn" fixes landed (glyph LRU
 * eviction at cap, tombstone hygiene at saturation, atlas page advance
 * vs full clear). This test drives those paths with counters as the
 * oracle:
 *
 *   - a working set ABOVE glyph_max_cap/2 must produce evictions, and
 *     the live count must stay pinned at the ceiling (never creep up);
 *   - repeated draws of the SAME text must be near-pure cache hits:
 *     misses stop growing once the working set is resident, so steady
 *     frames do not re-rasterise (the original "lag after a while" bug);
 *   - at most one atlas grow-prefill is expected: pages advance before
 *     any clear, so atlas_clears stays 0 for a working set that fits
 *     the page budget — a climbing clears counter is the "clear storm"
 *     signature;
 *   - host memory stays flat: identical stats across two windows of the
 *     same workload (the high-water policy must not grow per frame).
 *
 * Headless CPU canvas; skips if no shaping backend is compiled in.
 */
#include "optics_test.h"
#include <optics/gfx/canvas.h>
#include <optics/gfx/canvas_cpu.h>

/* one begin/end pair per frame */
#include <optics/gfx/math.h>
#include <optics/text.h>

#include <string.h>

/* Enough distinct CJK codepoints to exceed the 8192 live ceiling of the
 * production glyph table (init 256 / max 16384). U+4E00.. is the CJK
 * Unified Ideographs block: contiguous, real glyph outlines in Noto CJK. */
#define DISTINCT_GLYPHS 9000
static uint32_t g_codepoints[DISTINCT_GLYPHS];

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

/* Draws N codepoints in windows of 16 (a label-sized chunk), like a
 * scrolling list would. Returns the number of draw calls made. */
static uint64_t draw_window(optics_text_ctx *t, optics_gfx_canvas *c, uint32_t start,
                            uint32_t count, float size_px) {
    uint64_t draws = 0;
    for (uint32_t i = 0; i < count; i += 16) {
        char utf8[16 * 4 + 1];
        size_t len = 0;
        for (uint32_t j = 0; j < 16 && i + j < count; j++) {
            uint32_t cp = g_codepoints[start + i + j];
            /* encode UTF-8 (BMP: 3 bytes) */
            if (cp < 0x80) {
                utf8[len++] = (char)cp;
            } else if (cp < 0x800) {
                utf8[len++] = (char)(0xC0 | (cp >> 6));
                utf8[len++] = (char)(0x80 | (cp & 0x3F));
            } else {
                utf8[len++] = (char)(0xE0 | (cp >> 12));
                utf8[len++] = (char)(0x80 | ((cp >> 6) & 0x3F));
                utf8[len++] = (char)(0x80 | (cp & 0x3F));
            }
        }
        optics_text_style style = {};
        style.size_px = size_px;
        style.color = optics_gfx_color_rgba_premul(255, 255, 255, 255);
        draw(t, c, nullptr, 8.0f, 8.0f, utf8, len, &style);
        draws++;
    }
    return draws;
}

int main(void) {
    /* Distinct codepoints: CJK block start + i, sized to burst past the
     * live-entry ceiling (max_cap/2 = 8192). */
    for (uint32_t i = 0; i < DISTINCT_GLYPHS; i++)
        g_codepoints[i] = 0x4E00u + i;

    optics_text_ctx *t = nullptr;
    OT_EXPECT(optics_text_create(&(optics_text_desc){.device = nullptr, .scale = 1.0f}, &t) ==
              OPTICS_GFX_OK);
    OT_EXPECT(t != nullptr);

    /* Skip when no shaping backend, or when no CJK face is installed: the
     * oversize working set below is made of U+4E00.. glyphs, so a Latin-only
     * font set cannot drive the cache past its ceiling. */
    optics_text_metrics probe = measure(t, "A", 1, nullptr);
    optics_text_metrics cjk = measure(t, "\xE4\xB8\x80", 1, nullptr);
    if (probe.width <= 0.0f || cjk.width <= 0.0f) {
        fprintf(stderr, "text_soak: no shaping backend or CJK face; skipping\n");
        optics_text_release(t);
        OT_SKIP("no shaping backend or CJK face");
    }

    optics_gfx_canvas *c = nullptr;
    OT_EXPECT(
        optics_gfx_canvas_create(&(optics_gfx_canvas_desc){.type = OPTICS_GFX_TYPE_CANVAS_DESC,
                                                           .backend = OPTICS_GFX_CANVAS_BACKEND_CPU,
                                                           .scale = 1.0f,
                                                           .width = 256,
                                                           .height = 128},
                                 &c) == OPTICS_GFX_OK);

    /* --- Phase 1: oversize working set (forces evictions, maybe pages). */
    for (uint32_t round = 0; round < 3; round++) {
        optics_gfx_color bg = optics_gfx_color_rgba_premul(0, 0, 0, 255);
        OT_EXPECT(optics_gfx_canvas_begin(
                      c, &(optics_gfx_canvas_pass_desc){.type = OPTICS_GFX_TYPE_CANVAS_PASS_DESC,
                                                        .clear_color = &bg}) == OPTICS_GFX_OK);
        draw_window(t, c, 0, DISTINCT_GLYPHS, 16.0f);
        OT_EXPECT(optics_gfx_canvas_end(c) == OPTICS_GFX_OK);
    }
    optics_text_stats s1;
    optics_text_get_stats(t, &s1);
    OT_EXPECT(s1.glyph_count <= s1.glyph_max_cap / 2); /* pinned at ceiling */
    OT_EXPECT(s1.glyph_evictions > 0);                 /* oversize set evicted */
    OT_EXPECT(s1.glyph_grows <= 7);                    /* 256→16384 = 6 doublings */

    /* --- Phase 2: steady replay of a resident working set. Phase 1 may
     *     have evicted this window's entries (the oversize burst cycled
     *     the whole table), so allow ONE re-rasterisation pass — after
     *     that the same ~250 glyphs every frame must be pure hits.
     *     Frames: 1 miss pass (≤256) + 63 hit frames; a re-rasterise
     *     storm would add 256 misses per frame (64 x 256 = 16384). */
    for (int frame = 0; frame < 64; frame++) {
        optics_gfx_color bg = optics_gfx_color_rgba_premul(0, 0, 0, 255);
        OT_EXPECT(optics_gfx_canvas_begin(
                      c, &(optics_gfx_canvas_pass_desc){.type = OPTICS_GFX_TYPE_CANVAS_PASS_DESC,
                                                        .clear_color = &bg}) == OPTICS_GFX_OK);
        draw_window(t, c, 100, 256, 16.0f);
        OT_EXPECT(optics_gfx_canvas_end(c) == OPTICS_GFX_OK);
    }
    optics_text_stats s2;
    optics_text_get_stats(t, &s2);
    OT_EXPECT(s2.glyph_hits > s1.glyph_hits); /* replay hit the cache */
    uint64_t new_misses = s2.glyph_misses - s1.glyph_misses;
    OT_EXPECT(new_misses < 512); /* one re-raster pass, no storm */

    /* --- Phase 3: two identical churn windows must land on identical
     *      counters (high-water steady state; no per-frame growth). */
    optics_text_stats wa, wb;
    for (int frame = 0; frame < 8; frame++) {
        optics_gfx_color bg = optics_gfx_color_rgba_premul(0, 0, 0, 255);
        OT_EXPECT(optics_gfx_canvas_begin(
                      c, &(optics_gfx_canvas_pass_desc){.type = OPTICS_GFX_TYPE_CANVAS_PASS_DESC,
                                                        .clear_color = &bg}) == OPTICS_GFX_OK);
        draw_window(t, c, 0, 1024, 16.0f);
        OT_EXPECT(optics_gfx_canvas_end(c) == OPTICS_GFX_OK);
    }
    optics_text_get_stats(t, &wa);
    for (int frame = 0; frame < 8; frame++) {
        optics_gfx_color bg = optics_gfx_color_rgba_premul(0, 0, 0, 255);
        OT_EXPECT(optics_gfx_canvas_begin(
                      c, &(optics_gfx_canvas_pass_desc){.type = OPTICS_GFX_TYPE_CANVAS_PASS_DESC,
                                                        .clear_color = &bg}) == OPTICS_GFX_OK);
        draw_window(t, c, 0, 1024, 16.0f);
        OT_EXPECT(optics_gfx_canvas_end(c) == OPTICS_GFX_OK);
    }
    optics_text_get_stats(t, &wb);
    OT_EXPECT(wb.glyph_count == wa.glyph_count); /* no creep */
    OT_EXPECT(wb.atlas_pages == wa.atlas_pages); /* no page growth per frame */
    /* A working set that fits the page budget must not storm-clear:
     * after the phase-1 oversize burst, clears must not climb again
     * while replaying resident content. */
    OT_EXPECT(wb.atlas_clears <= wa.atlas_clears);

    optics_gfx_canvas_release(c);
    optics_text_release(t);
    return OT_REPORT();
}
