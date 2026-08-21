/*
 * Regression: the bucketed target-attachment pool must absorb a
 * size-sweeping animation. A capture pass whose extent walks a few pixels
 * per frame (window resize, reveal animation, window stream) used to miss
 * the per-exact-dimension pool on every frame: with the pool capped at 16
 * entries per slot, a sweep visiting more than 16 distinct extents leaves
 * 16 full entries parked per slot — one pooled RGBA16F intermediate plus
 * stencil per visited size, tens of megabytes on HiDPI — and once the cap
 * is reached every further step evicts and re-creates images.
 *
 * Entries are now keyed by 128-pixel buckets, so a sweep across 21 extents
 * that all fall inside 3 buckets must park only 3 entries per slot. The
 * allocator's live-allocation count is the observable: unlike slab-pooled
 * bytes, it counts one allocation per canvas-owned image (linear, stencil,
 * msaa variants), so the difference between 3 and 16 parked entries is a
 * multiple of images per slot, far outside any slack the allocator adds.
 *
 * Asserts the bucketed steady state is materially smaller than the exact-
 * dimension steady state would be: fewer than half the allocations of the
 * sweep's distinct extents times slots. With 21 extents x 3 slots the old
 * behavior parks 16 entries x 3 slots; 3 buckets x 3 slots must park well
 * under that.
 */
#include "test_helpers.h"
#include <flux/flux.h>
#include <flux/vulkan.h>

#include <stdio.h>
#include <stdlib.h>

#define W 512u
#define H 512u
/* 21 distinct widths; old code parked 16 (cap) per slot, new code parks 2
 * buckets (300..340 -> 384) per slot. */
static const uint32_t sweep[] = {300, 302, 304, 306, 308, 310, 312, 314, 316, 318, 320,
                                 322, 324, 326, 328, 330, 332, 334, 336, 338, 340};
#define SWEEP_LEN (sizeof(sweep) / sizeof(sweep[0]))
#define SLOTS 3u

static void run_sweep(flux_device *d, flux_surface *s, flux_canvas *canvas,
                      flux_format target_fmt) {
    for (size_t i = 0; i < SWEEP_LEN; ++i) {
        flux_image *target = nullptr;
        EXPECT(flux_image_create_render_target(d, sweep[i], 384, target_fmt, &target) == FLUX_OK);
        for (uint32_t slot = 0; slot < SLOTS; ++slot) {
            flux_frame *frame = nullptr;
            EXPECT(flux_surface_begin_frame(s, nullptr, &frame) == FLUX_OK);
            flux_canvas_pass_desc pd = FLUX_CANVAS_PASS_DESC_INIT;
            flux_color clear = flux_color_rgba(26, 51, 77, 255);
            pd.clear_color = &clear;
            EXPECT(flux_canvas_begin_target_pass(canvas, frame, target, &pd) == FLUX_OK);
            flux_canvas_fill_rect_color(canvas, (flux_rect){0, 0, (float)sweep[i], 384.0f},
                                        flux_color_rgba(230, 128, 51, 255));
            flux_canvas_end_target(canvas);
            EXPECT(flux_frame_submit(frame) == FLUX_OK);
            EXPECT(flux_frame_present(frame) == FLUX_OK);
        }
        flux_image_release(target);
    }
    flux_device_wait_idle(d);
}

int main(void) {
    flux_device *d = test_helpers_make_headless_device();
    if (!d) {
        fprintf(stderr, "test_canvas_target_pool: no Vulkan device; skipping\n");
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

    /* Baseline: the device, surface, and canvas alone. */
    flux_device_wait_idle(d);
    flux_memory_stats baseline;
    flux_device_memory_stats(d, &baseline);

    run_sweep(d, s, canvas, target_fmt);
    run_sweep(d, s, canvas, target_fmt);

    flux_memory_stats after;
    flux_device_memory_stats(d, &after);

    uint32_t parked = after.live_allocations - baseline.live_allocations;
    /* Bucketed: 2 buckets/slot x 3 slots, each parking a linear and a
     * stencil image (msaa never engaged — clear + antialias default), plus
     * the retired-but-not-drained sweep targets' own images: those are
     * released per iteration and drained by the stats call, so the steady
     * count is the attachment pool alone. Exact-dimension behavior parks
     * 16/slot; the bucketed bound keeps an order of magnitude below that. */
    fprintf(stderr, "  attachment pool parked %u live allocations (baseline %u, after %u)\n",
            parked, baseline.live_allocations, after.live_allocations);
    EXPECT(parked < 2u * SLOTS * 4u);

    flux_canvas_destroy(canvas);
    flux_surface_release(s);
    flux_device_release(d);
    TEST_SUMMARY();
}
