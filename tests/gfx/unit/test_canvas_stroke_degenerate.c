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
#include "../../../libs/gfx/src/canvas/internal.h"
#include "optics_test.h"
#include <optics/gfx/canvas_cpu.h>

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

    /* Circle centred (36,48) r=24 as four cubics, a trailing zero-length
     * cubic, close; then the "handle" as a separate subpath at (64..84). */
    optics_gfx_path *p = nullptr;
    OT_EXPECT(optics_gfx_path_create(&p, &arena) == OPTICS_GFX_OK);
    const float k = 24.0f * 0.5522847498f; /* circle-to-cubic constant */
    optics_gfx_path_move_to(p, 60.0f, 48.0f);
    optics_gfx_path_cubic_to(p, 60.0f, 48.0f + k, 36.0f + k, 72.0f, 36.0f, 72.0f);
    optics_gfx_path_cubic_to(p, 36.0f - k, 72.0f, 12.0f, 48.0f + k, 12.0f, 48.0f);
    optics_gfx_path_cubic_to(p, 12.0f, 48.0f - k, 36.0f - k, 24.0f, 36.0f, 24.0f);
    optics_gfx_path_cubic_to(p, 36.0f + k, 24.0f, 60.0f, 48.0f - k, 60.0f, 48.0f);
    optics_gfx_path_cubic_to(p, 60.0f, 48.0f, 60.0f, 48.0f, 60.0f, 48.0f); /* degenerate */
    optics_gfx_path_close(p);
    optics_gfx_path_move_to(p, 84.0f, 84.0f);
    optics_gfx_path_cubic_to(p, 76.0f, 76.0f, 70.0f, 70.0f, 64.0f, 64.0f);

    optics_gfx_color white = optics_gfx_color_rgba_premul(255, 255, 255, 255);
    optics_gfx_brush stroke = optics_gfx_brush_solid(white);
    optics_gfx_stroke_style stroke_stroke = {.width = 3.0f};

    optics_gfx_color clear = optics_gfx_color_rgba_premul(0, 0, 0, 255);
    OT_EXPECT(optics_gfx_canvas_begin(
                  c, &(optics_gfx_canvas_pass_desc){.type = OPTICS_GFX_TYPE_CANVAS_PASS_DESC,
                                                    .clear_color = &clear}) == OPTICS_GFX_OK);
    optics_gfx_canvas_stroke_path(c, p, &stroke_stroke, &stroke);
    OT_EXPECT(optics_gfx_canvas_end(c) == OPTICS_GFX_OK);

    uint32_t w = 0, h = 0, stride = 0;
    const uint8_t *fb = optics_gfx_canvas_cpu_pixels(c, &w, &h, &stride);
    OT_EXPECT(fb != nullptr && w == W && h == H && stride == W * 4);
    OT_EXPECT(stride == (uint32_t)W * 4);

    OT_EXPECT(any_ink(fb, 34, 22, 39, 27));  /* circle top edge        */
    OT_EXPECT(any_ink(fb, 70, 70, 80, 80));  /* handle diagonal        */
    OT_EXPECT(!any_ink(fb, 44, 44, 49, 49)); /* circle interior empty  */

    optics_gfx_canvas_release(c);
    optics_gfx_arena_deinit(&arena);
    return OT_REPORT();
}
