/*
 * gfx retire-FIFO backpressure: the zombie queue has a hard entry
 * bound (OPTICS_GFX_RETIRE_MAX_PENDING). Releasing resources while NO frames
 * are submitted freezes the retire watermark, so the FIFO would grow
 * without bound and pin the GPU memory of every parked resource — the
 * exact shape of a host that tears down and rebuilds pools while
 * minimised. Crossing the bound must force a full drain (queue wait +
 * watermark advance + destroy-everything), after which the queue is
 * empty and memory is returned. This test pins that contract without
 * needing to observe private counters: release well past the bound
 * with no frames in flight, then require the allocator to report a
 * steady state identical to before the burst. Skips if no Vulkan
 * device.
 */
#include "gfx_test.h"
#include "optics_test.h"
#include <optics/gfx.h>
#include <optics/gfx/vulkan.h>

#include <string.h>

#define IMG_W 8u
#define IMG_H 8u
/* Far past OPTICS_GFX_RETIRE_MAX_PENDING (4096): enough that a missing
 * backpressure path would leave thousands of zombies parked with
 * their memory pinned, while a working one stays flat after each
 * forced drain. */
#define BURST 16384u

static optics_gfx_image *make_image(optics_gfx_device *d) {
    optics_gfx_image_desc desc = OPTICS_GFX_IMAGE_DESC_INIT;
    desc.width = IMG_W;
    desc.height = IMG_H;
    desc.format = OPTICS_GFX_FORMAT_RGBA8_UNORM;
    optics_gfx_image *out = nullptr;
    return optics_gfx_image_create(d, &desc, &out) == OPTICS_GFX_OK ? out : nullptr;
}

int main(void) {
    optics_gfx_device *d = optics_gfx_test_make_headless_device();
    if (!d) {
        fprintf(stderr, "test_retire_backpressure: no Vulkan device; skipping\n");
        OT_SKIP("no Vulkan device");
    }

    /* Warm up allocator bookkeeping before taking the two steady-state
     * windows. Images intentionally have no initial upload: upload submissions
     * advance the graphics serial and would hide the frozen-watermark case
     * this test exists to exercise. */
    for (uint32_t i = 0; i < 16; ++i) {
        optics_gfx_image *img = make_image(d);
        OT_EXPECT(img != nullptr);
        optics_gfx_image_release(img);
    }
    optics_gfx_device_wait_idle(d);

    /* Window A: one full burst with no frame boundaries. Each release
     * parks a zombie; nothing advances the watermark, so the FIFO can
     * only stay bounded through the forced-drain backpressure. */
    for (uint32_t i = 0; i < BURST; ++i) {
        optics_gfx_image *img = make_image(d);
        OT_EXPECT(img != nullptr);
        optics_gfx_image_release(img);
    }
    optics_gfx_device_wait_idle(d);
    optics_gfx_memory_stats a;
    optics_gfx_device_memory_stats(d, &a);

    /* Window B: identical burst. If zombies accumulated, window B
     * would park BURST more entries and pin their GPU memory; a
     * working backpressure path drains at the bound, so both windows
     * end on the same live set (steady caches only). */
    for (uint32_t i = 0; i < BURST; ++i) {
        optics_gfx_image *img = make_image(d);
        OT_EXPECT(img != nullptr);
        optics_gfx_image_release(img);
    }
    optics_gfx_device_wait_idle(d);
    optics_gfx_memory_stats b;
    optics_gfx_device_memory_stats(d, &b);

    OT_EXPECT(b.live_allocations == a.live_allocations);
    OT_EXPECT(b.bytes_in_use == a.bytes_in_use);
    /* And the steady state is small: device caches only, not thousands
     * of pinned zombie images (8x8 RGBA8 ≈ 256 B each; even all 64 MiB
     * of staging cache would be far below this bound in count terms). */
    OT_EXPECT(b.live_allocations < 256);

    /* The device must still be fully usable afterwards. */
    optics_gfx_image *alive = make_image(d);
    OT_EXPECT(alive != nullptr);
    OT_EXPECT(optics_gfx_image_device(alive) == d);
    optics_gfx_image_release(alive);

    optics_gfx_device_release(d);
    return OT_REPORT();
}
