/* Observable clean-break contracts (ADR-0095). */
#include "test_helpers.h"
#include <flux/canvas_cpu.h>
#include <math.h>
#include <string.h>

static flux_result begin_target(flux_canvas *c, flux_target *target, const flux_color *clear) {
    flux_canvas_pass_desc pass = FLUX_CANVAS_PASS_DESC_INIT;
    pass.attachment =
        (flux_canvas_attachment){.kind = FLUX_CANVAS_ATTACHMENT_TARGET, .target = target};
    pass.clear_color = clear;
    return flux_canvas_begin(c, &pass);
}

int main(void) {
    flux_canvas *c = nullptr;
    EXPECT(flux_canvas_create_cpu(8, 8, 1, &c) == FLUX_OK);
    flux_cpu_target_desc desc = FLUX_CPU_TARGET_DESC_INIT;
    desc.width = 32;
    desc.height = 24;
    flux_target *a = nullptr, *b = nullptr;
    EXPECT(flux_target_create_cpu(&desc, &a) == FLUX_OK);
    EXPECT(flux_target_create_cpu(&desc, &b) == FLUX_OK);
    flux_color red = 0xffff0000u, blue = 0xff0000ffu;
    EXPECT(begin_target(c, a, &red) == FLUX_OK);
    EXPECT(flux_canvas_end(c) == FLUX_OK);
    EXPECT(begin_target(c, b, &blue) == FLUX_OK);
    EXPECT(flux_canvas_end(c) == FLUX_OK);
    /* Loading A after B preserves A; drawing uses A's extent, not 8x8. */
    EXPECT(begin_target(c, a, nullptr) == FLUX_OK);
    flux_canvas_fill_rect_color(c, (flux_rect){20, 16, 8, 6}, 0xff00ff00u);
    EXPECT(flux_canvas_end(c) == FLUX_OK);
    uint32_t width, height, stride;
    const uint8_t *px = flux_target_cpu_pixels(a, &width, &height, &stride);
    EXPECT(width == 32 && height == 24);
    EXPECT(memcmp(px, (uint8_t[]){255, 0, 0, 255}, 4) == 0);
    EXPECT(memcmp(px + 18 * stride + 22 * 4, (uint8_t[]){0, 255, 0, 255}, 4) == 0);
    px = flux_target_cpu_pixels(b, nullptr, nullptr, nullptr);
    EXPECT(memcmp(px, (uint8_t[]){0, 0, 255, 255}, 4) == 0);
    /* Dirty clears preserve the destination's pixels outside the region. */
    flux_canvas_pass_desc pass = FLUX_CANVAS_PASS_DESC_INIT;
    pass.attachment = (flux_canvas_attachment){.kind = FLUX_CANVAS_ATTACHMENT_TARGET, .target = a};
    pass.clear_color = &blue;
    pass.render_width = 2;
    pass.render_height = 2;
    EXPECT(flux_canvas_begin(c, &pass) == FLUX_OK);
    EXPECT(flux_canvas_end(c) == FLUX_OK);
    px = flux_target_cpu_pixels(a, nullptr, nullptr, nullptr);
    EXPECT(memcmp(px, (uint8_t[]){0, 0, 255, 255}, 4) == 0);
    EXPECT(memcmp(px + 3 * stride + 3 * 4, (uint8_t[]){255, 0, 0, 255}, 4) == 0);
    /* The pass owns its destination reference until close. */
    EXPECT(begin_target(c, a, &red) == FLUX_OK);
    flux_target_release(a);
    a = nullptr;
    EXPECT(flux_canvas_end(c) == FLUX_OK);
    desc.stride_bytes = 4;
    flux_target *bad = b;
    EXPECT(flux_target_create_cpu(&desc, &bad) == FLUX_ERROR_OUT_OF_RANGE);
    EXPECT(bad == nullptr);

    /* Invalid brush inputs fail consistently for encoding and immediate draws. */
    flux_geometry rect = flux_geom_rect((flux_rect){0, 0, 4, 4});
    flux_brush brush = flux_brush_solid(red);
    brush.opacity = NAN;
    EXPECT(flux_canvas_begin(c, nullptr) == FLUX_OK);
    flux_canvas_draw_geometry(c, &rect, &brush);
    EXPECT(flux_canvas_end(c) == FLUX_ERROR_INVALID_ARGUMENT);
    flux_encoder *encoder = nullptr;
    EXPECT(flux_encoder_create(nullptr, &encoder) == FLUX_OK);
    flux_encoder_draw_geometry(encoder, &rect, &brush);
    flux_display_list *list = nullptr;
    EXPECT(flux_encoder_finish(encoder, &list) == FLUX_ERROR_INVALID_ARGUMENT);
    EXPECT(list == nullptr);
    flux_encoder_destroy(encoder);
    /* Stack errors close and reset the pass, including unclosed layers. */
    EXPECT(flux_canvas_begin(c, nullptr) == FLUX_OK);
    flux_canvas_restore(c);
    EXPECT(flux_canvas_end(c) == FLUX_ERROR_INVALID_STATE);
    EXPECT(flux_canvas_begin(c, nullptr) == FLUX_OK);
    flux_canvas_save_layer(c, nullptr, 0.5f);
    EXPECT(flux_canvas_end(c) == FLUX_ERROR_INVALID_STATE);
    EXPECT(flux_canvas_begin(c, nullptr) == FLUX_OK);
    EXPECT(flux_canvas_end(c) == FLUX_OK);
    EXPECT(flux_canvas_end(c) == FLUX_ERROR_INVALID_STATE);

    /* Cap topology survives geometry -> backend conversion. */
    flux_geometry line = flux_geom_line(8, 12, 24, 12);
    line.stroke.width = 6;
    brush = flux_brush_solid(0xffffffffu);
    flux_color clear = 0;
    EXPECT(begin_target(c, b, &clear) == FLUX_OK);
    flux_canvas_draw_geometry(c, &line, &brush);
    EXPECT(flux_canvas_end(c) == FLUX_OK);
    px = flux_target_cpu_pixels(b, nullptr, nullptr, &stride);
    EXPECT(px[12 * stride + 6 * 4 + 3] == 0);
    line.stroke.cap = FLUX_CAP_SQUARE;
    EXPECT(begin_target(c, b, &clear) == FLUX_OK);
    flux_canvas_draw_geometry(c, &line, &brush);
    EXPECT(flux_canvas_end(c) == FLUX_OK);
    px = flux_target_cpu_pixels(b, nullptr, nullptr, &stride);
    EXPECT(px[12 * stride + 6 * 4 + 3] == 255);
    flux_target_release(b);
    flux_canvas_release(c);
    TEST_SUMMARY();
}
