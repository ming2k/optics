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
#include "../test_helpers.h"
#include <flux/canvas.h>
#include <flux/core.h>
#include <flux/flux.h>
#include <flux/vulkan.h>

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
    flux_device *d = test_helpers_make_headless_device();
    if (!d) {
        fprintf(stderr, "bench_frame_loop: no Vulkan device; skipping\n");
        return 0;
    }

    flux_surface_desc sd = FLUX_SURFACE_DESC_INIT;
    sd.width = W;
    sd.height = H;
    sd.vsync = false;
    flux_surface *s = nullptr;
    if (flux_surface_create(d, &sd, &s) != FLUX_OK) {
        flux_device_release(d);
        return 0;
    }

    VkFormat sfmt = flux_surface_vk_format(s);
    flux_format tf =
        (sfmt == VK_FORMAT_B8G8R8A8_UNORM) ? FLUX_FORMAT_BGRA8_UNORM : FLUX_FORMAT_RGBA8_UNORM;

    flux_canvas_desc cd = {.type = FLUX_TYPE_CANVAS_DESC, .surface = s};
    flux_canvas *canvas = nullptr;
    if (flux_canvas_create(&cd, &canvas) != FLUX_OK) {
        flux_surface_release(s);
        flux_device_release(d);
        return 0;
    }

    flux_image *targets[TARGETS] = {nullptr};
    for (uint32_t i = 0; i < TARGETS; ++i)
        (void)flux_image_create_render_target(d, W / 3, H / 3, tf, &targets[i]);

    /* Warm-up: shader/pipeline compilation and first-touch allocations
     * must not pollute the measurement. */
    for (uint32_t i = 0; i < 10u; ++i) {
        flux_frame *f = nullptr;
        (void)flux_surface_begin_frame(s, nullptr, &f);
        flux_canvas_pass_desc pd = FLUX_CANVAS_PASS_DESC_INIT;
        flux_color clear = flux_color_rgba(0, 0, 0, 255);
        pd.clear_color = &clear;
        (void)flux_canvas_begin_pass(canvas, f, &pd);
        flux_canvas_fill_rect_color(canvas, (flux_rect){0, 0, (float)W, (float)H},
                                    flux_color_rgba(64, 64, 77, 255));
        flux_canvas_end(canvas);
        (void)flux_frame_submit(f);
        (void)flux_frame_present(f);
    }
    flux_device_wait_idle(d);

    double start = now_ms();
    for (uint32_t frame_index = 0; frame_index < FRAMES; ++frame_index) {
        flux_frame *f = nullptr;
        (void)flux_surface_begin_frame(s, nullptr, &f);

        /* Two capture passes (a reveal animation sweeps the capture rect). */
        for (uint32_t t = 0; t < TARGETS; ++t) {
            float reveal = 0.2f + 0.8f * ((float)(frame_index % 60) / 60.0f);
            flux_canvas_pass_desc pd = FLUX_CANVAS_PASS_DESC_INIT;
            flux_color clear = flux_color_rgba(0, 0, 0, 0);
            pd.clear_color = &clear;
            (void)flux_canvas_begin_target_pass(canvas, f, targets[t], &pd);
            flux_canvas_fill_rect_color(
                canvas, (flux_rect){0, 0, (float)W / 3 * reveal, (float)H / 3},
                flux_color_rgba(204, 153, 102, 230));
            flux_canvas_end_target(canvas);
        }

        /* Main composite pass. */
        flux_canvas_pass_desc pd = FLUX_CANVAS_PASS_DESC_INIT;
        flux_color clear = flux_color_rgba(0, 0, 0, 255);
        pd.clear_color = &clear;
        (void)flux_canvas_begin_pass(canvas, f, &pd);
        flux_canvas_fill_rect_color(canvas, (flux_rect){0, 0, (float)W, (float)H},
                                    flux_color_rgba(64, 64, 77, 255));
        for (uint32_t t = 0; t < TARGETS; ++t)
            flux_canvas_draw_image(canvas, targets[t],
                                   (flux_rect){(float)(t * 40), 120.0f, (float)W / 3,
                                               (float)H / 3},
                                   nullptr);
        flux_canvas_end(canvas);

        (void)flux_frame_submit(f);
        (void)flux_frame_present(f);
    }
    double elapsed = now_ms() - start;
    flux_device_wait_idle(d);

    printf("frame_loop: %u frames in %.1f ms (%.3f ms/frame, %.1f fps)\n", FRAMES, elapsed,
           elapsed / (double)FRAMES, (double)FRAMES * 1000.0 / elapsed);

    for (uint32_t i = 0; i < TARGETS; ++i)
        flux_image_release(targets[i]);
    flux_canvas_destroy(canvas);
    flux_surface_release(s);
    flux_device_release(d);
    return 0;
}
