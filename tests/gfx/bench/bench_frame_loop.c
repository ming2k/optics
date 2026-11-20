/*
 * GPU frame-loop benchmark: a compositor-shaped workload.
 *
 * Per frame it renders one full-surface pass plus two render-target
 * capture passes — the shape of a backdrop-blur compositor animating its
 * panels (main output composite + per-region capture + the composited
 * effect). Measures wall time per frame at the headless surface's present
 * cadence, which for a MAILBOX/IMMEDIATE headless swapchain is bound by
 * the GPU work itself, so deltas in this benchmark track deltas in per-
 * frame GPU cost.
 *
 * Output is plain numbers for CI parsing (suite 'bench'; no pass/fail).
 */
#include "gfx_test.h"
#include "optics_test.h"
#include <optics/gfx.h>
#include <optics/gfx/canvas.h>
#include <optics/gfx/core.h>
#include <optics/gfx/vulkan.h>

#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#define W 1920u
#define H 1080u
#define FRAMES 200u
#define TARGETS 2u

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

int main(void) {
    optics_gfx_device *d = optics_gfx_test_make_headless_device();
    if (d == nullptr) {
        fprintf(stderr, "bench_frame_loop: no Vulkan device; skipping\n");
        return 0;
    }

    optics_gfx_surface_desc sd = OPTICS_GFX_SURFACE_DESC_INIT;
    sd.width = W;
    sd.height = H;
    sd.vsync = false;
    optics_gfx_surface *s = nullptr;
    if (optics_gfx_surface_create(d, &sd, &s) != OPTICS_GFX_OK) {
        optics_gfx_device_release(d);
        return 0;
    }

    VkFormat sfmt = optics_gfx_surface_vk_format(s);
    optics_gfx_format tf = (sfmt == VK_FORMAT_B8G8R8A8_UNORM) ? OPTICS_GFX_FORMAT_BGRA8_UNORM
                                                              : OPTICS_GFX_FORMAT_RGBA8_UNORM;

    optics_gfx_canvas_desc cd = {.type = OPTICS_GFX_TYPE_CANVAS_DESC, .surface = s};
    optics_gfx_canvas *canvas = nullptr;
    if (optics_gfx_canvas_create(&cd, &canvas) != OPTICS_GFX_OK) {
        optics_gfx_surface_release(s);
        optics_gfx_device_release(d);
        return 0;
    }

    optics_gfx_image *targets[TARGETS] = {};
    for (uint32_t i = 0; i < TARGETS; ++i)
        (void)optics_gfx_image_create(
            d,
            &(optics_gfx_image_desc){.type = OPTICS_GFX_TYPE_IMAGE_DESC,
                                     .width = W / 3,
                                     .height = H / 3,
                                     .format = tf,
                                     .usage = OPTICS_GFX_IMAGE_USAGE_RENDER_TARGET},
            &targets[i]);

    /* Warm-up: shader/pipeline compilation and first-touch allocations
     * must not pollute the measurement. */
    for (uint32_t i = 0; i < 10u; ++i) {
        optics_gfx_frame *f = nullptr;
        (void)optics_gfx_surface_begin_frame(s, nullptr, &f);
        optics_gfx_canvas_pass_desc pd = OPTICS_GFX_CANVAS_PASS_DESC_INIT;
        optics_gfx_color clear = optics_gfx_color_rgba(0, 0, 0, 255);
        pd.clear_color = &clear;
        pd.frame = f;
        (void)optics_gfx_canvas_begin(canvas, &pd);
        optics_gfx_canvas_fill_rect_color(canvas, (optics_gfx_rect){0, 0, (float)W, (float)H},
                                          optics_gfx_color_rgba(64, 64, 77, 255));
        (void)optics_gfx_canvas_end(canvas);
        (void)optics_gfx_frame_submit(f);
        (void)optics_gfx_frame_present(f);
    }
    optics_gfx_device_wait_idle(d);

    double start = now_ms();
    for (uint32_t frame_index = 0; frame_index < FRAMES; ++frame_index) {
        optics_gfx_frame *f = nullptr;
        (void)optics_gfx_surface_begin_frame(s, nullptr, &f);

        /* Two capture passes (a reveal animation sweeps the capture rect). */
        for (uint32_t t = 0; t < TARGETS; ++t) {
            float reveal = 0.2f + 0.8f * ((float)(frame_index % 60) / 60.0f);
            optics_gfx_canvas_pass_desc pd = OPTICS_GFX_CANVAS_PASS_DESC_INIT;
            optics_gfx_color clear = optics_gfx_color_rgba(0, 0, 0, 0);
            pd.clear_color = &clear;
            pd.frame = f;
            pd.attachment = (optics_gfx_canvas_attachment){
                .kind = OPTICS_GFX_CANVAS_ATTACHMENT_IMAGE, .image = targets[t]};
            (void)optics_gfx_canvas_begin(canvas, &pd);
            optics_gfx_canvas_fill_rect_color(
                canvas, (optics_gfx_rect){0, 0, (float)W / 3 * reveal, (float)H / 3},
                optics_gfx_color_rgba(204, 153, 102, 230));
            (void)optics_gfx_canvas_end(canvas);
        }

        /* Main composite pass. */
        optics_gfx_canvas_pass_desc pd = OPTICS_GFX_CANVAS_PASS_DESC_INIT;
        optics_gfx_color clear = optics_gfx_color_rgba(0, 0, 0, 255);
        pd.clear_color = &clear;
        pd.frame = f;
        (void)optics_gfx_canvas_begin(canvas, &pd);
        optics_gfx_canvas_fill_rect_color(canvas, (optics_gfx_rect){0, 0, (float)W, (float)H},
                                          optics_gfx_color_rgba(64, 64, 77, 255));
        for (uint32_t t = 0; t < TARGETS; ++t)
            optics_gfx_canvas_draw_image(
                canvas, targets[t],
                (optics_gfx_rect){(float)(t * 40), 120.0f, (float)W / 3, (float)H / 3}, nullptr);
        (void)optics_gfx_canvas_end(canvas);

        (void)optics_gfx_frame_submit(f);
        (void)optics_gfx_frame_present(f);
    }
    double elapsed = now_ms() - start;
    optics_gfx_device_wait_idle(d);

    printf("frame_loop: %u frames in %.1f ms (%.3f ms/frame, %.1f fps)\n", FRAMES, elapsed,
           elapsed / (double)FRAMES, (double)FRAMES * 1000.0 / elapsed);

    for (uint32_t i = 0; i < TARGETS; ++i)
        optics_gfx_image_release(targets[i]);
    optics_gfx_canvas_release(canvas);
    optics_gfx_surface_release(s);
    optics_gfx_device_release(d);
    return 0;
}
