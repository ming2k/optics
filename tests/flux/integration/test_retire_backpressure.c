/*
 * flux retire-FIFO backpressure: the zombie queue has a hard entry
 * bound (FLUX_RETIRE_MAX_PENDING). Releasing resources while NO frames
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
#include "test_helpers.h"
#include <flux/flux.h>
#include <flux/vulkan.h>

#include <string.h>

#define IMG_W 8u
#define IMG_H 8u
/* Far past FLUX_RETIRE_MAX_PENDING (4096): enough that a missing
 * backpressure path would leave thousands of zombies parked with
 * their memory pinned, while a working one stays flat after each
 * forced drain. */
#define BURST 16384u

static flux_image *make_image(flux_device *d) {
    flux_image_desc desc = FLUX_IMAGE_DESC_INIT;
    desc.width = IMG_W;
    desc.height = IMG_H;
    desc.format = FLUX_FORMAT_RGBA8_UNORM;
    flux_image *out = nullptr;
    return flux_image_create(d, &desc, &out) == FLUX_OK ? out : nullptr;
}

int main(void) {
    flux_device *d = test_helpers_make_headless_device();
    if (!d) {
        fprintf(stderr, "test_retire_backpressure: no Vulkan device; skipping\n");
        TEST_SUMMARY();
    }

    /* Warm up allocator bookkeeping before taking the two steady-state
     * windows. Images intentionally have no initial upload: upload submissions
     * advance the graphics serial and would hide the frozen-watermark case
     * this test exists to exercise. */
    for (uint32_t i = 0; i < 16; ++i) {
        flux_image *img = make_image(d);
        EXPECT(img != nullptr);
        flux_image_release(img);
    }
    flux_device_wait_idle(d);

    /* Window A: one full burst with no frame boundaries. Each release
     * parks a zombie; nothing advances the watermark, so the FIFO can
     * only stay bounded through the forced-drain backpressure. */
    for (uint32_t i = 0; i < BURST; ++i) {
        flux_image *img = make_image(d);
        EXPECT(img != nullptr);
        flux_image_release(img);
    }
    flux_device_wait_idle(d);
    flux_memory_stats a;
    flux_device_memory_stats(d, &a);

    /* Window B: identical burst. If zombies accumulated, window B
     * would park BURST more entries and pin their GPU memory; a
     * working backpressure path drains at the bound, so both windows
     * end on the same live set (steady caches only). */
    for (uint32_t i = 0; i < BURST; ++i) {
        flux_image *img = make_image(d);
        EXPECT(img != nullptr);
        flux_image_release(img);
    }
    flux_device_wait_idle(d);
    flux_memory_stats b;
    flux_device_memory_stats(d, &b);

    EXPECT(b.live_allocations == a.live_allocations);
    EXPECT(b.bytes_in_use == a.bytes_in_use);
    /* And the steady state is small: device caches only, not thousands
     * of pinned zombie images (8x8 RGBA8 ≈ 256 B each; even all 64 MiB
     * of staging cache would be far below this bound in count terms). */
    EXPECT(b.live_allocations < 256);

    /* The device must still be fully usable afterwards. */
    flux_image *alive = make_image(d);
    EXPECT(alive != nullptr);
    EXPECT(flux_image_device(alive) == d);
    flux_image_release(alive);

    flux_device_release(d);
    TEST_SUMMARY();
}
