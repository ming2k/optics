/*
 * Allocator fault-injection test.
 *
 * The test executable defines its own vkAllocateMemory, which ELF
 * symbol resolution prefers over libvulkan's for every library in the
 * process (liboptics_gfx included). A countdown arms the next N calls to
 * fail with an injected VkResult, letting us drive the GPU-OOM paths
 * on demand without any production-code hooks.
 *
 * Covers:
 *   A. pool-block allocation failure -> OPTICS_GFX_ERROR_OUT_OF_MEMORY,
 *      structured error info, no leaked accounting, device still healthy
 *   B. reclaim-and-retry: an empty pooled block is handed back to the
 *      driver so a retry after one injected failure SUCCEEDS
 *   C. dedicated-path failure (>= 16 MiB) -> OPTICS_GFX_ERROR_OUT_OF_MEMORY
 *      after both the attempt and the reclaim-retry are failed
 */
#include "gfx_test.h"
#include "optics_test.h"
#include <optics/gfx.h>
#include <optics/gfx/vulkan.h>

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
    optics_gfx_device *d = optics_gfx_test_make_headless_device();
    if (!d) {
        fprintf(stderr, "test_alloc_fault: no Vulkan device available; skipping\n");
        OT_SKIP("no Vulkan device");
    }

    /* --- A: pool-block vkAllocateMemory failure is reported as
     *     OPTICS_GFX_ERROR_OUT_OF_MEMORY with the VkResult preserved, leaves
     *     the allocator accounting untouched, and the device recovers
     *     for the very next request. --- */
    {
        optics_gfx_memory_stats before;
        optics_gfx_device_memory_stats(d, &before);

        fail_next(1);
        optics_gfx_buffer_desc bd = OPTICS_GFX_BUFFER_DESC_INIT;
        bd.size = 256;
        bd.usage = OPTICS_GFX_BUFFER_USAGE_UNIFORM;
        bd.location = OPTICS_GFX_BUFFER_HOST_VISIBLE;
        optics_gfx_buffer *b = nullptr;
        OT_EXPECT(optics_gfx_buffer_create(d, &bd, &b) == OPTICS_GFX_ERROR_OUT_OF_MEMORY);
        OT_EXPECT(b == nullptr);

        optics_gfx_error_info err;
        optics_gfx_get_last_error(&err);
        OT_EXPECT(err.code == OPTICS_GFX_ERROR_OUT_OF_MEMORY);
        OT_EXPECT(err.backend_code == VK_ERROR_OUT_OF_DEVICE_MEMORY);

        optics_gfx_memory_stats after;
        optics_gfx_device_memory_stats(d, &after);
        OT_EXPECT(after.live_allocations == before.live_allocations);
        OT_EXPECT(after.bytes_in_use == before.bytes_in_use);

        /* No injection left: the same request succeeds. */
        OT_EXPECT(optics_gfx_buffer_create(d, &bd, &b) == OPTICS_GFX_OK);
        optics_gfx_buffer_release(b);
    }

    /* --- B: reclaim-and-retry. Park an empty pooled block (create a
     *     buffer, release it, then sweep the retire zombie via an
     *     image upload). An image allocate then needs a fresh pool
     *     block; the first attempt fails (injected), the allocator
     *     reclaims the empty buffer block, and the retry must SUCCEED. --- */
    {
        optics_gfx_buffer_desc bd = OPTICS_GFX_BUFFER_DESC_INIT;
        bd.size = 256;
        bd.usage = OPTICS_GFX_BUFFER_USAGE_UNIFORM;
        bd.location = OPTICS_GFX_BUFFER_HOST_VISIBLE;
        optics_gfx_buffer *empty = nullptr;
        OT_EXPECT(optics_gfx_buffer_create(d, &bd, &empty) == OPTICS_GFX_OK);
        optics_gfx_buffer_release(empty);

        /* Advance the graphics serial past the buffer's retire zombie:
         * a real upload's fence wait sweeps it, emptying the block. */
        optics_gfx_image_desc sweep_desc = {
            .type = OPTICS_GFX_TYPE_IMAGE_DESC,
            .width = 8,
            .height = 8,
            .format = OPTICS_GFX_FORMAT_RGBA8_UNORM,
        };
        optics_gfx_image *sweeper = nullptr;
        OT_EXPECT(optics_gfx_image_create(d, &sweep_desc, &sweeper) == OPTICS_GFX_OK);

        optics_gfx_memory_stats before;
        optics_gfx_device_memory_stats(d, &before);

        fail_next(1);
        optics_gfx_image *img = nullptr;
        optics_gfx_result r = optics_gfx_image_create(d, &sweep_desc, &img);
        OT_EXPECT(r == OPTICS_GFX_OK); /* retry after reclaim recovered */
        OT_EXPECT(img != nullptr);
        OT_EXPECT(atomic_load(&g_injected_calls) > 0);

        /* No unbounded growth: the retried block replaced the reclaimed
         * one rather than piling on. */
        optics_gfx_memory_stats after;
        optics_gfx_device_memory_stats(d, &after);
        OT_EXPECT(after.live_allocations == before.live_allocations + 1);

        optics_gfx_image_release(img);
        optics_gfx_image_release(sweeper);
    }

    /* --- C: dedicated-path failure. A >= 16 MiB buffer bypasses the
     *     pool; failing both the first attempt and the reclaim-retry
     *     must surface OPTICS_GFX_ERROR_OUT_OF_MEMORY with clean accounting. --- */
    {
        optics_gfx_memory_stats before;
        optics_gfx_device_memory_stats(d, &before);

        fail_next(2);
        optics_gfx_buffer_desc bd = OPTICS_GFX_BUFFER_DESC_INIT;
        bd.size = 16u * 1024 * 1024;
        bd.usage = OPTICS_GFX_BUFFER_USAGE_STORAGE;
        bd.location = OPTICS_GFX_BUFFER_GPU_LOCAL;
        optics_gfx_buffer *big = nullptr;
        OT_EXPECT(optics_gfx_buffer_create(d, &bd, &big) == OPTICS_GFX_ERROR_OUT_OF_MEMORY);
        OT_EXPECT(big == nullptr);

        optics_gfx_memory_stats after;
        optics_gfx_device_memory_stats(d, &after);
        OT_EXPECT(after.live_allocations == before.live_allocations);
        OT_EXPECT(after.bytes_in_use == before.bytes_in_use);

        /* Healthy again afterwards. */
        OT_EXPECT(optics_gfx_buffer_create(d, &bd, &big) == OPTICS_GFX_OK);
        optics_gfx_buffer_release(big);
    }

    optics_gfx_device_release(d);
    return OT_REPORT();
}
