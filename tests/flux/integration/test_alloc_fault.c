/*
 * Allocator fault-injection test.
 *
 * The test executable defines its own vkAllocateMemory, which ELF
 * symbol resolution prefers over libvulkan's for every library in the
 * process (libflux included). A countdown arms the next N calls to
 * fail with an injected VkResult, letting us drive the GPU-OOM paths
 * on demand without any production-code hooks.
 *
 * Covers:
 *   A. pool-block allocation failure -> FLUX_ERROR_OUT_OF_MEMORY,
 *      structured error info, no leaked accounting, device still healthy
 *   B. reclaim-and-retry: an empty pooled block is handed back to the
 *      driver so a retry after one injected failure SUCCEEDS
 *   C. dedicated-path failure (>= 16 MiB) -> FLUX_ERROR_OUT_OF_MEMORY
 *      after both the attempt and the reclaim-retry are failed
 */
#include "test_helpers.h"
#include <flux/flux.h>
#include <flux/vulkan.h>

#include <dlfcn.h>
#include <stdatomic.h>

static atomic_int g_fail_remaining;
static atomic_uint g_injected_calls;

VKAPI_ATTR VkResult VKAPI_CALL vkAllocateMemory(VkDevice device,
                                                const VkMemoryAllocateInfo *pAllocateInfo,
                                                const VkAllocationCallbacks *pAllocator,
                                                VkDeviceMemory *pMemory) {
    static PFN_vkAllocateMemory real_fn;
    if (!real_fn)
        *(void **)(&real_fn) = dlsym(RTLD_NEXT, "vkAllocateMemory");

    int remaining = atomic_load_explicit(&g_fail_remaining, memory_order_acquire);
    while (remaining > 0 &&
           !atomic_compare_exchange_weak_explicit(&g_fail_remaining, &remaining, remaining - 1,
                                                  memory_order_acq_rel, memory_order_acquire)) {
    }
    if (remaining > 0) {
        atomic_fetch_add_explicit(&g_injected_calls, 1, memory_order_relaxed);
        return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    }
    return real_fn(device, pAllocateInfo, pAllocator, pMemory);
}

static void fail_next(int n) {
    atomic_store_explicit(&g_fail_remaining, n, memory_order_release);
}

int main(void) {
    flux_device *d = test_helpers_make_headless_device();
    if (!d) {
        fprintf(stderr, "test_alloc_fault: no Vulkan device available; skipping\n");
        return 0;
    }

    /* --- A: pool-block vkAllocateMemory failure is reported as
     *     FLUX_ERROR_OUT_OF_MEMORY with the VkResult preserved, leaves
     *     the allocator accounting untouched, and the device recovers
     *     for the very next request. --- */
    {
        flux_memory_stats before;
        flux_device_memory_stats(d, &before);

        fail_next(1);
        flux_buffer_desc bd = FLUX_BUFFER_DESC_INIT;
        bd.size = 256;
        bd.usage = FLUX_BUFFER_USAGE_UNIFORM;
        bd.location = FLUX_BUFFER_HOST_VISIBLE;
        flux_buffer *b = NULL;
        EXPECT(flux_buffer_create(d, &bd, &b) == FLUX_ERROR_OUT_OF_MEMORY);
        EXPECT(b == NULL);

        flux_error_info err;
        flux_get_last_error(&err);
        EXPECT(err.code == FLUX_ERROR_OUT_OF_MEMORY);
        EXPECT(err.backend_code == VK_ERROR_OUT_OF_DEVICE_MEMORY);

        flux_memory_stats after;
        flux_device_memory_stats(d, &after);
        EXPECT(after.live_allocations == before.live_allocations);
        EXPECT(after.bytes_in_use == before.bytes_in_use);

        /* No injection left: the same request succeeds. */
        EXPECT(flux_buffer_create(d, &bd, &b) == FLUX_OK);
        flux_buffer_release(b);
    }

    /* --- B: reclaim-and-retry. Park an empty pooled block (create a
     *     buffer, release it, then sweep the retire zombie via an
     *     image upload). An image allocate then needs a fresh pool
     *     block; the first attempt fails (injected), the allocator
     *     reclaims the empty buffer block, and the retry must SUCCEED. --- */
    {
        flux_buffer_desc bd = FLUX_BUFFER_DESC_INIT;
        bd.size = 256;
        bd.usage = FLUX_BUFFER_USAGE_UNIFORM;
        bd.location = FLUX_BUFFER_HOST_VISIBLE;
        flux_buffer *empty = NULL;
        EXPECT(flux_buffer_create(d, &bd, &empty) == FLUX_OK);
        flux_buffer_release(empty);

        /* Advance the graphics serial past the buffer's retire zombie:
         * a real upload's fence wait sweeps it, emptying the block. */
        flux_image_desc sweep_desc = {
            .type = FLUX_TYPE_IMAGE_DESC,
            .width = 8,
            .height = 8,
            .format = FLUX_FORMAT_RGBA8_UNORM,
        };
        flux_image *sweeper = NULL;
        EXPECT(flux_image_create(d, &sweep_desc, &sweeper) == FLUX_OK);

        flux_memory_stats before;
        flux_device_memory_stats(d, &before);

        fail_next(1);
        flux_image *img = NULL;
        flux_result r = flux_image_create(d, &sweep_desc, &img);
        EXPECT(r == FLUX_OK); /* retry after reclaim recovered */
        EXPECT(img != NULL);
        EXPECT(atomic_load(&g_injected_calls) > 0);

        /* No unbounded growth: the retried block replaced the reclaimed
         * one rather than piling on. */
        flux_memory_stats after;
        flux_device_memory_stats(d, &after);
        EXPECT(after.live_allocations == before.live_allocations + 1);

        flux_image_release(img);
        flux_image_release(sweeper);
    }

    /* --- C: dedicated-path failure. A >= 16 MiB buffer bypasses the
     *     pool; failing both the first attempt and the reclaim-retry
     *     must surface FLUX_ERROR_OUT_OF_MEMORY with clean accounting. --- */
    {
        flux_memory_stats before;
        flux_device_memory_stats(d, &before);

        fail_next(2);
        flux_buffer_desc bd = FLUX_BUFFER_DESC_INIT;
        bd.size = 16u * 1024 * 1024;
        bd.usage = FLUX_BUFFER_USAGE_STORAGE;
        bd.location = FLUX_BUFFER_GPU_LOCAL;
        flux_buffer *big = NULL;
        EXPECT(flux_buffer_create(d, &bd, &big) == FLUX_ERROR_OUT_OF_MEMORY);
        EXPECT(big == NULL);

        flux_memory_stats after;
        flux_device_memory_stats(d, &after);
        EXPECT(after.live_allocations == before.live_allocations);
        EXPECT(after.bytes_in_use == before.bytes_in_use);

        /* Healthy again afterwards. */
        EXPECT(flux_buffer_create(d, &bd, &big) == FLUX_OK);
        flux_buffer_release(big);
    }

    flux_device_release(d);
    TEST_SUMMARY();
}
