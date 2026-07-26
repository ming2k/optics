/*
 * Tessellation result cache (internal.h "Tessellation result cache").
 *
 * fill_path / stroke_path cache the path-space ear-clip/stroke output
 * keyed by the verb stream + pixel_scale + stroke parameters. Rendered
 * through the CPU backend:
 *
 *  - a second render of the same path must hit the cache and produce
 *    a pixel-identical framebuffer;
 *  - changing pixel_scale (canvas scale) or stroke_width must miss;
 *  - stalled ear-clips (self-intersecting input) and EVEN_ODD fills
 *    must bypass the cache without disturbing the cached entries.
 *
 * The counters are read straight out of struct flux_canvas via the
 * internal header (same pattern as test_canvas_tess.c).
 */
#include "../../../libs/flux/src/canvas/internal.h"
#include "test_helpers.h"
#include <flux/canvas_cpu.h>

#include <string.h>

#define W 96
#define H 96

/* Render one black-cleared frame drawing `path` (fill or stroke) and
 * snapshot the framebuffer into `out` (W*H*4 bytes). */
static void render(flux_canvas *c, const flux_path *path, const flux_paint *paint, bool stroke,
                   uint8_t *out) {
    flux_color clear = flux_color_rgba_premul(0, 0, 0, 255);
    EXPECT(flux_canvas_cpu_begin(c, &clear) == FLUX_OK);
    if (stroke)
        flux_canvas_stroke_path(c, path, paint);
    else
        flux_canvas_fill_path(c, path, paint);
    flux_canvas_cpu_end(c);
    uint32_t w = 0, h = 0, stride = 0;
    const uint8_t *fb = flux_canvas_cpu_pixels(c, &w, &h, &stride);
    EXPECT(fb != nullptr && w == W && h == H && stride == W * 4);
    memcpy(out, fb, (size_t)h * stride);
}

static bool frames_equal(const uint8_t *a, const uint8_t *b) {
    return memcmp(a, b, W * H * 4) == 0;
}

/* Multi-contour path with cubics (a circle, an opposite-wound diamond
 * hole inside it, and a cubic band) — exercises flatten + hole
 * bridging + ear clip. */
static flux_path *build_icon_path(flux_arena *a) {
    flux_path *p = nullptr;
    EXPECT(flux_path_create(&p, a) == FLUX_OK);
    flux_path_add_circle(p, 48.0f, 38.0f, 26.0f);
    flux_path_move_to(p, 48.0f, 26.0f);
    flux_path_line_to(p, 62.0f, 38.0f);
    flux_path_line_to(p, 48.0f, 50.0f);
    flux_path_line_to(p, 34.0f, 38.0f);
    flux_path_close(p);
    flux_path_move_to(p, 10.0f, 76.0f);
    flux_path_cubic_to(p, 30.0f, 58.0f, 60.0f, 90.0f, 86.0f, 72.0f);
    flux_path_line_to(p, 86.0f, 80.0f);
    flux_path_cubic_to(p, 60.0f, 96.0f, 30.0f, 66.0f, 10.0f, 84.0f);
    flux_path_close(p);
    return p;
}

/* Self-intersecting heptagram {7/3} (7 vertices, join every 3rd):
 * every candidate ear triangle contains another vertex, so the ear
 * clip's bounded-step guard trips and the fill stalls (ADR-0014).
 * Verified against ear_clip_contour directly — sparser stars such as
 * the {5/2} pentagram have clean spike ears and do NOT stall. */
static flux_path *build_stalled_star(flux_arena *a) {
    flux_path *p = nullptr;
    EXPECT(flux_path_create(&p, a) == FLUX_OK);
    const float cx = W / 2.0f, cy = H / 2.0f, r = 40.0f;
    flux_point v[7];
    for (int i = 0; i < 7; ++i) {
        float t = (float)i * (6.28318530718f / 7.0f) - 1.57079632679f;
        v[i] = (flux_point){cx + r * cosf(t), cy + r * sinf(t)};
    }
    flux_path_move_to(p, v[0].x, v[0].y);
    for (int i = 1; i < 7; ++i)
        flux_path_line_to(p, v[(i * 3) % 7].x, v[(i * 3) % 7].y);
    flux_path_close(p);
    return p;
}

/* Two overlapping rects as one path — parity differs from winding in
 * the overlap, which is what EVEN_ODD is for. */
static flux_path *build_overlap(flux_arena *a) {
    flux_path *p = nullptr;
    EXPECT(flux_path_create(&p, a) == FLUX_OK);
    flux_path_add_rect(p, (flux_rect){16.0f, 16.0f, 40.0f, 40.0f});
    flux_path_add_rect(p, (flux_rect){36.0f, 36.0f, 40.0f, 40.0f});
    return p;
}

int main(void) {
    flux_arena arena;
    EXPECT(flux_arena_init(&arena, 1u << 16, nullptr) == FLUX_OK);

    flux_canvas *c = nullptr;
    EXPECT(flux_canvas_create_cpu(W, H, 1.0f, &c) == FLUX_OK);
    EXPECT(c != nullptr);

    flux_color white = flux_color_rgba_premul(255, 255, 255, 255);
    uint8_t first[W * H * 4], second[W * H * 4];

    /* ---- Fill: second identical render hits and matches exactly ---- */
    flux_path *icon = build_icon_path(&arena);
    flux_paint fill = flux_paint_solid(white);
    EXPECT(c->tess_cache_hits == 0 && c->tess_cache_misses == 0 && c->tess_cache_stores == 0);
    render(c, icon, &fill, false, first);
    EXPECT(c->tess_cache_misses == 1);
    EXPECT(c->tess_cache_stores == 1);
    EXPECT(c->tess_cache_hits == 0);
    render(c, icon, &fill, false, second);
    EXPECT(c->tess_cache_hits == 1);
    EXPECT(c->tess_cache_misses == 1); /* no second miss */
    EXPECT(frames_equal(first, second));

    /* ---- pixel_scale change must miss (flatten tolerance input) ---- */
    flux_canvas_set_scale(c, 2.0f);
    render(c, icon, &fill, false, second);
    EXPECT(c->tess_cache_hits == 1);   /* unchanged: no hit */
    EXPECT(c->tess_cache_misses == 2); /* scale is part of the key */
    EXPECT(c->tess_cache_stores == 2);
    EXPECT(!frames_equal(first, second)); /* scaled output really differs */
    render(c, icon, &fill, false, first);
    EXPECT(c->tess_cache_hits == 2); /* the scale-2 entry now hits */
    EXPECT(frames_equal(first, second));
    flux_canvas_set_scale(c, 1.0f);

    /* ---- Stroke: cached too; stroke_width is part of the key ---- */
    flux_paint stroke = flux_paint_solid(white);
    stroke.stroke_width = 3.0f;
    uint64_t stores_before = c->tess_cache_stores;
    render(c, icon, &stroke, true, first);
    EXPECT(c->tess_cache_stores == stores_before + 1);
    render(c, icon, &stroke, true, second);
    EXPECT(frames_equal(first, second));

    stroke.stroke_width = 6.0f;
    uint64_t hits_before = c->tess_cache_hits;
    render(c, icon, &stroke, true, second);
    EXPECT(c->tess_cache_hits == hits_before); /* width change missed */
    EXPECT(!frames_equal(first, second));      /* and the wider stroke shows */
    render(c, icon, &stroke, true, first);
    EXPECT(c->tess_cache_hits == hits_before + 1);
    EXPECT(frames_equal(first, second));

    /* ---- Stalled fill (self-intersecting): never cached, no pollution ---- */
    flux_path *star = build_stalled_star(&arena);
    uint64_t misses_b = c->tess_cache_misses, stores_b = c->tess_cache_stores;
    render(c, star, &fill, false, first);
    render(c, star, &fill, false, second);
    EXPECT(c->tess_cache_misses == misses_b + 2); /* looked up both times… */
    EXPECT(c->tess_cache_stores == stores_b);     /* …but never stored     */
    EXPECT(frames_equal(first, second));          /* deterministic output  */

    /* The icon entry survived the stalled fills and still hits. */
    hits_before = c->tess_cache_hits;
    render(c, icon, &fill, false, second);
    EXPECT(c->tess_cache_hits == hits_before + 1);
    stroke.stroke_width = 3.0f;
    render(c, icon, &stroke, true, first);
    render(c, icon, &stroke, true, second);
    EXPECT(frames_equal(first, second));

    /* ---- EVEN_ODD: bypasses the cache entirely ---- */
    flux_path *overlap = build_overlap(&arena);
    flux_paint eo = flux_paint_solid(white);
    eo.fill_rule = FLUX_FILL_EVEN_ODD;
    misses_b = c->tess_cache_misses;
    stores_b = c->tess_cache_stores;
    hits_before = c->tess_cache_hits;
    render(c, overlap, &eo, false, first);
    render(c, overlap, &eo, false, second);
    EXPECT(c->tess_cache_misses == misses_b); /* not even looked up */
    EXPECT(c->tess_cache_stores == stores_b);
    EXPECT(c->tess_cache_hits == hits_before);
    EXPECT(frames_equal(first, second)); /* deterministic fallback output */

    /* …and the cached entries render exactly as before afterwards. */
    render(c, icon, &fill, false, second);
    render(c, icon, &fill, false, first);
    EXPECT(frames_equal(first, second));
    EXPECT(c->tess_cache_hits == hits_before + 2);

    flux_canvas_destroy(c);
    flux_arena_destroy(&arena);
    TEST_SUMMARY();
}
