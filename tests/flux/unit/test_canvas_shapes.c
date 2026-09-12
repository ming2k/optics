/* Observable contracts of the geometry x paint entry point. */
#include "../test_helpers.h"
#include <flux/canvas_cpu.h>

static const uint8_t *pixel(flux_canvas *c, unsigned x, unsigned y) {
    uint32_t stride;
    const uint8_t *p = flux_canvas_read_pixels(c, nullptr, nullptr, &stride);
    return p + y * stride + x * 4;
}

int main(void) {
    flux_canvas *c = nullptr;
    EXPECT(flux_canvas_create_cpu(64, 64, 1, &c) == FLUX_OK);
    flux_gradient_stop stops[] = {{0, 0xffff0000}, {1, 0xff0000ff}};
    flux_paint gradient =
        flux_paint_linear_gradient((flux_point){0, 0}, (flux_point){64, 0}, stops, 2);
    flux_shape shapes[] = {
        {.kind = FLUX_SHAPE_RECT, .rect = {4, 4, 56, 56}},
        {.kind = FLUX_SHAPE_RRECT, .rect = {4, 4, 56, 56}, .radius = 12},
        {.kind = FLUX_SHAPE_CIRCLE, .rect = {4, 4, 56, 56}},
    };
    for (unsigned i = 0; i < 3; i++) {
        EXPECT(flux_canvas_cpu_begin(c, nullptr) == FLUX_OK);
        flux_canvas_draw(c, &shapes[i], &gradient);
        EXPECT(flux_canvas_end_frame_checked(c) == FLUX_OK);
        const uint8_t *left = pixel(c, 16, 32);
        EXPECT(left[0] > left[2] && left[3] > 250);
        const uint8_t *right = pixel(c, 48, 32);
        EXPECT(right[2] > right[0] && right[3] > 250);
        if (i)
            EXPECT(pixel(c, 4, 4)[3] == 0);
    }
    /* Stroke width belongs to shape, irrespective of paint's old helper field. */
    flux_paint white = flux_paint_solid(0xffffffff);
    white.stroke_width = 1;
    EXPECT(flux_canvas_cpu_begin(c, nullptr) == FLUX_OK);
    flux_canvas_draw(
        c, &(flux_shape){.kind = FLUX_SHAPE_RECT, .rect = {16, 16, 32, 32}, .stroke_width = 8},
        &white);
    EXPECT(flux_canvas_end_frame_checked(c) == FLUX_OK);
    EXPECT(pixel(c, 32, 32)[3] == 0);
    EXPECT(pixel(c, 18, 32)[3] > 250);

    EXPECT(flux_canvas_cpu_begin(c, nullptr) == FLUX_OK);
    flux_canvas_draw(c,
                     &(flux_shape){.kind = FLUX_SHAPE_LINE,
                                   .rect = {8, 32, 48, 0},
                                   .stroke_width = 10,
                                   .stroke_cap = FLUX_CAP_ROUND},
                     &gradient);
    EXPECT(flux_canvas_end_frame_checked(c) == FLUX_OK);
    EXPECT(pixel(c, 32, 35)[3] > 250);
    EXPECT(pixel(c, 32, 40)[3] == 0);

    /* Unsupported combinations poison the pass; the next pass recovers. */
    EXPECT(flux_canvas_cpu_begin(c, nullptr) == FLUX_OK);
    flux_canvas_draw(c, &(flux_shape){.kind = FLUX_SHAPE_PATH}, &white);
    EXPECT(flux_canvas_end_frame_checked(c) == FLUX_ERROR_INVALID_ARGUMENT);
    EXPECT(flux_canvas_cpu_begin(c, nullptr) == FLUX_OK);
    flux_canvas_draw(c, &(flux_shape){.kind = FLUX_SHAPE_LINE}, &white);
    EXPECT(flux_canvas_end_frame_checked(c) == FLUX_ERROR_INVALID_ARGUMENT);
    EXPECT(flux_canvas_cpu_begin(c, nullptr) == FLUX_OK);
    flux_canvas_draw(c, &shapes[1], &white);
    EXPECT(flux_canvas_end_frame_checked(c) == FLUX_OK);
    EXPECT(pixel(c, 32, 32)[3] > 250);
    flux_canvas_destroy(c);
    TEST_SUMMARY();
}
