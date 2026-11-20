/* Observable clean-break contracts (ADR-0095). */
#include "optics_test.h"
#include <math.h>
#include <optics/gfx/canvas_cpu.h>
#include <string.h>

static optics_gfx_result begin_target(optics_gfx_canvas *c, optics_gfx_target *target,
                                      const optics_gfx_color *clear) {
    optics_gfx_canvas_pass_desc pass = OPTICS_GFX_CANVAS_PASS_DESC_INIT;
    pass.attachment = (optics_gfx_canvas_attachment){.kind = OPTICS_GFX_CANVAS_ATTACHMENT_TARGET,
                                                     .target = target};
    pass.clear_color = clear;
    return optics_gfx_canvas_begin(c, &pass);
}

int main(void) {
    optics_gfx_canvas *c = nullptr;
    OT_EXPECT(
        optics_gfx_canvas_create(&(optics_gfx_canvas_desc){.type = OPTICS_GFX_TYPE_CANVAS_DESC,
                                                           .backend = OPTICS_GFX_CANVAS_BACKEND_CPU,
                                                           .scale = 1,
                                                           .width = 8,
                                                           .height = 8},
                                 &c) == OPTICS_GFX_OK);
    optics_gfx_cpu_target_desc desc = OPTICS_GFX_CPU_TARGET_DESC_INIT;
    desc.width = 32;
    desc.height = 24;
    optics_gfx_target *a = nullptr, *b = nullptr;
    OT_EXPECT(optics_gfx_target_create_cpu(&desc, &a) == OPTICS_GFX_OK);
    OT_EXPECT(optics_gfx_target_create_cpu(&desc, &b) == OPTICS_GFX_OK);
    optics_gfx_color red = 0xffff0000u, blue = 0xff0000ffu;
    OT_EXPECT(begin_target(c, a, &red) == OPTICS_GFX_OK);
    OT_EXPECT(optics_gfx_canvas_end(c) == OPTICS_GFX_OK);
    OT_EXPECT(begin_target(c, b, &blue) == OPTICS_GFX_OK);
    OT_EXPECT(optics_gfx_canvas_end(c) == OPTICS_GFX_OK);
    /* Loading A after B preserves A; drawing uses A's extent, not 8x8. */
    OT_EXPECT(begin_target(c, a, nullptr) == OPTICS_GFX_OK);
    optics_gfx_canvas_fill_rect_color(c, (optics_gfx_rect){20, 16, 8, 6}, 0xff00ff00u);
    OT_EXPECT(optics_gfx_canvas_end(c) == OPTICS_GFX_OK);
    uint32_t width, height, stride;
    const uint8_t *px = optics_gfx_target_cpu_pixels(a, &width, &height, &stride);
    OT_EXPECT(width == 32 && height == 24);
    OT_EXPECT(memcmp(px, (uint8_t[]){255, 0, 0, 255}, 4) == 0);
    OT_EXPECT(memcmp(px + 18 * stride + 22 * 4, (uint8_t[]){0, 255, 0, 255}, 4) == 0);
    px = optics_gfx_target_cpu_pixels(b, nullptr, nullptr, nullptr);
    OT_EXPECT(memcmp(px, (uint8_t[]){0, 0, 255, 255}, 4) == 0);
    /* Dirty clears preserve the destination's pixels outside the region. */
    optics_gfx_canvas_pass_desc pass = OPTICS_GFX_CANVAS_PASS_DESC_INIT;
    pass.attachment =
        (optics_gfx_canvas_attachment){.kind = OPTICS_GFX_CANVAS_ATTACHMENT_TARGET, .target = a};
    pass.clear_color = &blue;
    const optics_gfx_rect corner = {0, 0, 2, 2};
    pass.damage = (optics_gfx_damage){.rects = &corner, .count = 1};
    OT_EXPECT(optics_gfx_canvas_begin(c, &pass) == OPTICS_GFX_OK);
    OT_EXPECT(optics_gfx_canvas_end(c) == OPTICS_GFX_OK);
    px = optics_gfx_target_cpu_pixels(a, nullptr, nullptr, nullptr);
    OT_EXPECT(memcmp(px, (uint8_t[]){0, 0, 255, 255}, 4) == 0);
    OT_EXPECT(memcmp(px + 3 * stride + 3 * 4, (uint8_t[]){255, 0, 0, 255}, 4) == 0);
    /* The pass owns its destination reference until close. */
    OT_EXPECT(begin_target(c, a, &red) == OPTICS_GFX_OK);
    optics_gfx_target_release(a);
    a = nullptr;
    OT_EXPECT(optics_gfx_canvas_end(c) == OPTICS_GFX_OK);
    desc.stride_bytes = 4;
    optics_gfx_target *bad = b;
    OT_EXPECT(optics_gfx_target_create_cpu(&desc, &bad) == OPTICS_GFX_ERROR_OUT_OF_RANGE);
    OT_EXPECT(bad == nullptr);

    /* Invalid brush inputs fail consistently for encoding and immediate draws. */
    optics_gfx_geometry rect = optics_gfx_geom_rect((optics_gfx_rect){0, 0, 4, 4});
    optics_gfx_brush brush = optics_gfx_brush_solid(red);
    brush.opacity = NAN;
    OT_EXPECT(optics_gfx_canvas_begin(c, nullptr) == OPTICS_GFX_OK);
    optics_gfx_canvas_draw_geometry(
        c, &(optics_gfx_canvas_draw_geometry_opts){.geom = &rect, .brush = &brush});
    OT_EXPECT(optics_gfx_canvas_end(c) == OPTICS_GFX_ERROR_INVALID_ARGUMENT);
    optics_gfx_encoder *encoder = nullptr;
    OT_EXPECT(optics_gfx_encoder_create(nullptr, &encoder) == OPTICS_GFX_OK);
    optics_gfx_encoder_draw_geometry(
        encoder, &(optics_gfx_encoder_draw_geometry_opts){.geom = &rect, .brush = &brush});
    optics_gfx_display_list *list = nullptr;
    OT_EXPECT(optics_gfx_encoder_finish(encoder, &list) == OPTICS_GFX_ERROR_INVALID_ARGUMENT);
    OT_EXPECT(list == nullptr);
    optics_gfx_encoder_destroy(encoder);
    /* Stack errors close and reset the pass, including unclosed layers. */
    OT_EXPECT(optics_gfx_canvas_begin(c, nullptr) == OPTICS_GFX_OK);
    optics_gfx_canvas_restore(c);
    OT_EXPECT(optics_gfx_canvas_end(c) == OPTICS_GFX_ERROR_INVALID_STATE);
    OT_EXPECT(optics_gfx_canvas_begin(c, nullptr) == OPTICS_GFX_OK);
    optics_gfx_canvas_save_layer(
        c, &(optics_gfx_canvas_save_layer_opts){.bounds = nullptr, .opacity = 0.5f});
    OT_EXPECT(optics_gfx_canvas_end(c) == OPTICS_GFX_ERROR_INVALID_STATE);
    OT_EXPECT(optics_gfx_canvas_begin(c, nullptr) == OPTICS_GFX_OK);
    OT_EXPECT(optics_gfx_canvas_end(c) == OPTICS_GFX_OK);
    OT_EXPECT(optics_gfx_canvas_end(c) == OPTICS_GFX_ERROR_INVALID_STATE);

    /* Cap topology survives geometry -> backend conversion. */
    optics_gfx_geometry line = optics_gfx_geom_line(8, 12, 24, 12);
    line.stroke.width = 6;
    brush = optics_gfx_brush_solid(0xffffffffu);
    optics_gfx_color clear = 0;
    OT_EXPECT(begin_target(c, b, &clear) == OPTICS_GFX_OK);
    optics_gfx_canvas_draw_geometry(
        c, &(optics_gfx_canvas_draw_geometry_opts){.geom = &line, .brush = &brush});
    OT_EXPECT(optics_gfx_canvas_end(c) == OPTICS_GFX_OK);
    px = optics_gfx_target_cpu_pixels(b, nullptr, nullptr, &stride);
    OT_EXPECT(px[12 * stride + 6 * 4 + 3] == 0);
    line.stroke.cap = OPTICS_GFX_CAP_SQUARE;
    OT_EXPECT(begin_target(c, b, &clear) == OPTICS_GFX_OK);
    optics_gfx_canvas_draw_geometry(
        c, &(optics_gfx_canvas_draw_geometry_opts){.geom = &line, .brush = &brush});
    OT_EXPECT(optics_gfx_canvas_end(c) == OPTICS_GFX_OK);
    px = optics_gfx_target_cpu_pixels(b, nullptr, nullptr, &stride);
    OT_EXPECT(px[12 * stride + 6 * 4 + 3] == 255);
    optics_gfx_target_release(b);
    optics_gfx_canvas_release(c);
    return OT_REPORT();
}
