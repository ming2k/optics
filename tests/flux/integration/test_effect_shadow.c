/*
 * GPU integration for the drop-shadow operator (ADR-0074):
 *   - input validation (null args, wrong type, null input, non-finite)
 *   - a solid opaque square mask, blur 0, offset 0: the shadow is the
 *     mask itself, tinted and premultiplied — bit-exact copy of alpha
 *   - the same mask with blur 0 and a +6px offset: the shadow edge
 *     moves exactly 6px down/right
 *   - blur > 0 softens the edge monotonically (alpha falls off with
 *     distance from the mask)
 *   - alpha 0 produces a fully transparent output
 *   - flux_effect_reset returns leases; a second shadow works
 */
#include "test_helpers.h"
#include <flux/effect.h>
#include <flux/flux.h>
#include <flux/vulkan.h>

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define W 32u
#define H 32u
#define BYTES (W * H * 4u)

/* Opaque white square mask: alpha 255 inside [8,24)×[8,24), 0 outside. */
static void fill_square_mask(uint8_t *px) {
    memset(px, 0, BYTES);
    for (uint32_t y = 8; y < 24; ++y) {
        for (uint32_t x = 8; x < 24; ++x) {
            uint8_t *p = px + (y * W + x) * 4;
            p[0] = 255u;
            p[1] = 255u;
            p[2] = 255u;
            p[3] = 255u;
        }
    }
}

/* Host-side reference for the separable clamp-to-edge Gaussian the shader
 * implements (radius = ceil(3σ), weights normalised per tap sum). Used to
 * assert the blurred-shadow centre value: on a 32 px image a σ=4 kernel has
 * radius 12 = half the extent, so the square's plateau does NOT stay at 255 —
 * the analytic centre value is ≈233/255. */
static float expected_blur_at(uint32_t px, uint32_t py) {
    const float sigma = 4.0f;
    const int radius = (int)(3.0f * sigma); /* ceil(12.0) == 12 */
    float sum = 0.0f;
    float wsum = 0.0f;
    for (int dy = -radius; dy <= radius; ++dy) {
        for (int dx = -radius; dx <= radius; ++dx) {
            int sx = (int)px + dx;
            int sy = (int)py + dy;
            sx = sx < 0 ? 0 : (sx >= (int)W ? (int)W - 1 : sx);
            sy = sy < 0 ? 0 : (sy >= (int)H ? (int)H - 1 : sy);
            float inside = (sx >= 8 && sx < 24 && sy >= 8 && sy < 24) ? 1.0f : 0.0f;
            float w = expf(-(float)(dx * dx + dy * dy) / (2.0f * sigma * sigma));
            sum += inside * w;
            wsum += w;
        }
    }
    return (sum / wsum) * 255.0f;
}

typedef void (*record_fn)(VkCommandBuffer cmd, void *user);

static void run_one_shot(flux_device *d, record_fn rec, void *user) {
    VkDevice vk = flux_device_vk_device(d);
    VkQueue queue = flux_device_vk_graphics_queue(d);
    uint32_t qfam = flux_device_vk_graphics_family(d);

    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandPoolCreateInfo pci = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .queueFamilyIndex = qfam,
        .flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT,
    };
    vkCreateCommandPool(vk, &pci, nullptr, &pool);

    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkCommandBufferAllocateInfo cbai = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };
    vkAllocateCommandBuffers(vk, &cbai, &cmd);

    VkCommandBufferBeginInfo cbbi = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    vkBeginCommandBuffer(cmd, &cbbi);
    rec(cmd, user);
    vkEndCommandBuffer(cmd);

    VkCommandBufferSubmitInfo cbsi = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
        .commandBuffer = cmd,
    };
    VkSubmitInfo2 si = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
        .commandBufferInfoCount = 1,
        .pCommandBufferInfos = &cbsi,
    };
    VkFence fence = VK_NULL_HANDLE;
    VkFenceCreateInfo fci = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    vkCreateFence(vk, &fci, nullptr, &fence);
    vkQueueSubmit2(queue, 1, &si, fence);
    vkWaitForFences(vk, 1, &fence, VK_TRUE, UINT64_MAX);
    vkDestroyFence(vk, fence, nullptr);
    vkDestroyCommandPool(vk, pool, nullptr);
}

typedef struct shadow_record_ctx {
    const flux_effect_shadow_desc *desc;
    flux_image **out;
    VkBuffer dst_buffer;
} shadow_record_ctx;

static void record_shadow_and_readback(VkCommandBuffer cmd, void *user) {
    shadow_record_ctx *ctx = user;
    EXPECT(flux_effect_shadow(cmd, ctx->desc, ctx->out) == FLUX_OK);
    EXPECT(*ctx->out != nullptr);

    VkImage img = flux_image_vk_image(*ctx->out);
    VkImageMemoryBarrier2 b = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        .srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT,
        .dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
        .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        .image = img,
        .subresourceRange =
            {
                .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                .levelCount = 1,
                .layerCount = 1,
            },
    };
    VkDependencyInfo di = {
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .imageMemoryBarrierCount = 1,
        .pImageMemoryBarriers = &b,
    };
    vkCmdPipelineBarrier2(cmd, &di);

    VkBufferImageCopy region = {
        .imageSubresource = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .layerCount = 1},
        .imageExtent = {W, H, 1},
    };
    vkCmdCopyImageToBuffer(cmd, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, ctx->dst_buffer, 1,
                           &region);

    VkMemoryBarrier2 mb = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
        .srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT,
        .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT,
        .dstAccessMask = VK_ACCESS_2_HOST_READ_BIT,
    };
    VkDependencyInfo di2 = {
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .memoryBarrierCount = 1,
        .pMemoryBarriers = &mb,
    };
    vkCmdPipelineBarrier2(cmd, &di2);
}

int main(void) {
    /* --- input validation (no device required) --- */
    {
        flux_image *out = nullptr;
        flux_effect_shadow_desc d = FLUX_EFFECT_SHADOW_DESC_INIT;
        EXPECT(flux_effect_shadow(nullptr, &d, &out) == FLUX_ERROR_INVALID_ARGUMENT);
        EXPECT(out == nullptr);
        EXPECT(flux_effect_shadow((VkCommandBuffer)1, &d, &out) == FLUX_ERROR_INVALID_ARGUMENT);
        EXPECT(out == nullptr);
        d.input = (flux_image *)1;
        EXPECT(flux_effect_shadow((VkCommandBuffer)1, &d, nullptr) == FLUX_ERROR_INVALID_ARGUMENT);
    }
    {
        /* Wrong type. */
        flux_image *out = nullptr;
        flux_effect_shadow_desc d = FLUX_EFFECT_SHADOW_DESC_INIT;
        d.input = (flux_image *)1;
        d.type = FLUX_TYPE_EFFECT_BLUR_DESC;
        EXPECT(flux_effect_shadow((VkCommandBuffer)1, &d, &out) == FLUX_ERROR_INVALID_ARGUMENT);
        EXPECT(out == nullptr);
    }
    {
        /* Non-finite parameters. */
        flux_image *out = nullptr;
        flux_effect_shadow_desc d = FLUX_EFFECT_SHADOW_DESC_INIT;
        d.input = (flux_image *)1;
        d.alpha = (0.0f / 0.0f);
        EXPECT(flux_effect_shadow((VkCommandBuffer)1, &d, &out) == FLUX_ERROR_INVALID_ARGUMENT);
        d.alpha = 1.0f;
        d.offset_x = (1.0f / 0.0f);
        EXPECT(flux_effect_shadow((VkCommandBuffer)1, &d, &out) == FLUX_ERROR_INVALID_ARGUMENT);
    }

    flux_device *d = test_helpers_make_headless_device();
    if (!d) {
        fprintf(stderr, "test_effect_shadow: no Vulkan device; skipping\n");
        TEST_SUMMARY();
    }

    uint8_t *mask_px = malloc(BYTES);
    fill_square_mask(mask_px);
    flux_image_desc idesc = FLUX_IMAGE_DESC_INIT;
    idesc.width = W;
    idesc.height = H;
    idesc.format = FLUX_FORMAT_RGBA8_UNORM;
    idesc.initial_data = mask_px;
    flux_image *mask = nullptr;
    EXPECT(flux_image_create(d, &idesc, &mask) == FLUX_OK);
    EXPECT(mask != nullptr);

    /* Host-visible destination buffer. */
    VkBuffer dst = VK_NULL_HANDLE;
    VkDeviceMemory dst_mem = VK_NULL_HANDLE;
    {
        VkBufferCreateInfo bi = {
            .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .size = BYTES,
            .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        };
        VkDevice vk = flux_device_vk_device(d);
        EXPECT(vkCreateBuffer(vk, &bi, nullptr, &dst) == VK_SUCCESS);
        VkMemoryRequirements mr;
        vkGetBufferMemoryRequirements(vk, dst, &mr);
        VkMemoryAllocateInfo ai = {
            .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .allocationSize = mr.size,
            .memoryTypeIndex = test_helpers_find_memory_type(
                d, mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                          VK_MEMORY_PROPERTY_HOST_COHERENT_BIT),
        };
        EXPECT(vkAllocateMemory(vk, &ai, nullptr, &dst_mem) == VK_SUCCESS);
        EXPECT(vkBindBufferMemory(vk, dst, dst_mem, 0) == VK_SUCCESS);
    }
    uint8_t *pixels = nullptr;
    EXPECT(vkMapMemory(flux_device_vk_device(d), dst_mem, 0, BYTES, 0, (void **)&pixels) ==
           VK_SUCCESS);

    /* --- Case 1: blur 0, offset 0, white tint, alpha 1 = the mask itself
     * (tinted, premultiplied: rgb = a). --- */
    {
        flux_effect_shadow_desc sd = FLUX_EFFECT_SHADOW_DESC_INIT;
        sd.input = mask;
        sd.blur = 0.0f;
        sd.tint_red = 1.0f;
        sd.tint_green = 1.0f;
        sd.tint_blue = 1.0f;
        sd.alpha = 1.0f;

        flux_image *out = nullptr;
        shadow_record_ctx ctx = {.desc = &sd, .out = &out, .dst_buffer = dst};
        memset(pixels, 0xAA, BYTES);
        run_one_shot(d, record_shadow_and_readback, &ctx);
        vkDeviceWaitIdle(flux_device_vk_device(d));

        int mismatches = 0;
        for (uint32_t y = 0; y < H; ++y) {
            for (uint32_t x = 0; x < W; ++x) {
                uint8_t a = pixels[(y * W + x) * 4 + 3];
                uint8_t expect = (x >= 8 && x < 24 && y >= 8 && y < 24) ? 255u : 0u;
                if (a != expect)
                    ++mismatches;
            }
        }
        EXPECT(mismatches == 0);
        flux_effect_reset(d);
    }

    /* --- Case 2: blur 0, offset +6/+6 — the square moves exactly 6 px. --- */
    {
        flux_effect_shadow_desc sd = FLUX_EFFECT_SHADOW_DESC_INIT;
        sd.input = mask;
        sd.blur = 0.0f;
        sd.offset_x = 6.0f;
        sd.offset_y = 6.0f;
        sd.tint_red = 1.0f;
        sd.tint_green = 1.0f;
        sd.tint_blue = 1.0f;
        sd.alpha = 1.0f;

        flux_image *out = nullptr;
        shadow_record_ctx ctx = {.desc = &sd, .out = &out, .dst_buffer = dst};
        memset(pixels, 0xAA, BYTES);
        run_one_shot(d, record_shadow_and_readback, &ctx);
        vkDeviceWaitIdle(flux_device_vk_device(d));

        int mismatches = 0;
        for (uint32_t y = 0; y < H; ++y) {
            for (uint32_t x = 0; x < W; ++x) {
                uint8_t a = pixels[(y * W + x) * 4 + 3];
                uint8_t expect = (x >= 14 && x < 30 && y >= 14 && y < 30) ? 255u : 0u;
                if (a != expect)
                    ++mismatches;
            }
        }
        EXPECT(mismatches == 0);
        flux_effect_reset(d);
    }

    /* --- Case 3: blur 4 softens the edge; alpha decays with distance. --- */
    {
        flux_effect_shadow_desc sd = FLUX_EFFECT_SHADOW_DESC_INIT;
        sd.input = mask;
        sd.blur = 4.0f;
        sd.tint_red = 1.0f;
        sd.tint_green = 1.0f;
        sd.tint_blue = 1.0f;
        sd.alpha = 1.0f;

        flux_image *out = nullptr;
        shadow_record_ctx ctx = {.desc = &sd, .out = &out, .dst_buffer = dst};
        memset(pixels, 0xAA, BYTES);
        run_one_shot(d, record_shadow_and_readback, &ctx);
        vkDeviceWaitIdle(flux_device_vk_device(d));

        /* Deep inside the mask the shadow stays nearly opaque but not exactly
         * 255: with clamp-to-edge sampling on a 32 px image, a σ=4 kernel
         * (radius 12 = half the extent) redistributes mass from the plateau
         * to the skirts, so the centre settles at ≈233. Assert the analytic
         * separable-Gaussian value (computed on the host, below) instead of
         * a naive 255. Far away it is fully transparent; the pixel just
         * outside the edge is the softened skirt. */
        uint8_t center = pixels[(16 * W + 16) * 4 + 3];
        uint8_t far = pixels[(2 * W + 2) * 4 + 3];
        uint8_t skirt = pixels[(16 * W + 6) * 4 + 3]; /* 2px left of the edge */
        float expect_center = expected_blur_at(16, 16);
        EXPECT(center >= (uint8_t)(expect_center - 4.0f) && center <= (uint8_t)(expect_center + 4.0f));
        /* The corner is far outside the square but a radius-12 kernel still
         * leaks a faint skirt there (analytic ≈1-2/255); assert near-zero. */
        EXPECT(far <= 4u);
        EXPECT(skirt > 0u && skirt < 255u);
        flux_effect_reset(d);
    }

    /* --- Case 4: alpha 0 → fully transparent output. --- */
    {
        flux_effect_shadow_desc sd = FLUX_EFFECT_SHADOW_DESC_INIT;
        sd.input = mask;
        sd.blur = 2.0f;
        sd.tint_red = 1.0f;
        sd.tint_green = 1.0f;
        sd.tint_blue = 1.0f;
        sd.alpha = 0.0f;

        flux_image *out = nullptr;
        shadow_record_ctx ctx = {.desc = &sd, .out = &out, .dst_buffer = dst};
        memset(pixels, 0xAA, BYTES);
        run_one_shot(d, record_shadow_and_readback, &ctx);
        vkDeviceWaitIdle(flux_device_vk_device(d));

        int nonzero = 0;
        for (uint32_t i = 3; i < BYTES; i += 4)
            if (pixels[i] != 0)
                ++nonzero;
        EXPECT(nonzero == 0);
        flux_effect_reset(d);
    }

    /* --- Case 5: leases return; a second shadow works after reset. --- */
    {
        flux_effect_shadow_desc sd = FLUX_EFFECT_SHADOW_DESC_INIT;
        sd.input = mask;
        sd.blur = 3.0f;
        sd.tint_red = 0.0f;
        sd.tint_green = 0.0f;
        sd.tint_blue = 0.0f;
        sd.alpha = 1.0f;

        for (int i = 0; i < 2; ++i) {
            flux_image *out = nullptr;
            shadow_record_ctx ctx = {.desc = &sd, .out = &out, .dst_buffer = dst};
            run_one_shot(d, record_shadow_and_readback, &ctx);
            vkDeviceWaitIdle(flux_device_vk_device(d));
            EXPECT(out != nullptr);
            flux_effect_reset(d);
        }
    }

    vkUnmapMemory(flux_device_vk_device(d), dst_mem);
    vkDestroyBuffer(flux_device_vk_device(d), dst, nullptr);
    vkFreeMemory(flux_device_vk_device(d), dst_mem, nullptr);
    flux_image_release(mask);
    free(mask_px);
    flux_device_release(d);
    TEST_SUMMARY();
}
