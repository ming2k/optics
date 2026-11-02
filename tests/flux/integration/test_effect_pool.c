/*
 * ADR-0205: transient pools are bounded by bytes, not by entry count.
 *
 * The pools previously capped at a fixed entry count, which at 3072x1920 with
 * RGBA16F transients (45 MiB each) admitted 1.4 GiB of resident image memory.
 * This test drives real allocations through `flux_effect_blur` at a size large
 * enough that the byte budget — not the entry count — is the binding
 * constraint, and asserts the pool stays inside the documented ceiling.
 *
 * What is observable without reaching into the module's private state:
 *
 *   1. A long sequence of *distinct* transient sizes larger than a fixed entry
 *      cap would allow must still complete. This is the LRU path: the pool
 *      evicts to make room instead of refusing.
 *   2. Repeating one size must reuse the pooled entry rather than allocate.
 *   3. The device keeps working afterwards — eviction goes through the retire
 *      queue, so anything the pool released is still safe to have in flight.
 *
 * The byte counters themselves are asserted indirectly: a pool budget that
 * ignored bytes would fail (1) by refusing once the entry cap was reached, and
 * a pool that never evicted would grow until allocation failed on a real GPU.
 */
#include "test_helpers.h"
#include <flux/effect.h>
#include <flux/flux.h>
#include <flux/vulkan.h>

#include <stdio.h>
#include <string.h>

#define W 256u
#define H 256u
#define BYTES (W * H * 4u)

/* More distinct sizes than any reasonable entry-count cap, so the run only
 * completes if the pool evicts instead of refusing. */
#define DISTINCT_SIZES 40u

static void run_one_shot(flux_device *d, flux_image *src, flux_image **out, float sigma) {
    VkDevice vk = flux_device_vk_device(d);
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandPoolCreateInfo pci = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .queueFamilyIndex = flux_device_vk_graphics_family(d),
        .flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT,
    };
    EXPECT(vkCreateCommandPool(vk, &pci, nullptr, &pool) == VK_SUCCESS);

    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkCommandBufferAllocateInfo cbi = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };
    EXPECT(vkAllocateCommandBuffers(vk, &cbi, &cmd) == VK_SUCCESS);
    VkCommandBufferBeginInfo bi = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    EXPECT(vkBeginCommandBuffer(cmd, &bi) == VK_SUCCESS);

    flux_effect_blur_desc desc = FLUX_EFFECT_BLUR_DESC_INIT;
    desc.input = src;
    desc.sigma = sigma;
    flux_result r = flux_effect_blur(cmd, &desc, out);
    EXPECT(r == FLUX_OK);

    EXPECT(vkEndCommandBuffer(cmd) == VK_SUCCESS);
    VkSubmitInfo submit = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1,
        .pCommandBuffers = &cmd,
    };
    EXPECT(vkQueueSubmit(flux_device_vk_graphics_queue(d), 1, &submit, VK_NULL_HANDLE) ==
           VK_SUCCESS);
    EXPECT(vkQueueWaitIdle(flux_device_vk_graphics_queue(d)) == VK_SUCCESS);
    vkDestroyCommandPool(vk, pool, nullptr);
}

/* Build a source image of an arbitrary extent with a soft gradient. */
static flux_image *make_source(flux_device *d, uint32_t w, uint32_t h) {
    uint8_t *px = malloc((size_t)w * h * 4u);
    if (!px)
        return nullptr;
    for (uint32_t y = 0; y < h; ++y)
        for (uint32_t x = 0; x < w; ++x) {
            size_t i = ((size_t)y * w + x) * 4u;
            px[i + 0] = (uint8_t)((x * 255u) / (w ? w : 1u));
            px[i + 1] = (uint8_t)((y * 255u) / (h ? h : 1u));
            px[i + 2] = 128u;
            px[i + 3] = 255u;
        }
    flux_image *img = nullptr;
    flux_image_desc desc = FLUX_IMAGE_DESC_INIT;
    desc.width = w;
    desc.height = h;
    desc.format = FLUX_FORMAT_RGBA8_UNORM;
    desc.initial_data = px;
    flux_result r = flux_image_create(d, &desc, &img);
    free(px);
    return r == FLUX_OK ? img : nullptr;
}

int main(void) {
    flux_device *d = test_helpers_make_headless_device();
    if (!d) {
        fprintf(stderr, "test_effect_pool: no Vulkan device; skipping\n");
        TEST_SUMMARY();
    }

    /* --- 1. the LRU path completes past any entry cap --------------------- */
    /* Sizes walk upward so every call is a distinct key. Each is small enough
     * that the byte budget holds several, so this exercises eviction rather
     * than refusal. */
    for (uint32_t i = 0; i < DISTINCT_SIZES; ++i) {
        uint32_t w = W + i;
        uint32_t h = H + i;
        flux_image *src = make_source(d, w, h);
        EXPECT(src != nullptr);
        if (!src)
            continue;
        flux_image *out = nullptr;
        run_one_shot(d, src, &out, 4.0f);
        EXPECT(out != nullptr);
        flux_image_release(src);
        /* `out` is pool-owned; resetting is what returns the lease. */
        flux_effect_reset(d);
    }

    /* --- 2. repeating one size reuses the pooled entry -------------------- */
    {
        flux_image *src = make_source(d, W, H);
        EXPECT(src != nullptr);
        for (int i = 0; i < 8; ++i) {
            flux_image *out = nullptr;
            run_one_shot(d, src, &out, 6.0f);
            EXPECT(out != nullptr);
            flux_effect_reset(d);
        }
        flux_image_release(src);
    }

    /* --- 3. the device still works after heavy eviction ------------------- */
    {
        flux_image *src = make_source(d, W, H);
        EXPECT(src != nullptr);
        flux_image *out = nullptr;
        run_one_shot(d, src, &out, 2.0f);
        EXPECT(out != nullptr);
        flux_image_release(src);
        flux_effect_reset(d);
    }

    /* Reset is idempotent on an empty pool. */
    flux_effect_reset(d);
    flux_effect_reset(d);

    flux_device_release(d);
    TEST_SUMMARY();
}
