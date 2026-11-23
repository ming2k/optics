/*
 * Render-target capture (ADR-0017): render canvas content into a
 * optics_gfx_image via optics_gfx_canvas_begin, then feed that image to
 * optics_gfx_effect_blur, then draw the blurred result back onto a frame.
 *
 * This is the capture -> effect -> composite round trip that real
 * backdrop blur needs. Asserts:
 *   - a captured target holds rendered content (sharp edge present)
 *   - blurring the captured image softens that edge
 *   - drawing the blurred image onto the frame composites correctly
 *   - the begin/end_target state machine rejects nesting and misuse
 */
#include "gfx_test.h"
#include "optics_test.h"
#include <optics/gfx.h>
#include <optics/gfx/effect.h>
#include <optics/gfx/vulkan.h>

#include <stdlib.h>
#include <string.h>

#define W 64u
#define H 64u
#define BYTES (W * H * 4u)
#define BACKDROP_W 640u
#define BACKDROP_H 360u

int main(void) {
    optics_gfx_device *d = optics_gfx_test_make_headless_device();
    if (!d) {
        fprintf(stderr, "test_canvas_target: no Vulkan device; skipping\n");
        OT_SKIP("no Vulkan device");
    }

    optics_gfx_surface_desc sd = OPTICS_GFX_SURFACE_DESC_INIT;
    sd.width = W;
    sd.height = H;
    optics_gfx_surface *s = nullptr;
    OT_EXPECT(optics_gfx_surface_create(d, &sd, &s) == OPTICS_GFX_OK);

    VkFormat sfmt = optics_gfx_surface_vk_format(s);
    optics_gfx_format target_fmt = (sfmt == VK_FORMAT_B8G8R8A8_UNORM)
                                       ? OPTICS_GFX_FORMAT_BGRA8_UNORM
                                       : OPTICS_GFX_FORMAT_RGBA8_UNORM;

    optics_gfx_canvas_desc cd = {.type = OPTICS_GFX_TYPE_CANVAS_DESC, .surface = s};
    optics_gfx_canvas *canvas = nullptr;
    OT_EXPECT(optics_gfx_canvas_create(&cd, &canvas) == OPTICS_GFX_OK);

    optics_gfx_image *target = nullptr;
    OT_EXPECT(optics_gfx_image_create(
                  d,
                  &(optics_gfx_image_desc){.type = OPTICS_GFX_TYPE_IMAGE_DESC,
                                           .width = W,
                                           .height = H,
                                           .format = target_fmt,
                                           .usage = OPTICS_GFX_IMAGE_USAGE_RENDER_TARGET},
                  &target) == OPTICS_GFX_OK);
    OT_EXPECT(optics_gfx_image_bindless_handle(target) != OPTICS_GFX_BINDLESS_INVALID);
    OT_EXPECT(optics_gfx_image_width(target) == W);
    OT_EXPECT(optics_gfx_image_height(target) == H);
    OT_EXPECT(optics_gfx_image_format(target) == target_fmt);

    optics_gfx_blur_filter *blur_filter = nullptr;
    OT_EXPECT(optics_gfx_blur_filter_create(d, &blur_filter) == OPTICS_GFX_OK);

    static uint8_t px[BYTES];
    static uint8_t full_blur_px[BYTES];

    /* --- explicit one-sample clears for compositor/image-heavy passes --- */
    {
        optics_gfx_frame *frame = nullptr;
        OT_EXPECT(optics_gfx_surface_begin_frame(s, nullptr, &frame) == OPTICS_GFX_OK);

        optics_gfx_color teal = optics_gfx_color_rgba(17, 101, 149, 255);
        optics_gfx_canvas_pass_desc pd = OPTICS_GFX_CANVAS_PASS_DESC_INIT;
        pd.clear_color = &teal;
        pd.frame = frame;
        pd.attachment = (optics_gfx_canvas_attachment){.kind = OPTICS_GFX_CANVAS_ATTACHMENT_IMAGE,
                                                       .image = target};
        OT_EXPECT(optics_gfx_canvas_begin(canvas, &pd) == OPTICS_GFX_OK);
        OT_EXPECT(optics_gfx_canvas_end(canvas) == OPTICS_GFX_OK);

        optics_gfx_color black = optics_gfx_color_rgba(0, 0, 0, 255);
        pd.attachment = (optics_gfx_canvas_attachment){};
        pd.clear_color = &black;
        pd.frame = frame;
        OT_EXPECT(optics_gfx_canvas_begin(canvas, &pd) == OPTICS_GFX_OK);
        optics_gfx_canvas_draw_image(canvas, target, (optics_gfx_rect){0, 0, (float)W, (float)H},
                                     nullptr);
        OT_EXPECT(optics_gfx_canvas_end(canvas) == OPTICS_GFX_OK);

        OT_EXPECT(optics_gfx_frame_submit(frame) == OPTICS_GFX_OK);
        OT_EXPECT(optics_gfx_frame_present(frame) == OPTICS_GFX_OK);
        OT_EXPECT(optics_gfx_surface_read_pixels(s, px, BYTES) == OPTICS_GFX_OK);
        const uint8_t *centre = &px[(H / 2 * W + W / 2) * 4];
        OT_EXPECT(centre[0] == 17 && centre[1] == 101 && centre[2] == 149 && centre[3] == 255);
    }

    /* --- capture a half-black / half-white edge, blur it, composite --- */
    {
        optics_gfx_frame *frame = nullptr;
        OT_EXPECT(optics_gfx_surface_begin_frame(s, nullptr, &frame) == OPTICS_GFX_OK);

        /* Capture: black clear + white right half. */
        optics_gfx_color black = optics_gfx_color_rgba(0, 0, 0, 255);
        OT_EXPECT(optics_gfx_canvas_begin(
                      canvas, &(optics_gfx_canvas_pass_desc){
                                  .type = OPTICS_GFX_TYPE_CANVAS_PASS_DESC,
                                  .frame = frame,
                                  .attachment = {.kind = OPTICS_GFX_CANVAS_ATTACHMENT_IMAGE,
                                                 .image = target},
                                  .clear_color = &black}) == OPTICS_GFX_OK);
        optics_gfx_canvas_fill_rect_color(
            canvas, (optics_gfx_rect){(float)(W / 2), 0.0f, (float)(W / 2), (float)H},
            optics_gfx_color_rgba_premul(255, 255, 255, 255));
        OT_EXPECT(optics_gfx_canvas_end(canvas) == OPTICS_GFX_OK);

        /* Blur the captured target (compute, no active pass). */
        optics_gfx_image *blurred = nullptr;
        optics_gfx_effect_blur_desc bd = OPTICS_GFX_EFFECT_BLUR_DESC_INIT;
        bd.input = target;
        bd.sigma = 6.0f;
        OT_EXPECT(optics_gfx_blur_filter_apply(blur_filter,
                                               &(optics_gfx_blur_filter_apply_opts){
                                                   .frame = frame, .desc = &bd, .out = &blurred}) ==
                  OPTICS_GFX_OK);
        OT_EXPECT(blurred != nullptr);
        /* ADR-0147: the blur is deferred; materialize before the pass that
         * samples it, since compute cannot be inserted inside a pass. */
        OT_EXPECT(optics_gfx_frame_materialize_image(frame, blurred) == OPTICS_GFX_OK);

        /* Composite the blurred capture onto the frame. */
        optics_gfx_color fc = optics_gfx_color_rgba(0, 0, 0, 255);
        OT_EXPECT(optics_gfx_canvas_begin(canvas, &(optics_gfx_canvas_pass_desc){
                                                      .type = OPTICS_GFX_TYPE_CANVAS_PASS_DESC,
                                                      .frame = frame,
                                                      .clear_color = &fc}) == OPTICS_GFX_OK);
        optics_gfx_canvas_draw_image(canvas, blurred, (optics_gfx_rect){0, 0, (float)W, (float)H},
                                     nullptr);
        OT_EXPECT(optics_gfx_canvas_end(canvas) == OPTICS_GFX_OK);

        OT_EXPECT(optics_gfx_frame_submit(frame) == OPTICS_GFX_OK);
        OT_EXPECT(optics_gfx_frame_present(frame) == OPTICS_GFX_OK);

        memset(px, 0xCD, BYTES);
        OT_EXPECT(optics_gfx_surface_read_pixels(s, px, BYTES) == OPTICS_GFX_OK);

        /* The sharp captured edge, after sigma=6 blur + composite, should
         * be softened at the midpoint. */
        uint8_t edge_left = px[(H / 2 * W + (W / 2 - 1)) * 4 + 0];
        uint8_t edge_right = px[(H / 2 * W + (W / 2)) * 4 + 0];
        OT_EXPECT(edge_left > 32 && edge_left < 224);
        OT_EXPECT(edge_right > 32 && edge_right < 224);
        /* far sides stay near-black / near-white */
        OT_EXPECT(px[(H / 2 * W + 0) * 4 + 0] < 16);
        OT_EXPECT(px[(H / 2 * W + (W - 1)) * 4 + 0] > 240);
        memcpy(full_blur_px, px, BYTES);
        printf("capture+blur+composite edge: left=%u right=%u (softened)\n", edge_left, edge_right);
    }

    /* --- reusable region blur matches the full pass away from its halo --- */
    {
        optics_gfx_frame *frame = nullptr;
        OT_EXPECT(optics_gfx_surface_begin_frame(s, nullptr, &frame) == OPTICS_GFX_OK);

        /* A wholly clipped list is rejected instead of returning stale slot
         * pixels under the guise of a valid blurred image. */
        optics_gfx_effect_region invalid_region = {
            .x = W + 8u, .y = H + 8u, .width = 4u, .height = 4u};
        optics_gfx_effect_blur_regions_desc invalid_regions =
            OPTICS_GFX_EFFECT_BLUR_REGIONS_DESC_INIT;
        invalid_regions.regions = &invalid_region;
        invalid_regions.region_count = 1;
        optics_gfx_effect_blur_desc invalid_blur = OPTICS_GFX_EFFECT_BLUR_DESC_INIT;
        invalid_blur.next = &invalid_regions;
        invalid_blur.input = target;
        invalid_blur.sigma = 6.0f;
        optics_gfx_image *blurred = nullptr;
        OT_EXPECT(optics_gfx_blur_filter_apply(
                      blur_filter, &(optics_gfx_blur_filter_apply_opts){.frame = frame,
                                                                        .desc = &invalid_blur,
                                                                        .out = &blurred}) ==
                  OPTICS_GFX_ERROR_INVALID_ARGUMENT);
        OT_EXPECT(blurred == nullptr);

        /* The requested dispatch includes a 16px halo. Only the 24x24 inner
         * rectangle is consumed below; its pixels must match the full-image
         * reference produced by the previous frame. */
        optics_gfx_effect_region region = {.x = 4u, .y = 4u, .width = 56u, .height = 56u};
        optics_gfx_effect_blur_regions_desc regions = OPTICS_GFX_EFFECT_BLUR_REGIONS_DESC_INIT;
        regions.regions = &region;
        regions.region_count = 1;
        optics_gfx_effect_blur_desc bd = OPTICS_GFX_EFFECT_BLUR_DESC_INIT;
        bd.next = &regions;
        bd.input = target;
        bd.sigma = 6.0f;
        OT_EXPECT(optics_gfx_blur_filter_apply(blur_filter,
                                               &(optics_gfx_blur_filter_apply_opts){
                                                   .frame = frame, .desc = &bd, .out = &blurred}) ==
                  OPTICS_GFX_OK);
        OT_EXPECT(blurred != nullptr);
        OT_EXPECT(optics_gfx_frame_materialize_image(frame, blurred) == OPTICS_GFX_OK);

        optics_gfx_color black = optics_gfx_color_rgba(0, 0, 0, 255);
        OT_EXPECT(optics_gfx_canvas_begin(canvas, &(optics_gfx_canvas_pass_desc){
                                                      .type = OPTICS_GFX_TYPE_CANVAS_PASS_DESC,
                                                      .frame = frame,
                                                      .clear_color = &black}) == OPTICS_GFX_OK);
        optics_gfx_canvas_draw_image_sub(
            canvas, blurred, (optics_gfx_rect){20, 20, 24, 24},
            (optics_gfx_rect){20.0f / W, 20.0f / H, 24.0f / W, 24.0f / H});
        OT_EXPECT(optics_gfx_canvas_end(canvas) == OPTICS_GFX_OK);
        OT_EXPECT(optics_gfx_frame_submit(frame) == OPTICS_GFX_OK);
        OT_EXPECT(optics_gfx_frame_present(frame) == OPTICS_GFX_OK);
        memset(px, 0, BYTES);
        OT_EXPECT(optics_gfx_surface_read_pixels(s, px, BYTES) == OPTICS_GFX_OK);

        for (uint32_t y = 20; y < 44; ++y) {
            for (uint32_t x = 20; x < 44; ++x) {
                for (uint32_t channel = 0; channel < 4; ++channel) {
                    uint8_t actual = px[(y * W + x) * 4 + channel];
                    uint8_t expected = full_blur_px[(y * W + x) * 4 + channel];
                    int delta = (int)actual - (int)expected;
                    OT_EXPECT(delta >= -2 && delta <= 2);
                }
            }
        }
    }

    /* --- LOAD a captured target, overlay, then composite it unchanged --- */
    {
        optics_gfx_frame *frame = nullptr;
        OT_EXPECT(optics_gfx_surface_begin_frame(s, nullptr, &frame) == OPTICS_GFX_OK);

        OT_EXPECT(optics_gfx_canvas_begin(
                      canvas, &(optics_gfx_canvas_pass_desc){
                                  .type = OPTICS_GFX_TYPE_CANVAS_PASS_DESC,
                                  .frame = frame,
                                  .attachment = {.kind = OPTICS_GFX_CANVAS_ATTACHMENT_IMAGE,
                                                 .image = target},
                                  .clear_color = nullptr}) == OPTICS_GFX_OK);
        optics_gfx_canvas_fill_rect_color(canvas, (optics_gfx_rect){2, 2, 8, 8},
                                          optics_gfx_color_rgba_premul(0, 255, 0, 255));
        OT_EXPECT(optics_gfx_canvas_end(canvas) == OPTICS_GFX_OK);

        optics_gfx_color black = optics_gfx_color_rgba(0, 0, 0, 255);
        OT_EXPECT(optics_gfx_canvas_begin(canvas, &(optics_gfx_canvas_pass_desc){
                                                      .type = OPTICS_GFX_TYPE_CANVAS_PASS_DESC,
                                                      .frame = frame,
                                                      .clear_color = &black}) == OPTICS_GFX_OK);
        optics_gfx_canvas_draw_image(canvas, target, (optics_gfx_rect){0, 0, (float)W, (float)H},
                                     nullptr);
        OT_EXPECT(optics_gfx_canvas_end(canvas) == OPTICS_GFX_OK);

        OT_EXPECT(optics_gfx_frame_submit(frame) == OPTICS_GFX_OK);
        OT_EXPECT(optics_gfx_frame_present(frame) == OPTICS_GFX_OK);
        memset(px, 0xCD, BYTES);
        OT_EXPECT(optics_gfx_surface_read_pixels(s, px, BYTES) == OPTICS_GFX_OK);

        const uint8_t *overlay = &px[(5 * W + 5) * 4];
        OT_EXPECT(overlay[0] < 5 && overlay[1] > 250 && overlay[2] < 5);
        OT_EXPECT(px[(H / 2 * W + 4) * 4] < 16);
        OT_EXPECT(px[(H / 2 * W + (W - 4)) * 4] > 240);
    }

    /* --- reusable blur cycles through and safely reuses frame slots --- */
    for (uint32_t iteration = 0; iteration < 5; ++iteration) {
        optics_gfx_frame *frame = nullptr;
        OT_EXPECT(optics_gfx_surface_begin_frame(s, nullptr, &frame) == OPTICS_GFX_OK);

        uint8_t red = (uint8_t)(40u + iteration * 30u);
        optics_gfx_color fill = optics_gfx_color_rgba(red, 20, 40, 255);
        OT_EXPECT(optics_gfx_canvas_begin(
                      canvas, &(optics_gfx_canvas_pass_desc){
                                  .type = OPTICS_GFX_TYPE_CANVAS_PASS_DESC,
                                  .frame = frame,
                                  .attachment = {.kind = OPTICS_GFX_CANVAS_ATTACHMENT_IMAGE,
                                                 .image = target},
                                  .clear_color = &fill}) == OPTICS_GFX_OK);
        OT_EXPECT(optics_gfx_canvas_end(canvas) == OPTICS_GFX_OK);

        optics_gfx_effect_blur_desc bd = OPTICS_GFX_EFFECT_BLUR_DESC_INIT;
        bd.input = target;
        bd.sigma = 2.0f;
        optics_gfx_image *blurred = nullptr;
        OT_EXPECT(optics_gfx_blur_filter_apply(blur_filter,
                                               &(optics_gfx_blur_filter_apply_opts){
                                                   .frame = frame, .desc = &bd, .out = &blurred}) ==
                  OPTICS_GFX_OK);
        OT_EXPECT(blurred != nullptr);
        OT_EXPECT(optics_gfx_frame_materialize_image(frame, blurred) == OPTICS_GFX_OK);

        optics_gfx_color black = optics_gfx_color_rgba(0, 0, 0, 255);
        OT_EXPECT(optics_gfx_canvas_begin(canvas, &(optics_gfx_canvas_pass_desc){
                                                      .type = OPTICS_GFX_TYPE_CANVAS_PASS_DESC,
                                                      .frame = frame,
                                                      .clear_color = &black}) == OPTICS_GFX_OK);
        optics_gfx_canvas_draw_image(canvas, blurred, (optics_gfx_rect){0, 0, (float)W, (float)H},
                                     nullptr);
        OT_EXPECT(optics_gfx_canvas_end(canvas) == OPTICS_GFX_OK);
        OT_EXPECT(optics_gfx_frame_submit(frame) == OPTICS_GFX_OK);
        OT_EXPECT(optics_gfx_frame_present(frame) == OPTICS_GFX_OK);
        memset(px, 0, BYTES);
        OT_EXPECT(optics_gfx_surface_read_pixels(s, px, BYTES) == OPTICS_GFX_OK);
        OT_EXPECT(px[(H / 2 * W + W / 2) * 4] >= red - 2u);
    }

    /* --- reusable frame-slot shadow: capture a shape, shadow it, composite,
     * and cycle frame slots without transient-pool leases --- */
    {
        optics_gfx_shadow_filter *shadow_filter = nullptr;
        OT_EXPECT(optics_gfx_shadow_filter_create(d, &shadow_filter) == OPTICS_GFX_OK);

        for (uint32_t iteration = 0; iteration < 5; ++iteration) {
            optics_gfx_frame *frame = nullptr;
            OT_EXPECT(optics_gfx_surface_begin_frame(s, nullptr, &frame) == OPTICS_GFX_OK);

            /* Capture an opaque white square on transparent: the mask. */
            optics_gfx_color transparent = optics_gfx_color_rgba(0, 0, 0, 0);
            OT_EXPECT(optics_gfx_canvas_begin(
                          canvas, &(optics_gfx_canvas_pass_desc){
                                      .type = OPTICS_GFX_TYPE_CANVAS_PASS_DESC,
                                      .frame = frame,
                                      .attachment = {.kind = OPTICS_GFX_CANVAS_ATTACHMENT_IMAGE,
                                                     .image = target},
                                      .clear_color = &transparent}) == OPTICS_GFX_OK);
            optics_gfx_canvas_fill_rect_color(
                canvas,
                (optics_gfx_rect){(float)(W / 4), (float)(H / 4), (float)(W / 2), (float)(H / 2)},
                optics_gfx_color_rgba_premul(255, 255, 255, 255));
            OT_EXPECT(optics_gfx_canvas_end(canvas) == OPTICS_GFX_OK);

            optics_gfx_effect_shadow_desc sd = OPTICS_GFX_EFFECT_SHADOW_DESC_INIT;
            sd.input = target;
            sd.blur = 4.0f;
            sd.offset_y = 4.0f;
            sd.tint_red = 0.0f;
            sd.tint_green = 0.0f;
            sd.tint_blue = 0.0f;
            sd.alpha = 1.0f;
            optics_gfx_image *shadow = nullptr;
            OT_EXPECT(optics_gfx_shadow_filter_apply(
                          shadow_filter,
                          &(optics_gfx_shadow_filter_apply_opts){
                              .frame = frame, .desc = &sd, .out = &shadow}) == OPTICS_GFX_OK);
            OT_EXPECT(shadow != nullptr);
            /* ADR-0147: the shadow is deferred; materialize before the pass
             * that samples it (compute cannot run inside a render pass). */
            OT_EXPECT(optics_gfx_frame_materialize_image(frame, shadow) == OPTICS_GFX_OK);

            /* White background so the black shadow is actually visible:
             * inside the offset square the coverage is near-opaque (colour
             * collapses toward black); the far corner keeps near-white. */
            optics_gfx_color white = optics_gfx_color_rgba(255, 255, 255, 255);
            OT_EXPECT(optics_gfx_canvas_begin(canvas, &(optics_gfx_canvas_pass_desc){
                                                          .type = OPTICS_GFX_TYPE_CANVAS_PASS_DESC,
                                                          .frame = frame,
                                                          .clear_color = &white}) == OPTICS_GFX_OK);
            optics_gfx_canvas_draw_image(canvas, shadow,
                                         (optics_gfx_rect){0, 0, (float)W, (float)H}, nullptr);
            OT_EXPECT(optics_gfx_canvas_end(canvas) == OPTICS_GFX_OK);
            OT_EXPECT(optics_gfx_frame_submit(frame) == OPTICS_GFX_OK);
            OT_EXPECT(optics_gfx_frame_present(frame) == OPTICS_GFX_OK);

            memset(px, 0, BYTES);
            OT_EXPECT(optics_gfx_surface_read_pixels(s, px, BYTES) == OPTICS_GFX_OK);
            /* The composite target is opaque, so the alpha channel is
             * always 255 — the shadow is visible through the colour
             * channels (black shadow over white mask). Inside the offset
             * square the colour is near-black; the far corner stays
             * near-white. */
            uint8_t centre_r = px[((H / 2 + 4) * W + W / 2) * 4 + 0];
            uint8_t corner_r = px[(2 * W + 2) * 4 + 0];
            OT_EXPECT(centre_r < 55u);
            OT_EXPECT(corner_r > 235u);
        }
        optics_gfx_device_wait_idle(d);
        optics_gfx_shadow_filter_release(shadow_filter);
    }

    /* --- nesting rejected: begin_target inside an active frame pass --- */
    {
        optics_gfx_frame *frame = nullptr;
        OT_EXPECT(optics_gfx_surface_begin_frame(s, nullptr, &frame) == OPTICS_GFX_OK);
        optics_gfx_color black = optics_gfx_color_rgba(0, 0, 0, 255);
        OT_EXPECT(optics_gfx_canvas_begin(canvas, &(optics_gfx_canvas_pass_desc){
                                                      .type = OPTICS_GFX_TYPE_CANVAS_PASS_DESC,
                                                      .frame = frame,
                                                      .clear_color = &black}) == OPTICS_GFX_OK);
        OT_EXPECT(optics_gfx_canvas_begin(
                      canvas, &(optics_gfx_canvas_pass_desc){
                                  .type = OPTICS_GFX_TYPE_CANVAS_PASS_DESC,
                                  .frame = frame,
                                  .attachment = {.kind = OPTICS_GFX_CANVAS_ATTACHMENT_IMAGE,
                                                 .image = target},
                                  .clear_color = &black}) == OPTICS_GFX_ERROR_INVALID_STATE);
        OT_EXPECT(optics_gfx_canvas_end(canvas) == OPTICS_GFX_OK);
        optics_gfx_effect_reset(d);
        OT_EXPECT(optics_gfx_frame_submit(frame) == OPTICS_GFX_OK);
        OT_EXPECT(optics_gfx_frame_present(frame) == OPTICS_GFX_OK);
    }

    /* --- downsampled target: render small, then upscale on composite --- */
    {
        optics_gfx_image *downsampled = nullptr;
        OT_EXPECT(optics_gfx_image_create(
                      d,
                      &(optics_gfx_image_desc){.type = OPTICS_GFX_TYPE_IMAGE_DESC,
                                               .width = W / 2,
                                               .height = H / 2,
                                               .format = target_fmt,
                                               .usage = OPTICS_GFX_IMAGE_USAGE_RENDER_TARGET},
                      &downsampled) == OPTICS_GFX_OK);
        optics_gfx_frame *frame = nullptr;
        OT_EXPECT(optics_gfx_surface_begin_frame(s, nullptr, &frame) == OPTICS_GFX_OK);
        optics_gfx_color red = optics_gfx_color_rgba(255, 0, 0, 255);
        OT_EXPECT(optics_gfx_canvas_begin(
                      canvas, &(optics_gfx_canvas_pass_desc){
                                  .type = OPTICS_GFX_TYPE_CANVAS_PASS_DESC,
                                  .frame = frame,
                                  .attachment = {.kind = OPTICS_GFX_CANVAS_ATTACHMENT_IMAGE,
                                                 .image = downsampled},
                                  .clear_color = &red}) == OPTICS_GFX_OK);
        OT_EXPECT(optics_gfx_canvas_end(canvas) == OPTICS_GFX_OK);
        optics_gfx_color black = optics_gfx_color_rgba(0, 0, 0, 255);
        OT_EXPECT(optics_gfx_canvas_begin(canvas, &(optics_gfx_canvas_pass_desc){
                                                      .type = OPTICS_GFX_TYPE_CANVAS_PASS_DESC,
                                                      .frame = frame,
                                                      .clear_color = &black}) == OPTICS_GFX_OK);
        optics_gfx_canvas_draw_image(canvas, downsampled,
                                     (optics_gfx_rect){0, 0, (float)W, (float)H}, nullptr);
        OT_EXPECT(optics_gfx_canvas_end(canvas) == OPTICS_GFX_OK);
        OT_EXPECT(optics_gfx_frame_submit(frame) == OPTICS_GFX_OK);
        OT_EXPECT(optics_gfx_frame_present(frame) == OPTICS_GFX_OK);
        memset(px, 0, BYTES);
        OT_EXPECT(optics_gfx_surface_read_pixels(s, px, BYTES) == OPTICS_GFX_OK);
        OT_EXPECT(px[(H / 2 * W + W / 2) * 4] > 240);
        optics_gfx_image_release(downsampled);
    }

    /* --- representative HiDPI backdrop capture stays bounded at max sigma --- */
    {
        optics_gfx_image *backdrop = nullptr;
        OT_EXPECT(optics_gfx_image_create(
                      d,
                      &(optics_gfx_image_desc){.type = OPTICS_GFX_TYPE_IMAGE_DESC,
                                               .width = BACKDROP_W,
                                               .height = BACKDROP_H,
                                               .format = target_fmt,
                                               .usage = OPTICS_GFX_IMAGE_USAGE_RENDER_TARGET},
                      &backdrop) == OPTICS_GFX_OK);
        optics_gfx_frame *frame = nullptr;
        OT_EXPECT(optics_gfx_surface_begin_frame(s, nullptr, &frame) == OPTICS_GFX_OK);
        optics_gfx_color source = optics_gfx_color_rgba(35, 70, 105, 255);
        OT_EXPECT(optics_gfx_canvas_begin(
                      canvas, &(optics_gfx_canvas_pass_desc){
                                  .type = OPTICS_GFX_TYPE_CANVAS_PASS_DESC,
                                  .frame = frame,
                                  .attachment = {.kind = OPTICS_GFX_CANVAS_ATTACHMENT_IMAGE,
                                                 .image = backdrop},
                                  .clear_color = &source}) == OPTICS_GFX_OK);
        OT_EXPECT(optics_gfx_canvas_end(canvas) == OPTICS_GFX_OK);

        optics_gfx_effect_blur_desc bd = OPTICS_GFX_EFFECT_BLUR_DESC_INIT;
        bd.input = backdrop;
        bd.sigma = OPTICS_GFX_EFFECT_BLUR_SIGMA_MAX;
        optics_gfx_image *blurred = nullptr;
        OT_EXPECT(optics_gfx_blur_filter_apply(blur_filter,
                                               &(optics_gfx_blur_filter_apply_opts){
                                                   .frame = frame, .desc = &bd, .out = &blurred}) ==
                  OPTICS_GFX_OK);
        OT_EXPECT(optics_gfx_frame_materialize_image(frame, blurred) == OPTICS_GFX_OK);

        optics_gfx_color black = optics_gfx_color_rgba(0, 0, 0, 255);
        OT_EXPECT(optics_gfx_canvas_begin(canvas, &(optics_gfx_canvas_pass_desc){
                                                      .type = OPTICS_GFX_TYPE_CANVAS_PASS_DESC,
                                                      .frame = frame,
                                                      .clear_color = &black}) == OPTICS_GFX_OK);
        optics_gfx_canvas_draw_image(canvas, blurred, (optics_gfx_rect){0, 0, (float)W, (float)H},
                                     nullptr);
        OT_EXPECT(optics_gfx_canvas_end(canvas) == OPTICS_GFX_OK);
        OT_EXPECT(optics_gfx_frame_submit(frame) == OPTICS_GFX_OK);
        OT_EXPECT(optics_gfx_frame_present(frame) == OPTICS_GFX_OK);

        memset(px, 0, BYTES);
        OT_EXPECT(optics_gfx_surface_read_pixels(s, px, BYTES) == OPTICS_GFX_OK);
        const uint8_t *centre = &px[(H / 2 * W + W / 2) * 4];
        OT_EXPECT(centre[0] >= 33 && centre[0] <= 37);
        OT_EXPECT(centre[1] >= 68 && centre[1] <= 72);
        OT_EXPECT(centre[2] >= 103 && centre[2] <= 107);
        optics_gfx_image_release(backdrop);
    }

    /* --- cross-format capture: sampled BGRA/RGBA normalizes to RGBA storage --- */
    {
        optics_gfx_format cross_fmt = target_fmt == OPTICS_GFX_FORMAT_RGBA8_UNORM
                                          ? OPTICS_GFX_FORMAT_BGRA8_UNORM
                                          : OPTICS_GFX_FORMAT_RGBA8_UNORM;
        optics_gfx_image *cross_target = nullptr;
        OT_EXPECT(optics_gfx_image_create(
                      d,
                      &(optics_gfx_image_desc){.type = OPTICS_GFX_TYPE_IMAGE_DESC,
                                               .width = W / 2,
                                               .height = H / 2,
                                               .format = cross_fmt,
                                               .usage = OPTICS_GFX_IMAGE_USAGE_RENDER_TARGET},
                      &cross_target) == OPTICS_GFX_OK);
        optics_gfx_frame *frame = nullptr;
        OT_EXPECT(optics_gfx_surface_begin_frame(s, nullptr, &frame) == OPTICS_GFX_OK);
        optics_gfx_color source = optics_gfx_color_rgba(11, 22, 33, 255);
        OT_EXPECT(optics_gfx_canvas_begin(
                      canvas, &(optics_gfx_canvas_pass_desc){
                                  .type = OPTICS_GFX_TYPE_CANVAS_PASS_DESC,
                                  .frame = frame,
                                  .attachment = {.kind = OPTICS_GFX_CANVAS_ATTACHMENT_IMAGE,
                                                 .image = cross_target},
                                  .clear_color = &source}) == OPTICS_GFX_OK);
        OT_EXPECT(optics_gfx_canvas_end(canvas) == OPTICS_GFX_OK);

        optics_gfx_effect_blur_desc bd = OPTICS_GFX_EFFECT_BLUR_DESC_INIT;
        bd.input = cross_target;
        bd.sigma = 4.0f;
        optics_gfx_image *blurred = nullptr;
        OT_EXPECT(optics_gfx_blur_filter_apply(blur_filter,
                                               &(optics_gfx_blur_filter_apply_opts){
                                                   .frame = frame, .desc = &bd, .out = &blurred}) ==
                  OPTICS_GFX_OK);
        OT_EXPECT(optics_gfx_image_format(blurred) == OPTICS_GFX_FORMAT_RGBA8_UNORM);
        OT_EXPECT(optics_gfx_frame_materialize_image(frame, blurred) == OPTICS_GFX_OK);

        optics_gfx_color black = optics_gfx_color_rgba(0, 0, 0, 255);
        OT_EXPECT(optics_gfx_canvas_begin(canvas, &(optics_gfx_canvas_pass_desc){
                                                      .type = OPTICS_GFX_TYPE_CANVAS_PASS_DESC,
                                                      .frame = frame,
                                                      .clear_color = &black}) == OPTICS_GFX_OK);
        optics_gfx_canvas_draw_image(canvas, blurred, (optics_gfx_rect){0, 0, (float)W, (float)H},
                                     nullptr);
        OT_EXPECT(optics_gfx_canvas_end(canvas) == OPTICS_GFX_OK);
        OT_EXPECT(optics_gfx_frame_submit(frame) == OPTICS_GFX_OK);
        OT_EXPECT(optics_gfx_frame_present(frame) == OPTICS_GFX_OK);

        memset(px, 0, BYTES);
        OT_EXPECT(optics_gfx_surface_read_pixels(s, px, BYTES) == OPTICS_GFX_OK);
        const uint8_t *centre = &px[(H / 2 * W + W / 2) * 4];
        OT_EXPECT(centre[0] >= 9 && centre[0] <= 13);
        OT_EXPECT(centre[1] >= 20 && centre[1] <= 24);
        OT_EXPECT(centre[2] >= 31 && centre[2] <= 35);
        optics_gfx_image_release(cross_target);
    }

    optics_gfx_device_wait_idle(d);
    optics_gfx_blur_filter_release(blur_filter);
    optics_gfx_image_release(target);
    optics_gfx_canvas_release(canvas);
    optics_gfx_surface_release(s);
    optics_gfx_device_release(d);
    return OT_REPORT();
}
