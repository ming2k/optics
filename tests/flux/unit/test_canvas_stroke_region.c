/*
 * Stroke coverage oracle.
 *
 * The stroker emits its band as overlapping strip geometry. Where the band
 * overlaps itself the pixels are blended twice — invisible for opaque paint,
 * a darker seam under translucency. The case is "retrograde": offsetting
 * toward the centre of curvature by more than the local radius of curvature
 * makes the inner boundary turn back through the centre and self-overlap.
 *
 * The oracle is analytic and self-calibrating rather than golden-image based.
 * It draws a solid fill of the *same paint* as a single-coverage reference, so
 * no assumption is made about the colour pipeline (the readback applies a
 * linear->sRGB transform, so the raw byte value is not predictable a priori).
 * Every pixel inside the stroke's own region, away from the anti-aliased
 * boundary, must then equal that reference. A doubly-blended pixel is far
 * outside the tolerance; with a 50%-alpha white over opaque black the reference
 * is 188 and a double blend is 255.
 */
#include "test_helpers.h"

#include <flux/canvas_cpu.h>
#include <flux/flux.h>

#include <math.h>
#include <string.h>

#define W 128
#define H 128

/* Self-calibrated: with this paint the single-coverage reference reads 188 and
 * a fully double-blended pixel reads 255 (deviation 67). The clamp removes
 * whole-pixel double blending; a sub-supersample residual (~1 of 4 samples on
 * a handful of pixels, deviation ~15) remains in the extreme retrograde case,
 * so the threshold sits between the two. */
#define TOL 20
/* Pixels closer than this to the analytic boundary are anti-aliased and
 * excluded: it covers the flattening tolerance (0.25 px), the supersample
 * footprint (0.25 px) and the polygon-vs-arc difference. */
#define EDGE_MARGIN 2.0f

static uint8_t ink_at(const uint8_t *fb, int x, int y) {
    return fb[(y * W + x) * 4 + 0];
}

/* Stroke a centred circle with translucent white; draw a solid fill of the same
 * paint as the single-coverage reference. */
static const uint8_t *render_circle_stroke(flux_arena *arena, flux_canvas **out_c, float radius,
                                           float width, int *ref_out) {
    flux_canvas *c = nullptr;
    if (flux_canvas_create_cpu(W, H, 1.0f, &c) != FLUX_OK)
        return nullptr;

    flux_path *p = nullptr;
    if (flux_path_create(&p, arena) != FLUX_OK) {
        flux_canvas_release(c);
        return nullptr;
    }
    flux_path_add_circle(p, (float)W * 0.5f, (float)H * 0.5f, radius);

    flux_brush brush = flux_brush_solid(flux_color_rgba_premul(255, 255, 255, 128));
    flux_stroke_style style = {.width = width};
    flux_color clear = flux_color_rgba_premul(0, 0, 0, 255);

    if (flux_canvas_begin(c, &(flux_canvas_pass_desc){.type = FLUX_TYPE_CANVAS_PASS_DESC,
                                                      .clear_color = &clear}) != FLUX_OK) {
        flux_canvas_release(c);
        return nullptr;
    }
    flux_canvas_fill_rect(c, (flux_rect){4.0f, 4.0f, 20.0f, 20.0f}, &brush);
    flux_canvas_stroke_path(c, p, &style, &brush);
    if (flux_canvas_end(c) != FLUX_OK) {
        flux_canvas_release(c);
        return nullptr;
    }
    *out_c = c;
    const uint8_t *fb = flux_canvas_cpu_pixels(c, nullptr, nullptr, nullptr);
    if (fb)
        *ref_out = ink_at(fb, 10, 10);
    return fb;
}

/* Worst |ink - reference| over pixels strictly inside the band
 * [inner_r + MARGIN, outer_r - MARGIN]. */
static int probe_band(const uint8_t *fb, int ref, float inner_r, float outer_r, int *probed) {
    int worst = 0;
    float cx = (float)W * 0.5f, cy = (float)H * 0.5f;
    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            float px = (float)x + 0.5f, py = (float)y + 0.5f;
            float d = sqrtf((px - cx) * (px - cx) + (py - cy) * (py - cy));
            if (d < inner_r + EDGE_MARGIN || d > outer_r - EDGE_MARGIN)
                continue;
            (*probed)++;
            int dev = (int)ink_at(fb, x, y) - ref;
            if (dev < 0)
                dev = -dev;
            if (dev > worst)
                worst = dev;
        }
    }
    return worst;
}

static void check_annulus(float radius, float half, int min_probed, const char *label) {
    flux_arena arena;
    EXPECT(flux_arena_init(&arena, 1u << 16, nullptr) == FLUX_OK);
    flux_canvas *c = nullptr;
    int ref = -1;
    const uint8_t *fb = render_circle_stroke(&arena, &c, radius, half * 2.0f, &ref);
    EXPECT(fb != nullptr);
    if (fb) {
        EXPECT(ref > 0);
        int probed = 0;
        int worst = probe_band(fb, ref, radius - half, radius + half, &probed);
        EXPECT(probed > min_probed);
        EXPECT(worst <= TOL);
        flux_canvas_release(c);
    }
    flux_arena_deinit(&arena);
}

static void check_disc(float radius, float half, int min_probed, const char *label) {
    flux_arena arena;
    EXPECT(flux_arena_init(&arena, 1u << 16, nullptr) == FLUX_OK);
    flux_canvas *c = nullptr;
    int ref = -1;
    const uint8_t *fb = render_circle_stroke(&arena, &c, radius, half * 2.0f, &ref);
    EXPECT(fb != nullptr);
    if (fb) {
        EXPECT(ref > 0);
        int probed = 0;
        int worst = probe_band(fb, ref, 0.0f, radius + half, &probed);
        EXPECT(probed > min_probed);
        EXPECT(worst <= TOL);
        flux_canvas_release(c);
    }
    flux_arena_deinit(&arena);
}

int main(void) {
    /* Ordinary strokes: half stays inside the radius of curvature, so no part
     * of the band retrogrades. These must hold before and after any stroker
     * change — they are the regression net. */
    check_annulus(30.0f, 10.0f, 400, "annulus r=30 half=10");
    check_annulus(20.0f, 6.0f, 400, "annulus r=20 half=6");
    check_annulus(40.0f, 4.0f, 400, "annulus r=40 half=4");

    /* Retrograde: half exceeds the radius, so the inner offset passes through
     * the centre and doubles back. The stroke covers the disc of radius
     * radius+half and every pixel in it must still carry exactly ONE coverage.
     * Overlapping band geometry shows up here as double blending. */
    check_disc(6.0f, 10.0f, 300, "disc r=6 half=10 (retrograde)");
    check_disc(2.0f, 12.0f, 300, "disc r=2 half=12 (retrograde)");

    TEST_SUMMARY();
}
