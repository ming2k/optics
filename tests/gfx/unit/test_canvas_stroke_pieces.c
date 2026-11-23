/* Stroke outline pieces (pixel level, CPU canvas).
 *
 * The stroker emits each segment body, join and cap as one positively
 * oriented convex contour and fills their non-zero union. Two properties
 * follow, and each has a way to fail:
 *
 *   - Exactly one coverage: where a join or cap overlaps the bodies beside it,
 *     a translucent stroke must not double-blend. Fails if a piece is emitted
 *     with the wrong orientation or the union is not taken.
 *   - No holes: at a vertex whose miter overflows, each body must end on its
 *     own segment's normal (offsets taken from the other segment twist the
 *     quad past a 90-degree turn, which once drew half the band). A wide
 *     stroke on a tight zigzag also crosses its inner offsets, turning a body
 *     quad into a bowtie whose lobes wind in opposite directions; filled as
 *     one contour it would cancel itself, so it must be split. Either failure
 *     shows as uncovered pixels on the centreline.
 *
 * Paint is premultiplied white at alpha 128 on a transparent clear, so a
 * pixel's alpha reads its coverage: 128 inside the band, ~192 where two
 * coverages stack, 0 in a hole.
 */
#include "optics_test.h"
#include <optics/gfx/canvas.h>
#include <optics/gfx/canvas_cpu.h>

#include <math.h>
#include <stdint.h>
#include <stdlib.h>

#define W 96
#define H 96
#define INK 128
#define TOL 2

typedef struct pt {
    float x, y;
} pt;

static const uint8_t *stroke_polyline(optics_gfx_canvas *c, optics_gfx_arena *arena, const pt *v,
                                      int n, const optics_gfx_stroke_style *style) {
    optics_gfx_path *p = nullptr;
    OT_EXPECT(optics_gfx_path_create(&p, arena) == OPTICS_GFX_OK);
    optics_gfx_path_move_to(p, v[0].x, v[0].y);
    for (int i = 1; i < n; ++i)
        optics_gfx_path_line_to(p, v[i].x, v[i].y);

    optics_gfx_color clear = optics_gfx_color_rgba_premul(0, 0, 0, 0);
    optics_gfx_brush brush =
        optics_gfx_brush_solid(optics_gfx_color_rgba_premul(INK, INK, INK, INK));
    OT_EXPECT(optics_gfx_canvas_begin(
                  c, &(optics_gfx_canvas_pass_desc){.type = OPTICS_GFX_TYPE_CANVAS_PASS_DESC,
                                                    .clear_color = &clear}) == OPTICS_GFX_OK);
    optics_gfx_canvas_stroke_path(c, p, style, &brush);
    OT_EXPECT(optics_gfx_canvas_end(c) == OPTICS_GFX_OK);
    optics_gfx_arena_reset(arena);

    uint32_t w = 0, h = 0, stride = 0;
    const uint8_t *fb = optics_gfx_canvas_cpu_pixels(c, &w, &h, &stride);
    OT_EXPECT(fb != nullptr && w == W && h == H && stride == W * 4);
    return fb;
}

static int alpha_at(const uint8_t *fb, int x, int y) {
    return fb[(y * W + x) * 4 + 3];
}

/* No pixel anywhere carries more than one coverage. */
static int max_alpha(const uint8_t *fb) {
    int m = 0;
    for (int i = 0; i < W * H; ++i)
        if (fb[i * 4 + 3] > m)
            m = fb[i * 4 + 3];
    return m;
}

/* Every pixel the centreline passes through is fully inside the band. The two
 * open ends are skipped: a butt cap ends the band on the endpoint itself. */
static int centreline_holes(const uint8_t *fb, const pt *v, int n) {
    int holes = 0;
    for (int s = 0; s + 1 < n; ++s) {
        const int k0 = s == 0 ? 1 : 0;
        const int k1 = s + 2 == n ? 31 : 32;
        for (int k = k0; k <= k1; ++k) {
            const float t = (float)k / 32.0f;
            const int x = (int)floorf(v[s].x + (v[s + 1].x - v[s].x) * t);
            const int y = (int)floorf(v[s].y + (v[s + 1].y - v[s].y) * t);
            if (abs(alpha_at(fb, x, y) - INK) > TOL)
                holes++;
        }
    }
    return holes;
}

int main(void) {
    optics_gfx_arena arena;
    OT_EXPECT(optics_gfx_arena_init(&arena, 1u << 16, nullptr) == OPTICS_GFX_OK);
    optics_gfx_canvas *c = nullptr;
    OT_EXPECT(
        optics_gfx_canvas_create(&(optics_gfx_canvas_desc){.type = OPTICS_GFX_TYPE_CANVAS_DESC,
                                                           .backend = OPTICS_GFX_CANVAS_BACKEND_CPU,
                                                           .scale = 1.0f,
                                                           .width = W,
                                                           .height = H},
                                 &c) == OPTICS_GFX_OK);
    OT_EXPECT(c != nullptr);

    /* A Z with two acute corners: every join overflows a low miter limit and
     * overlaps both bodies; the caps overlap the end bodies. */
    const pt zed[] = {{16.5f, 16.5f}, {80.5f, 16.5f}, {16.5f, 56.5f}, {80.5f, 80.5f}};
    const optics_gfx_line_join joins[] = {OPTICS_GFX_JOIN_MITER, OPTICS_GFX_JOIN_BEVEL,
                                          OPTICS_GFX_JOIN_ROUND};
    const optics_gfx_line_cap caps[] = {OPTICS_GFX_CAP_BUTT, OPTICS_GFX_CAP_SQUARE,
                                        OPTICS_GFX_CAP_ROUND};
    for (int j = 0; j < 3; ++j) {
        for (int k = 0; k < 3; ++k) {
            const optics_gfx_stroke_style style = {
                .width = 9.0f, .miter_limit = 2.0f, .join = joins[j], .cap = caps[k]};
            const uint8_t *fb = stroke_polyline(c, &arena, zed, 4, &style);
            OT_EXPECT(max_alpha(fb) <= INK + TOL);             /* no double blend anywhere */
            OT_EXPECT(abs(alpha_at(fb, 48, 16) - INK) <= TOL); /* body interior */
            OT_EXPECT(centreline_holes(fb, zed, 4) == 0);
        }
    }

    /* Tight zigzag, much wider than its segments are long: the inner offsets
     * cross at every vertex, which is where a bowtie quad would cut holes. */
    const pt zigzag[] = {{14.5f, 40.5f}, {24.5f, 56.5f}, {34.5f, 40.5f}, {44.5f, 56.5f},
                         {54.5f, 40.5f}, {64.5f, 56.5f}, {74.5f, 40.5f}};
    for (int j = 0; j < 3; ++j) {
        const optics_gfx_stroke_style style = {
            .width = 22.0f, .miter_limit = 4.0f, .join = joins[j], .cap = OPTICS_GFX_CAP_BUTT};
        const uint8_t *fb = stroke_polyline(c, &arena, zigzag, 7, &style);
        OT_EXPECT(centreline_holes(fb, zigzag, 7) == 0);
        OT_EXPECT(max_alpha(fb) <= INK + TOL);
    }

    optics_gfx_canvas_release(c);
    optics_gfx_arena_deinit(&arena);
    return OT_REPORT();
}
