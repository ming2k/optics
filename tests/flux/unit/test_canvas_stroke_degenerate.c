/* Degenerate-cubic stroke regression (pixel level).
 *
 * A zero-length cubic (all four anchors coincident — nanosvg emits one
 * after converting <circle> elements, so every circle-first runtime SVG
 * icon carried it) once made the flattener recurse to its depth cap and
 * flood the path scratch buffer with 2^16 duplicate points. Every contour
 * after the degenerate segment was dropped: a settings gear stroked as
 * just its hub circle.
 *
 * This strokes a circle-with-degenerate-tail plus a separate handle
 * subpath through the CPU canvas and asserts ink lands in BOTH regions.
 */
#include "../../../libs/flux/src/canvas/internal.h"
#include "test_helpers.h"
#include <flux/canvas_cpu.h>

#include <string.h>

#define W 96
#define H 96

static bool any_ink(const uint8_t *fb, int x0, int y0, int x1, int y1) {
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++)
            if (fb[(y * W + x) * 4] != 0) /* white ink on the black clear */
                return true;
    return false;
}

int main(void) {
    flux_arena arena;
    EXPECT(flux_arena_init(&arena, 1u << 16, nullptr) == FLUX_OK);

    flux_canvas *c = nullptr;
    EXPECT(flux_canvas_create_cpu(W, H, 1.0f, &c) == FLUX_OK);
    EXPECT(c != nullptr);

    /* Circle centred (36,48) r=24 as four cubics, a trailing zero-length
     * cubic, close; then the "handle" as a separate subpath at (64..84). */
    flux_path *p = nullptr;
    EXPECT(flux_path_create(&p, &arena) == FLUX_OK);
    const float k = 24.0f * 0.5522847498f; /* circle-to-cubic constant */
    flux_path_move_to(p, 60.0f, 48.0f);
    flux_path_cubic_to(p, 60.0f, 48.0f + k, 36.0f + k, 72.0f, 36.0f, 72.0f);
    flux_path_cubic_to(p, 36.0f - k, 72.0f, 12.0f, 48.0f + k, 12.0f, 48.0f);
    flux_path_cubic_to(p, 12.0f, 48.0f - k, 36.0f - k, 24.0f, 36.0f, 24.0f);
    flux_path_cubic_to(p, 36.0f + k, 24.0f, 60.0f, 48.0f - k, 60.0f, 48.0f);
    flux_path_cubic_to(p, 60.0f, 48.0f, 60.0f, 48.0f, 60.0f, 48.0f); /* degenerate */
    flux_path_close(p);
    flux_path_move_to(p, 84.0f, 84.0f);
    flux_path_cubic_to(p, 76.0f, 76.0f, 70.0f, 70.0f, 64.0f, 64.0f);

    flux_color white = flux_color_rgba_premul(255, 255, 255, 255);
    flux_paint stroke = flux_paint_solid(white);
    stroke.stroke_width = 3.0f;

    flux_color clear = flux_color_rgba_premul(0, 0, 0, 255);
    EXPECT(flux_canvas_cpu_begin(c, &clear) == FLUX_OK);
    flux_canvas_stroke_path(c, p, &stroke);
    flux_canvas_cpu_end(c);

    uint32_t w = 0, h = 0, stride = 0;
    const uint8_t *fb = flux_canvas_cpu_pixels(c, &w, &h, &stride);
    EXPECT(fb != nullptr && w == W && h == H && stride == W * 4);
    EXPECT(stride == (uint32_t)W * 4);

    EXPECT(any_ink(fb, 34, 22, 39, 27));   /* circle top edge        */
    EXPECT(any_ink(fb, 70, 70, 80, 80));   /* handle diagonal        */
    EXPECT(!any_ink(fb, 44, 44, 49, 49));  /* circle interior empty  */

    flux_arena_destroy(&arena);
    TEST_SUMMARY();
}
