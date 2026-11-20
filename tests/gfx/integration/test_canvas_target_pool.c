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
#include "gfx_test.h"
#include "optics_test.h"
#include <optics/gfx.h>
#include <optics/gfx/vulkan.h>

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

static void run_sweep(optics_gfx_device *d, optics_gfx_surface *s, optics_gfx_canvas *canvas,
                      optics_gfx_format target_fmt) {
    for (size_t i = 0; i < SWEEP_LEN; ++i) {
        optics_gfx_image *target = nullptr;
        OT_EXPECT(optics_gfx_image_create(
                      d,
                      &(optics_gfx_image_desc){.type = OPTICS_GFX_TYPE_IMAGE_DESC,
                                               .width = sweep[i],
                                               .height = 384,
                                               .format = target_fmt,
                                               .usage = OPTICS_GFX_IMAGE_USAGE_RENDER_TARGET},
                      &target) == OPTICS_GFX_OK);
        for (uint32_t slot = 0; slot < SLOTS; ++slot) {
            optics_gfx_frame *frame = nullptr;
            OT_EXPECT(optics_gfx_surface_begin_frame(s, nullptr, &frame) == OPTICS_GFX_OK);
            optics_gfx_canvas_pass_desc pd = OPTICS_GFX_CANVAS_PASS_DESC_INIT;
            optics_gfx_color clear = optics_gfx_color_rgba(26, 51, 77, 255);
            pd.clear_color = &clear;
            pd.frame = frame;
            pd.attachment = (optics_gfx_canvas_attachment){
                .kind = OPTICS_GFX_CANVAS_ATTACHMENT_IMAGE, .image = target};
            OT_EXPECT(optics_gfx_canvas_begin(canvas, &pd) == OPTICS_GFX_OK);
            optics_gfx_canvas_fill_rect_color(canvas,
                                              (optics_gfx_rect){0, 0, (float)sweep[i], 384.0f},
                                              optics_gfx_color_rgba(230, 128, 51, 255));
            OT_EXPECT(optics_gfx_canvas_end(canvas) == OPTICS_GFX_OK);
            OT_EXPECT(optics_gfx_frame_submit(frame) == OPTICS_GFX_OK);
            OT_EXPECT(optics_gfx_frame_present(frame) == OPTICS_GFX_OK);
        }
        optics_gfx_image_release(target);
    }
    optics_gfx_device_wait_idle(d);
}

int main(void) {
    optics_gfx_device *d = optics_gfx_test_make_headless_device();
    if (!d) {
        fprintf(stderr, "test_canvas_target_pool: no Vulkan device; skipping\n");
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

    /* Baseline: the device, surface, and canvas alone. */
    optics_gfx_device_wait_idle(d);
    optics_gfx_memory_stats baseline;
    optics_gfx_device_memory_stats(d, &baseline);

    run_sweep(d, s, canvas, target_fmt);
    run_sweep(d, s, canvas, target_fmt);

    optics_gfx_memory_stats after;
    optics_gfx_device_memory_stats(d, &after);

    uint32_t parked = after.live_allocations - baseline.live_allocations;
    /* Bucketed: 2 buckets/slot x 3 slots, each parking a single linear
     * intermediate image, plus the retired-but-not-drained sweep targets'
     * own images: those are released per iteration and drained by the stats
     * call, so the steady count is the attachment pool alone. Exact-dimension
     * behavior parks 16/slot; the bucketed bound stays well below that. */
    fprintf(stderr, "  attachment pool parked %u live allocations (baseline %u, after %u)\n",
            parked, baseline.live_allocations, after.live_allocations);
    OT_EXPECT(parked < 2u * SLOTS * 4u);

    optics_gfx_canvas_release(canvas);
    optics_gfx_surface_release(s);
    optics_gfx_device_release(d);
    return OT_REPORT();
}
