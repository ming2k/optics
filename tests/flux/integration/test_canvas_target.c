/*
 * Render-target capture (ADR-0017): render canvas content into a
 * flux_image via flux_canvas_begin_target, then feed that image to
 * flux_effect_blur, then draw the blurred result back onto a frame.
 *
 * This is the capture -> effect -> composite round trip that real
 * backdrop blur needs. Asserts:
 *   - a captured target holds rendered content (sharp edge present)
 *   - blurring the captured image softens that edge
 *   - drawing the blurred image onto the frame composites correctly
 *   - the begin/end_target state machine rejects nesting and misuse
 */
#include "test_helpers.h"
#include <flux/effect.h>
#include <flux/flux.h>
#include <flux/vulkan.h>

#include <stdlib.h>
#include <string.h>

#define W 64u
#define H 64u
#define BYTES (W * H * 4u)
#define BACKDROP_W 640u
#define BACKDROP_H 360u

int main(void) {
    flux_device *d = test_helpers_make_headless_device();
    if (!d) {
        fprintf(stderr, "test_canvas_target: no Vulkan device; skipping\n");
        TEST_SUMMARY();
        return 0;
    }

    flux_surface_desc sd = FLUX_SURFACE_DESC_INIT;
    sd.width = W;
    sd.height = H;
    flux_surface *s = nullptr;
    EXPECT(flux_surface_create(d, &sd, &s) == FLUX_OK);

    VkFormat sfmt = flux_surface_vk_format(s);
    flux_format target_fmt =
        (sfmt == VK_FORMAT_B8G8R8A8_UNORM) ? FLUX_FORMAT_BGRA8_UNORM : FLUX_FORMAT_RGBA8_UNORM;

    flux_canvas_desc cd = {.type = FLUX_TYPE_CANVAS_DESC, .surface = s};
    flux_canvas *canvas = nullptr;
    EXPECT(flux_canvas_create(&cd, &canvas) == FLUX_OK);

    flux_image *target = nullptr;
    EXPECT(flux_image_create_render_target(d, W, H, target_fmt, &target) == FLUX_OK);
    EXPECT(flux_image_bindless_handle(target) != FLUX_BINDLESS_INVALID);
    EXPECT(flux_image_width(target) == W);
    EXPECT(flux_image_height(target) == H);
    EXPECT(flux_image_format(target) == target_fmt);

    flux_blur_filter *blur_filter = nullptr;
    EXPECT(flux_blur_filter_create(d, &blur_filter) == FLUX_OK);

    static uint8_t px[BYTES];
    static uint8_t full_blur_px[BYTES];

    /* --- explicit one-sample clears for compositor/image-heavy passes --- */
    {
        flux_frame *frame = nullptr;
        EXPECT(flux_surface_begin_frame(s, nullptr, &frame) == FLUX_OK);

        flux_color teal = flux_color_rgba(17, 101, 149, 255);
        flux_canvas_pass_desc pd = FLUX_CANVAS_PASS_DESC_INIT;
        pd.clear_color = &teal;
        pd.antialias = FLUX_CANVAS_ANTIALIAS_NONE;
        EXPECT(flux_canvas_begin_target_pass(canvas, frame, target, &pd) == FLUX_OK);
        flux_canvas_end_target(canvas);

        flux_color black = flux_color_rgba(0, 0, 0, 255);
        pd.clear_color = &black;
        EXPECT(flux_canvas_begin_pass(canvas, frame, &pd) == FLUX_OK);
        flux_canvas_draw_image(canvas, target, (flux_rect){0, 0, (float)W, (float)H}, nullptr);
        flux_canvas_end(canvas);

        EXPECT(flux_frame_submit(frame) == FLUX_OK);
        EXPECT(flux_frame_present(frame) == FLUX_OK);
        EXPECT(flux_surface_read_pixels(s, px, BYTES) == FLUX_OK);
        const uint8_t *centre = &px[(H / 2 * W + W / 2) * 4];
        EXPECT(centre[0] == 17 && centre[1] == 101 && centre[2] == 149 && centre[3] == 255);
    }

    /* --- capture a half-black / half-white edge, blur it, composite --- */
    {
        flux_frame *frame = nullptr;
        EXPECT(flux_surface_begin_frame(s, nullptr, &frame) == FLUX_OK);

        /* Capture: black clear + white right half. */
        flux_color black = flux_color_rgba(0, 0, 0, 255);
        EXPECT(flux_canvas_begin_target(canvas, frame, target, &black) == FLUX_OK);
        flux_canvas_fill_rect_color(canvas,
                                    (flux_rect){(float)(W / 2), 0.0f, (float)(W / 2), (float)H},
                                    flux_color_rgba_premul(255, 255, 255, 255));
        flux_canvas_end_target(canvas);

        /* Blur the captured target (compute, no active pass). */
        flux_image *blurred = nullptr;
        flux_effect_blur_desc bd = FLUX_EFFECT_BLUR_DESC_INIT;
        bd.input = target;
        bd.sigma = 6.0f;
        EXPECT(flux_blur_filter_apply(blur_filter, frame, &bd, &blurred) == FLUX_OK);
        EXPECT(blurred != nullptr);

        /* Composite the blurred capture onto the frame. */
        flux_color fc = flux_color_rgba(0, 0, 0, 255);
        EXPECT(flux_canvas_begin(canvas, frame, &fc) == FLUX_OK);
        flux_canvas_draw_image(canvas, blurred, (flux_rect){0, 0, (float)W, (float)H}, nullptr);
        flux_canvas_end(canvas);

        EXPECT(flux_frame_submit(frame) == FLUX_OK);
        EXPECT(flux_frame_present(frame) == FLUX_OK);

        memset(px, 0xCD, BYTES);
        EXPECT(flux_surface_read_pixels(s, px, BYTES) == FLUX_OK);

        /* The sharp captured edge, after sigma=6 blur + composite, should
         * be softened at the midpoint. */
        uint8_t edge_left = px[(H / 2 * W + (W / 2 - 1)) * 4 + 0];
        uint8_t edge_right = px[(H / 2 * W + (W / 2)) * 4 + 0];
        EXPECT(edge_left > 32 && edge_left < 224);
        EXPECT(edge_right > 32 && edge_right < 224);
        /* far sides stay near-black / near-white */
        EXPECT(px[(H / 2 * W + 0) * 4 + 0] < 16);
        EXPECT(px[(H / 2 * W + (W - 1)) * 4 + 0] > 240);
        memcpy(full_blur_px, px, BYTES);
        printf("capture+blur+composite edge: left=%u right=%u (softened)\n", edge_left, edge_right);
    }

    /* --- reusable region blur matches the full pass away from its halo --- */
    {
        flux_frame *frame = nullptr;
        EXPECT(flux_surface_begin_frame(s, nullptr, &frame) == FLUX_OK);

        /* A wholly clipped list is rejected instead of returning stale slot
         * pixels under the guise of a valid blurred image. */
        flux_effect_region invalid_region = {.x = W + 8u, .y = H + 8u, .width = 4u, .height = 4u};
        flux_effect_blur_regions_desc invalid_regions = FLUX_EFFECT_BLUR_REGIONS_DESC_INIT;
        invalid_regions.regions = &invalid_region;
        invalid_regions.region_count = 1;
        flux_effect_blur_desc invalid_blur = FLUX_EFFECT_BLUR_DESC_INIT;
        invalid_blur.next = &invalid_regions;
        invalid_blur.input = target;
        invalid_blur.sigma = 6.0f;
        flux_image *blurred = nullptr;
        EXPECT(flux_blur_filter_apply(blur_filter, frame, &invalid_blur, &blurred) ==
               FLUX_ERROR_INVALID_ARGUMENT);
        EXPECT(blurred == nullptr);

        /* The requested dispatch includes a 16px halo. Only the 24x24 inner
         * rectangle is consumed below; its pixels must match the full-image
         * reference produced by the previous frame. */
        flux_effect_region region = {.x = 4u, .y = 4u, .width = 56u, .height = 56u};
        flux_effect_blur_regions_desc regions = FLUX_EFFECT_BLUR_REGIONS_DESC_INIT;
        regions.regions = &region;
        regions.region_count = 1;
        flux_effect_blur_desc bd = FLUX_EFFECT_BLUR_DESC_INIT;
        bd.next = &regions;
        bd.input = target;
        bd.sigma = 6.0f;
        EXPECT(flux_blur_filter_apply(blur_filter, frame, &bd, &blurred) == FLUX_OK);
        EXPECT(blurred != nullptr);

        flux_color black = flux_color_rgba(0, 0, 0, 255);
        EXPECT(flux_canvas_begin(canvas, frame, &black) == FLUX_OK);
        flux_canvas_draw_image_sub(canvas, blurred, (flux_rect){20, 20, 24, 24},
                                   (flux_rect){20.0f / W, 20.0f / H, 24.0f / W, 24.0f / H});
        flux_canvas_end(canvas);
        EXPECT(flux_frame_submit(frame) == FLUX_OK);
        EXPECT(flux_frame_present(frame) == FLUX_OK);
        memset(px, 0, BYTES);
        EXPECT(flux_surface_read_pixels(s, px, BYTES) == FLUX_OK);

        for (uint32_t y = 20; y < 44; ++y) {
            for (uint32_t x = 20; x < 44; ++x) {
                for (uint32_t channel = 0; channel < 4; ++channel) {
                    uint8_t actual = px[(y * W + x) * 4 + channel];
                    uint8_t expected = full_blur_px[(y * W + x) * 4 + channel];
                    int delta = (int)actual - (int)expected;
                    EXPECT(delta >= -2 && delta <= 2);
                }
            }
        }
    }

    /* --- LOAD a captured target, overlay, then composite it unchanged --- */
    {
        flux_frame *frame = nullptr;
        EXPECT(flux_surface_begin_frame(s, nullptr, &frame) == FLUX_OK);

        EXPECT(flux_canvas_begin_target(canvas, frame, target, nullptr) == FLUX_OK);
        flux_canvas_fill_rect_color(canvas, (flux_rect){2, 2, 8, 8},
                                    flux_color_rgba_premul(0, 255, 0, 255));
        flux_canvas_end_target(canvas);

        flux_color black = flux_color_rgba(0, 0, 0, 255);
        EXPECT(flux_canvas_begin(canvas, frame, &black) == FLUX_OK);
        flux_canvas_draw_image(canvas, target, (flux_rect){0, 0, (float)W, (float)H}, nullptr);
        flux_canvas_end(canvas);

        EXPECT(flux_frame_submit(frame) == FLUX_OK);
        EXPECT(flux_frame_present(frame) == FLUX_OK);
        memset(px, 0xCD, BYTES);
        EXPECT(flux_surface_read_pixels(s, px, BYTES) == FLUX_OK);

        const uint8_t *overlay = &px[(5 * W + 5) * 4];
        EXPECT(overlay[0] < 5 && overlay[1] > 250 && overlay[2] < 5);
        EXPECT(px[(H / 2 * W + 4) * 4] < 16);
        EXPECT(px[(H / 2 * W + (W - 4)) * 4] > 240);
    }

    /* --- reusable blur cycles through and safely reuses frame slots --- */
    for (uint32_t iteration = 0; iteration < 5; ++iteration) {
        flux_frame *frame = nullptr;
        EXPECT(flux_surface_begin_frame(s, nullptr, &frame) == FLUX_OK);

        uint8_t red = (uint8_t)(40u + iteration * 30u);
        flux_color fill = flux_color_rgba(red, 20, 40, 255);
        EXPECT(flux_canvas_begin_target(canvas, frame, target, &fill) == FLUX_OK);
        flux_canvas_end_target(canvas);

        flux_effect_blur_desc bd = FLUX_EFFECT_BLUR_DESC_INIT;
        bd.input = target;
        bd.sigma = 2.0f;
        flux_image *blurred = nullptr;
        EXPECT(flux_blur_filter_apply(blur_filter, frame, &bd, &blurred) == FLUX_OK);
        EXPECT(blurred != nullptr);

        flux_color black = flux_color_rgba(0, 0, 0, 255);
        EXPECT(flux_canvas_begin(canvas, frame, &black) == FLUX_OK);
        flux_canvas_draw_image(canvas, blurred, (flux_rect){0, 0, (float)W, (float)H}, nullptr);
        flux_canvas_end(canvas);
        EXPECT(flux_frame_submit(frame) == FLUX_OK);
        EXPECT(flux_frame_present(frame) == FLUX_OK);
        memset(px, 0, BYTES);
        EXPECT(flux_surface_read_pixels(s, px, BYTES) == FLUX_OK);
        EXPECT(px[(H / 2 * W + W / 2) * 4] >= red - 2u);
    }

    /* --- reusable frame-slot shadow: capture a shape, shadow it, composite,
     * and cycle frame slots without transient-pool leases --- */
    {
        flux_shadow_filter *shadow_filter = nullptr;
        EXPECT(flux_shadow_filter_create(d, &shadow_filter) == FLUX_OK);

        for (uint32_t iteration = 0; iteration < 5; ++iteration) {
            flux_frame *frame = nullptr;
            EXPECT(flux_surface_begin_frame(s, nullptr, &frame) == FLUX_OK);

            /* Capture an opaque white square on transparent: the mask. */
            flux_color transparent = flux_color_rgba(0, 0, 0, 0);
            EXPECT(flux_canvas_begin_target(canvas, frame, target, &transparent) == FLUX_OK);
            flux_canvas_fill_rect_color(canvas, (flux_rect){(float)(W / 4), (float)(H / 4),
                                                            (float)(W / 2), (float)(H / 2)},
                                        flux_color_rgba_premul(255, 255, 255, 255));
            flux_canvas_end_target(canvas);

            flux_effect_shadow_desc sd = FLUX_EFFECT_SHADOW_DESC_INIT;
            sd.input = target;
            sd.blur = 4.0f;
            sd.offset_y = 4.0f;
            sd.tint_red = 0.0f;
            sd.tint_green = 0.0f;
            sd.tint_blue = 0.0f;
            sd.alpha = 1.0f;
            flux_image *shadow = nullptr;
            EXPECT(flux_shadow_filter_apply(shadow_filter, frame, &sd, &shadow) == FLUX_OK);
            EXPECT(shadow != nullptr);

            /* White background so the black shadow is actually visible:
             * inside the offset square the coverage is near-opaque (colour
             * collapses toward black); the far corner keeps near-white. */
            flux_color white = flux_color_rgba(255, 255, 255, 255);
            EXPECT(flux_canvas_begin(canvas, frame, &white) == FLUX_OK);
            flux_canvas_draw_image(canvas, shadow, (flux_rect){0, 0, (float)W, (float)H}, nullptr);
            flux_canvas_end(canvas);
            EXPECT(flux_frame_submit(frame) == FLUX_OK);
            EXPECT(flux_frame_present(frame) == FLUX_OK);

            memset(px, 0, BYTES);
            EXPECT(flux_surface_read_pixels(s, px, BYTES) == FLUX_OK);
            /* The composite target is opaque, so the alpha channel is
             * always 255 — the shadow is visible through the colour
             * channels (black shadow over white mask). Inside the offset
             * square the colour is near-black; the far corner stays
             * near-white. */
            uint8_t centre_r = px[((H / 2 + 4) * W + W / 2) * 4 + 0];
            uint8_t corner_r = px[(2 * W + 2) * 4 + 0];
            EXPECT(centre_r < 55u);
            EXPECT(corner_r > 235u);
        }
        flux_device_wait_idle(d);
        flux_shadow_filter_release(shadow_filter);
    }

    /* --- nesting rejected: begin_target inside an active frame pass --- */
    {
        flux_frame *frame = nullptr;
        EXPECT(flux_surface_begin_frame(s, nullptr, &frame) == FLUX_OK);
        flux_color black = flux_color_rgba(0, 0, 0, 255);
        EXPECT(flux_canvas_begin(canvas, frame, &black) == FLUX_OK);
        EXPECT(flux_canvas_begin_target(canvas, frame, target, &black) == FLUX_ERROR_INVALID_STATE);
        flux_canvas_end(canvas);
        flux_effect_reset(d);
        EXPECT(flux_frame_submit(frame) == FLUX_OK);
        EXPECT(flux_frame_present(frame) == FLUX_OK);
    }

    /* --- downsampled target: render small, then upscale on composite --- */
    {
        flux_image *downsampled = nullptr;
        EXPECT(flux_image_create_render_target(d, W / 2, H / 2, target_fmt, &downsampled) ==
               FLUX_OK);
        flux_frame *frame = nullptr;
        EXPECT(flux_surface_begin_frame(s, nullptr, &frame) == FLUX_OK);
        flux_color red = flux_color_rgba(255, 0, 0, 255);
        EXPECT(flux_canvas_begin_target(canvas, frame, downsampled, &red) == FLUX_OK);
        flux_canvas_end_target(canvas);
        flux_color black = flux_color_rgba(0, 0, 0, 255);
        EXPECT(flux_canvas_begin(canvas, frame, &black) == FLUX_OK);
        flux_canvas_draw_image(canvas, downsampled, (flux_rect){0, 0, (float)W, (float)H}, nullptr);
        flux_canvas_end(canvas);
        EXPECT(flux_frame_submit(frame) == FLUX_OK);
        EXPECT(flux_frame_present(frame) == FLUX_OK);
        memset(px, 0, BYTES);
        EXPECT(flux_surface_read_pixels(s, px, BYTES) == FLUX_OK);
        EXPECT(px[(H / 2 * W + W / 2) * 4] > 240);
        flux_image_release(downsampled);
    }

    /* --- representative HiDPI backdrop capture stays bounded at max sigma --- */
    {
        flux_image *backdrop = nullptr;
        EXPECT(flux_image_create_render_target(d, BACKDROP_W, BACKDROP_H, target_fmt, &backdrop) ==
               FLUX_OK);
        flux_frame *frame = nullptr;
        EXPECT(flux_surface_begin_frame(s, nullptr, &frame) == FLUX_OK);
        flux_color source = flux_color_rgba(35, 70, 105, 255);
        EXPECT(flux_canvas_begin_target(canvas, frame, backdrop, &source) == FLUX_OK);
        flux_canvas_end_target(canvas);

        flux_effect_blur_desc bd = FLUX_EFFECT_BLUR_DESC_INIT;
        bd.input = backdrop;
        bd.sigma = FLUX_EFFECT_BLUR_SIGMA_MAX;
        flux_image *blurred = nullptr;
        EXPECT(flux_blur_filter_apply(blur_filter, frame, &bd, &blurred) == FLUX_OK);

        flux_color black = flux_color_rgba(0, 0, 0, 255);
        EXPECT(flux_canvas_begin(canvas, frame, &black) == FLUX_OK);
        flux_canvas_draw_image(canvas, blurred, (flux_rect){0, 0, (float)W, (float)H}, nullptr);
        flux_canvas_end(canvas);
        EXPECT(flux_frame_submit(frame) == FLUX_OK);
        EXPECT(flux_frame_present(frame) == FLUX_OK);

        memset(px, 0, BYTES);
        EXPECT(flux_surface_read_pixels(s, px, BYTES) == FLUX_OK);
        const uint8_t *centre = &px[(H / 2 * W + W / 2) * 4];
        EXPECT(centre[0] >= 33 && centre[0] <= 37);
        EXPECT(centre[1] >= 68 && centre[1] <= 72);
        EXPECT(centre[2] >= 103 && centre[2] <= 107);
        flux_image_release(backdrop);
    }

    /* --- cross-format capture: sampled BGRA/RGBA normalizes to RGBA storage --- */
    {
        flux_format cross_fmt = target_fmt == FLUX_FORMAT_RGBA8_UNORM ? FLUX_FORMAT_BGRA8_UNORM
                                                                      : FLUX_FORMAT_RGBA8_UNORM;
        flux_image *cross_target = nullptr;
        EXPECT(flux_image_create_render_target(d, W / 2, H / 2, cross_fmt, &cross_target) ==
               FLUX_OK);
        flux_frame *frame = nullptr;
        EXPECT(flux_surface_begin_frame(s, nullptr, &frame) == FLUX_OK);
        flux_color source = flux_color_rgba(11, 22, 33, 255);
        EXPECT(flux_canvas_begin_target(canvas, frame, cross_target, &source) == FLUX_OK);
        flux_canvas_end_target(canvas);

        flux_effect_blur_desc bd = FLUX_EFFECT_BLUR_DESC_INIT;
        bd.input = cross_target;
        bd.sigma = 4.0f;
        flux_image *blurred = nullptr;
        EXPECT(flux_blur_filter_apply(blur_filter, frame, &bd, &blurred) == FLUX_OK);
        EXPECT(flux_image_format(blurred) == FLUX_FORMAT_RGBA8_UNORM);

        flux_color black = flux_color_rgba(0, 0, 0, 255);
        EXPECT(flux_canvas_begin(canvas, frame, &black) == FLUX_OK);
        flux_canvas_draw_image(canvas, blurred, (flux_rect){0, 0, (float)W, (float)H}, nullptr);
        flux_canvas_end(canvas);
        EXPECT(flux_frame_submit(frame) == FLUX_OK);
        EXPECT(flux_frame_present(frame) == FLUX_OK);

        memset(px, 0, BYTES);
        EXPECT(flux_surface_read_pixels(s, px, BYTES) == FLUX_OK);
        const uint8_t *centre = &px[(H / 2 * W + W / 2) * 4];
        EXPECT(centre[0] >= 9 && centre[0] <= 13);
        EXPECT(centre[1] >= 20 && centre[1] <= 24);
        EXPECT(centre[2] >= 31 && centre[2] <= 35);
        flux_image_release(cross_target);
    }

    flux_device_wait_idle(d);
    flux_blur_filter_release(blur_filter);
    flux_image_release(target);
    flux_canvas_destroy(canvas);
    flux_surface_release(s);
    flux_device_release(d);
    TEST_SUMMARY();
}
