/* Observable contracts of the geometry x brush entry point (ADR-0088 / ADR-0091). */
#include "../test_helpers.h"
#include <flux/canvas_cpu.h>
#include <flux/canvas_helpers.h>

static const uint8_t *pixel(flux_canvas *c, unsigned x, unsigned y) {
    uint32_t stride;
    const uint8_t *p = flux_canvas_read_pixels(c, nullptr, nullptr, &stride);
    return p + y * stride + x * 4;
}

int main(void) {
    flux_canvas *c = nullptr;
    EXPECT(flux_canvas_create_cpu(64, 64, 1, &c) == FLUX_OK);
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
        EXPECT(flux_canvas_cpu_begin(c, nullptr) == FLUX_OK);
        flux_canvas_draw_geometry(c, &geoms[i], &gradient);
        EXPECT(flux_canvas_end_frame_checked(c) == FLUX_OK);
        const uint8_t *left = pixel(c, 16, 32);
        EXPECT(left[0] > left[2] && left[3] > 250);
        const uint8_t *right = pixel(c, 48, 32);
        EXPECT(right[2] > right[0] && right[3] > 250);
        if (i)
            EXPECT(pixel(c, 4, 4)[3] == 0);
    }
    /* Stroke width belongs to geometry. */
    flux_brush white = flux_brush_solid(0xffffffff);
    flux_geometry stroked_rect = flux_geom_rect((flux_rect){16, 16, 32, 32});
    stroked_rect.stroke_width = 8;
    EXPECT(flux_canvas_cpu_begin(c, nullptr) == FLUX_OK);
    flux_canvas_draw_geometry(c, &stroked_rect, &white);
    EXPECT(flux_canvas_end_frame_checked(c) == FLUX_OK);
    EXPECT(pixel(c, 32, 32)[3] == 0);
    EXPECT(pixel(c, 18, 32)[3] > 250);

    flux_geometry line = flux_geom_line(8, 32, 56, 32);
    line.stroke_width = 10;
    EXPECT(flux_canvas_cpu_begin(c, nullptr) == FLUX_OK);
    flux_canvas_draw_geometry(c, &line, &gradient);
    EXPECT(flux_canvas_end_frame_checked(c) == FLUX_OK);
    EXPECT(pixel(c, 32, 35)[3] > 250);
    EXPECT(pixel(c, 32, 40)[3] == 0);

    /* Unsupported / invalid combinations poison the pass; the next pass recovers. */
    EXPECT(flux_canvas_cpu_begin(c, nullptr) == FLUX_OK);
    flux_canvas_draw_geometry(c, &(flux_geometry){.kind = FLUX_GEOM_PATH}, &white);
    EXPECT(flux_canvas_end_frame_checked(c) == FLUX_ERROR_INVALID_ARGUMENT);
    EXPECT(flux_canvas_cpu_begin(c, nullptr) == FLUX_OK);
    flux_canvas_draw_geometry(c, &(flux_geometry){.kind = FLUX_GEOM_LINE, .stroke_width = 0},
                              &white);
    EXPECT(flux_canvas_end_frame_checked(c) == FLUX_ERROR_INVALID_ARGUMENT);
    EXPECT(flux_canvas_cpu_begin(c, nullptr) == FLUX_OK);
    flux_canvas_draw_geometry(c, &geoms[1], &white);
    EXPECT(flux_canvas_end_frame_checked(c) == FLUX_OK);
    EXPECT(pixel(c, 32, 32)[3] > 250);
    flux_canvas_release(c);
    TEST_SUMMARY();
}
