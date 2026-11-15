/* Observable contracts of the geometry x brush entry point (ADR-0088 / ADR-0091). */
#include "optics_test.h"
#include <flux/canvas_cpu.h>
#include <flux/canvas_helpers.h>

static const uint8_t *pixel(flux_canvas *c, unsigned x, unsigned y) {
    uint32_t stride;
    const uint8_t *p = flux_canvas_read_pixels(c, nullptr, nullptr, &stride);
    return p + y * stride + x * 4;
}

int main(void) {
    flux_canvas *c = nullptr;
    OT_EXPECT(flux_canvas_create_cpu(64, 64, 1, &c) == FLUX_OK);
    flux_brush gradient = {
        .kind = FLUX_BRUSH_LINEAR_GRADIENT,
        .blend = FLUX_BLEND_SRC_OVER,
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
    flux_geometry geoms[] = {
        flux_geom_rect((flux_rect){4, 4, 56, 56}),
        flux_geom_rrect((flux_rect){4, 4, 56, 56}, 12),
        flux_geom_circle(32, 32, 28),
    };
    for (unsigned i = 0; i < 3; i++) {
        OT_EXPECT(flux_canvas_begin(c, &(flux_canvas_pass_desc){.type = FLUX_TYPE_CANVAS_PASS_DESC,
                                                                .clear_color = &(flux_color){}}) ==
                  FLUX_OK);
        flux_canvas_draw_geometry(
            c, &(flux_canvas_draw_geometry_opts){.geom = &geoms[i], .brush = &gradient});
        OT_EXPECT(flux_canvas_end(c) == FLUX_OK);
        const uint8_t *left = pixel(c, 16, 32);
        OT_EXPECT(left[0] > left[2] && left[3] > 250);
        const uint8_t *right = pixel(c, 48, 32);
        OT_EXPECT(right[2] > right[0] && right[3] > 250);
        if (i)
            OT_EXPECT(pixel(c, 4, 4)[3] == 0);
    }
    /* Stroke width belongs to geometry. */
    flux_brush white = flux_brush_solid(0xffffffff);
    flux_geometry stroked_rect = flux_geom_rect((flux_rect){16, 16, 32, 32});
    stroked_rect.stroke.width = 8;
    OT_EXPECT(flux_canvas_begin(c, &(flux_canvas_pass_desc){.type = FLUX_TYPE_CANVAS_PASS_DESC,
                                                            .clear_color = &(flux_color){}}) ==
              FLUX_OK);
    flux_canvas_draw_geometry(
        c, &(flux_canvas_draw_geometry_opts){.geom = &stroked_rect, .brush = &white});
    OT_EXPECT(flux_canvas_end(c) == FLUX_OK);
    OT_EXPECT(pixel(c, 32, 32)[3] == 0);
    OT_EXPECT(pixel(c, 18, 32)[3] > 250);

    flux_geometry line = flux_geom_line(8, 32, 56, 32);
    line.stroke.width = 10;
    OT_EXPECT(flux_canvas_begin(c, &(flux_canvas_pass_desc){.type = FLUX_TYPE_CANVAS_PASS_DESC,
                                                            .clear_color = &(flux_color){}}) ==
              FLUX_OK);
    flux_canvas_draw_geometry(c,
                              &(flux_canvas_draw_geometry_opts){.geom = &line, .brush = &gradient});
    OT_EXPECT(flux_canvas_end(c) == FLUX_OK);
    OT_EXPECT(pixel(c, 32, 35)[3] > 250);
    OT_EXPECT(pixel(c, 32, 40)[3] == 0);

    /* Unsupported / invalid combinations poison the pass; the next pass recovers. */
    OT_EXPECT(flux_canvas_begin(c, &(flux_canvas_pass_desc){.type = FLUX_TYPE_CANVAS_PASS_DESC,
                                                            .clear_color = &(flux_color){}}) ==
              FLUX_OK);
    flux_canvas_draw_geometry(
        c, &(flux_canvas_draw_geometry_opts){.geom = &(flux_geometry){.kind = FLUX_GEOM_PATH},
                                             .brush = &white});
    OT_EXPECT(flux_canvas_end(c) == FLUX_ERROR_INVALID_ARGUMENT);
    OT_EXPECT(flux_canvas_begin(c, &(flux_canvas_pass_desc){.type = FLUX_TYPE_CANVAS_PASS_DESC,
                                                            .clear_color = &(flux_color){}}) ==
              FLUX_OK);
    flux_canvas_draw_geometry(
        c,
        &(flux_canvas_draw_geometry_opts){
            .geom = &(flux_geometry){.kind = FLUX_GEOM_LINE, .stroke.width = 0}, .brush = &white});
    OT_EXPECT(flux_canvas_end(c) == FLUX_ERROR_INVALID_ARGUMENT);
    OT_EXPECT(flux_canvas_begin(c, &(flux_canvas_pass_desc){.type = FLUX_TYPE_CANVAS_PASS_DESC,
                                                            .clear_color = &(flux_color){}}) ==
              FLUX_OK);
    flux_canvas_draw_geometry(
        c, &(flux_canvas_draw_geometry_opts){.geom = &geoms[1], .brush = &white});
    OT_EXPECT(flux_canvas_end(c) == FLUX_OK);
    OT_EXPECT(pixel(c, 32, 32)[3] > 250);
    flux_canvas_release(c);
    return OT_REPORT();
}
