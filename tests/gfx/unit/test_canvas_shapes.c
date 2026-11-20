/* Observable contracts of the geometry x brush entry point (ADR-0088 / ADR-0091). */
#include "optics_test.h"
#include <optics/gfx/canvas_cpu.h>
#include <optics/gfx/canvas_helpers.h>

static const uint8_t *pixel(optics_gfx_canvas *c, unsigned x, unsigned y) {
    uint32_t stride;
    const uint8_t *p = optics_gfx_canvas_read_pixels(c, nullptr, nullptr, &stride);
    return p + y * stride + x * 4;
}

int main(void) {
    optics_gfx_canvas *c = nullptr;
    OT_EXPECT(
        optics_gfx_canvas_create(&(optics_gfx_canvas_desc){.type = OPTICS_GFX_TYPE_CANVAS_DESC,
                                                           .backend = OPTICS_GFX_CANVAS_BACKEND_CPU,
                                                           .scale = 1,
                                                           .width = 64,
                                                           .height = 64},
                                 &c) == OPTICS_GFX_OK);
    optics_gfx_brush gradient = {
        .kind = OPTICS_GFX_BRUSH_LINEAR_GRADIENT,
        .blend = OPTICS_GFX_BLEND_SRC_OVER,
        .opacity = 1.0f,
        .gradient =
            {
                .start = {0, 0},
                .end = {64, 0},
                .stops =
                    {
                        .stops = {{0.0f, 0xffff0000}, {1.0f, 0xff0000ff}},
                        .count = 2,
                    },
            },
    };
    optics_gfx_geometry geoms[] = {
        optics_gfx_geom_rect((optics_gfx_rect){4, 4, 56, 56}),
        optics_gfx_geom_rrect((optics_gfx_rect){4, 4, 56, 56}, 12),
        optics_gfx_geom_circle(32, 32, 28),
    };
    for (unsigned i = 0; i < 3; i++) {
        OT_EXPECT(optics_gfx_canvas_begin(
                      c, &(optics_gfx_canvas_pass_desc){.type = OPTICS_GFX_TYPE_CANVAS_PASS_DESC,
                                                        .clear_color = &(optics_gfx_color){}}) ==
                  OPTICS_GFX_OK);
        optics_gfx_canvas_draw_geometry(
            c, &(optics_gfx_canvas_draw_geometry_opts){.geom = &geoms[i], .brush = &gradient});
        OT_EXPECT(optics_gfx_canvas_end(c) == OPTICS_GFX_OK);
        const uint8_t *left = pixel(c, 16, 32);
        OT_EXPECT(left[0] > left[2] && left[3] > 250);
        const uint8_t *right = pixel(c, 48, 32);
        OT_EXPECT(right[2] > right[0] && right[3] > 250);
        if (i)
            OT_EXPECT(pixel(c, 4, 4)[3] == 0);
    }
    /* Stroke width belongs to geometry. */
    optics_gfx_brush white = optics_gfx_brush_solid(0xffffffff);
    optics_gfx_geometry stroked_rect = optics_gfx_geom_rect((optics_gfx_rect){16, 16, 32, 32});
    stroked_rect.stroke.width = 8;
    OT_EXPECT(optics_gfx_canvas_begin(
                  c, &(optics_gfx_canvas_pass_desc){.type = OPTICS_GFX_TYPE_CANVAS_PASS_DESC,
                                                    .clear_color = &(optics_gfx_color){}}) ==
              OPTICS_GFX_OK);
    optics_gfx_canvas_draw_geometry(
        c, &(optics_gfx_canvas_draw_geometry_opts){.geom = &stroked_rect, .brush = &white});
    OT_EXPECT(optics_gfx_canvas_end(c) == OPTICS_GFX_OK);
    OT_EXPECT(pixel(c, 32, 32)[3] == 0);
    OT_EXPECT(pixel(c, 18, 32)[3] > 250);

    optics_gfx_geometry line = optics_gfx_geom_line(8, 32, 56, 32);
    line.stroke.width = 10;
    OT_EXPECT(optics_gfx_canvas_begin(
                  c, &(optics_gfx_canvas_pass_desc){.type = OPTICS_GFX_TYPE_CANVAS_PASS_DESC,
                                                    .clear_color = &(optics_gfx_color){}}) ==
              OPTICS_GFX_OK);
    optics_gfx_canvas_draw_geometry(
        c, &(optics_gfx_canvas_draw_geometry_opts){.geom = &line, .brush = &gradient});
    OT_EXPECT(optics_gfx_canvas_end(c) == OPTICS_GFX_OK);
    OT_EXPECT(pixel(c, 32, 35)[3] > 250);
    OT_EXPECT(pixel(c, 32, 40)[3] == 0);

    /* Unsupported / invalid combinations poison the pass; the next pass recovers. */
    OT_EXPECT(optics_gfx_canvas_begin(
                  c, &(optics_gfx_canvas_pass_desc){.type = OPTICS_GFX_TYPE_CANVAS_PASS_DESC,
                                                    .clear_color = &(optics_gfx_color){}}) ==
              OPTICS_GFX_OK);
    optics_gfx_canvas_draw_geometry(
        c, &(optics_gfx_canvas_draw_geometry_opts){
               .geom = &(optics_gfx_geometry){.kind = OPTICS_GFX_GEOM_PATH}, .brush = &white});
    OT_EXPECT(optics_gfx_canvas_end(c) == OPTICS_GFX_ERROR_INVALID_ARGUMENT);
    OT_EXPECT(optics_gfx_canvas_begin(
                  c, &(optics_gfx_canvas_pass_desc){.type = OPTICS_GFX_TYPE_CANVAS_PASS_DESC,
                                                    .clear_color = &(optics_gfx_color){}}) ==
              OPTICS_GFX_OK);
    optics_gfx_canvas_draw_geometry(
        c, &(optics_gfx_canvas_draw_geometry_opts){
               .geom = &(optics_gfx_geometry){.kind = OPTICS_GFX_GEOM_LINE, .stroke.width = 0},
               .brush = &white});
    OT_EXPECT(optics_gfx_canvas_end(c) == OPTICS_GFX_ERROR_INVALID_ARGUMENT);
    OT_EXPECT(optics_gfx_canvas_begin(
                  c, &(optics_gfx_canvas_pass_desc){.type = OPTICS_GFX_TYPE_CANVAS_PASS_DESC,
                                                    .clear_color = &(optics_gfx_color){}}) ==
              OPTICS_GFX_OK);
    optics_gfx_canvas_draw_geometry(
        c, &(optics_gfx_canvas_draw_geometry_opts){.geom = &geoms[1], .brush = &white});
    OT_EXPECT(optics_gfx_canvas_end(c) == OPTICS_GFX_OK);
    OT_EXPECT(pixel(c, 32, 32)[3] > 250);
    optics_gfx_canvas_release(c);
    return OT_REPORT();
}
