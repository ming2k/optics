/*
 * Memory budget query test (VK_EXT_memory_budget).
 *
 * The device now enables VK_EXT_memory_budget whenever the driver
 * advertises it, so flux_device_memory_budget must report per-heap
 * usage/budget on supporting drivers and degrade cleanly elsewhere.
 * Runs on any ICD; skips gracefully when no Vulkan device exists.
 */
#include "flux_test.h"
#include "optics_test.h"
#include <flux/flux.h>

int main(void) {
    flux_device *d = flux_test_make_headless_device();
    if (!d) {
        fprintf(stderr, "test_memory_budget: no Vulkan device available; skipping\n");
        OT_SKIP("no Vulkan device");
    }

    flux_memory_budget mb;
    flux_device_memory_budget(d, &mb);

    OT_EXPECT(mb.heap_count > 0);
    OT_EXPECT(mb.heap_count <= FLUX_MAX_MEMORY_HEAPS);

    uint64_t total_sum = 0;
    for (uint32_t i = 0; i < mb.heap_count; ++i)
        total_sum += mb.heap_bytes_total[i];
    OT_EXPECT(total_sum > 0);

    if (mb.has_budget_extension) {
        /* With the extension enabled the driver must fill in real
         * numbers, and reported usage must stay within the budget it
         * advertised (the allocator's budget gate reclaims before
         * crossing it). */
        for (uint32_t i = 0; i < mb.heap_count; ++i) {
            OT_EXPECT(mb.heap_budget[i] > 0);
            OT_EXPECT(mb.heap_budget[i] <= mb.heap_bytes_total[i]);
            OT_EXPECT(mb.heap_bytes_used[i] <= mb.heap_budget[i]);
        }
    } else {
        /* No extension: usage/budget must read as zero, never garbage. */
        for (uint32_t i = 0; i < mb.heap_count; ++i) {
            OT_EXPECT(mb.heap_bytes_used[i] == 0);
            OT_EXPECT(mb.heap_budget[i] == 0);
        }
    }

    /* NULL is tolerated. */
    flux_device_memory_budget(d, nullptr);
    flux_device_memory_budget(nullptr, &mb);

    flux_device_release(d);
    return OT_REPORT();
}
