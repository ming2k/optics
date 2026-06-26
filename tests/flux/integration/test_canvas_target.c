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

    static uint8_t px[BYTES];

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
        VkCommandBuffer cmd = flux_frame_vk_command_buffer(frame);
        EXPECT(flux_effect_blur(cmd, &bd, &blurred) == FLUX_OK);
        EXPECT(blurred != nullptr);

        /* Composite the blurred capture onto the frame. */
        flux_color fc = flux_color_rgba(0, 0, 0, 255);
        EXPECT(flux_canvas_begin(canvas, frame, &fc) == FLUX_OK);
        flux_canvas_draw_image(canvas, blurred, (flux_rect){0, 0, (float)W, (float)H}, nullptr);
        flux_canvas_end(canvas);

        flux_effect_reset(d);
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
        printf("capture+blur+composite edge: left=%u right=%u (softened)\n", edge_left, edge_right);
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

    /* --- validation: extent mismatch rejected --- */
    {
        flux_image *bad = nullptr;
        EXPECT(flux_image_create_render_target(d, W / 2, H, target_fmt, &bad) == FLUX_OK);
        flux_frame *frame = nullptr;
        EXPECT(flux_surface_begin_frame(s, nullptr, &frame) == FLUX_OK);
        flux_color black = flux_color_rgba(0, 0, 0, 255);
        EXPECT(flux_canvas_begin_target(canvas, frame, bad, &black) == FLUX_ERROR_INVALID_ARGUMENT);
        EXPECT(flux_frame_submit(frame) == FLUX_OK);
        EXPECT(flux_frame_present(frame) == FLUX_OK);
        flux_image_release(bad);
    }

    flux_image_release(target);
    flux_canvas_destroy(canvas);
    flux_surface_release(s);
    flux_device_release(d);
    TEST_SUMMARY();
}
