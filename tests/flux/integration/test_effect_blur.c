/*
 * GPU integration: build a high-contrast image, blur it, read the
 * output back, assert the sharp edge is softened by an amount
 * consistent with the requested sigma.
 *
 * Also covers: input validation, sigma=0 acts as a copy,
 * flux_effect_reset is harmless when called twice.
 */
#include "test_helpers.h"
#include <flux/effect.h>
#include <flux/flux.h>
#include <flux/vulkan.h>

#include <stdlib.h>
#include <string.h>

#define W 32u
#define H 32u
#define BYTES (W * H * 4u)

/* Build a half-black / half-white RGBA8 source (sharp vertical edge
 * at column W/2). */
static void fill_edge_pattern(uint8_t *px) {
    for (uint32_t y = 0; y < H; ++y) {
        for (uint32_t x = 0; x < W; ++x) {
            uint8_t v = (x < W / 2) ? 0u : 255u;
            px[(y * W + x) * 4 + 0] = v;
            px[(y * W + x) * 4 + 1] = v;
            px[(y * W + x) * 4 + 2] = v;
            px[(y * W + x) * 4 + 3] = 255u;
        }
    }
}

/* Run a one-shot cmd buffer that records `record(cmd, user)` and
 * waits for completion. */
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

typedef struct blur_record_ctx {
    const flux_effect_blur_desc *bdesc;
    flux_image **out;
    VkBuffer dst_buffer;
} blur_record_ctx;

/* IEEE-754 binary16 helpers for the 16F effect cases. */
static uint16_t f32_to_f16(float f) {
    uint32_t x;
    memcpy(&x, &f, 4);
    uint32_t sign = (x >> 16) & 0x8000u;
    int exp = (int)((x >> 23) & 0xFFu) - 112; /* rebased exponent */
    uint32_t mant = x & 0x7FFFFFu;
    if (exp <= 0)
        return (uint16_t)sign; /* underflow to zero (test values never go subnormal) */
    if (exp >= 31)
        return (uint16_t)(sign | 0x7C00u);
    return (uint16_t)(sign | ((uint32_t)exp << 10) | (mant >> 13));
}

static float f16_to_f32(uint16_t h) {
    uint32_t sign = (uint32_t)(h & 0x8000u) << 16;
    uint32_t exp = (h >> 10) & 0x1Fu;
    uint32_t mant = h & 0x3FFu;
    uint32_t bits;
    if (exp == 0) {
        if (mant == 0) {
            bits = sign;
        } else {
            int shifts = 0;
            while (!(mant & 0x400u)) {
                mant <<= 1;
                ++shifts;
            }
            bits = sign | ((uint32_t)(113 - shifts) << 23) | ((mant & 0x3FFu) << 13);
        }
    } else if (exp == 31) {
        bits = sign | 0x7F800000u | (mant << 13);
    } else {
        bits = sign | ((exp + 112) << 23) | (mant << 13);
    }
    float out;
    memcpy(&out, &bits, 4);
    return out;
}

typedef struct blur16_record_ctx {
    const flux_effect_blur_desc *bdesc;
    flux_image **out;
    VkBuffer dst_buffer;
    flux_result result;
} blur16_record_ctx;

static void record_blur16_and_readback(VkCommandBuffer cmd, void *user) {
    blur16_record_ctx *ctx = user;
    ctx->result = flux_effect_blur(cmd, ctx->bdesc, ctx->out);
    if (ctx->result != FLUX_OK)
        return;

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

static void record_blur_and_readback(VkCommandBuffer cmd, void *user) {
    blur_record_ctx *ctx = user;
    EXPECT(flux_effect_blur(cmd, ctx->bdesc, ctx->out) == FLUX_OK);
    EXPECT(*ctx->out != nullptr);

    /* Transition the output from GENERAL to TRANSFER_SRC_OPTIMAL so
     * the copy is valid. The blur's trailing barrier already made
     * the storage writes visible to subsequent reads. */
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
        flux_effect_blur_desc d = FLUX_EFFECT_BLUR_DESC_INIT;
        EXPECT(flux_effect_blur(nullptr, &d, &out) == FLUX_ERROR_INVALID_ARGUMENT);
        EXPECT(out == nullptr);
    }

    flux_device *d = test_helpers_make_headless_device();
    if (!d) {
        fprintf(stderr, "test_effect_blur: no Vulkan device; skipping\n");
        TEST_SUMMARY();
    }
    VkDevice vk = flux_device_vk_device(d);

    /* --- input validation against a real device --- */
    {
        flux_image *out = nullptr;
        flux_effect_blur_desc bdesc = FLUX_EFFECT_BLUR_DESC_INIT;
        bdesc.type = FLUX_TYPE_UNKNOWN; /* wrong tag */
        bdesc.sigma = 1.0f;

        VkCommandPool pool = VK_NULL_HANDLE;
        VkCommandPoolCreateInfo pci = {
            .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
            .queueFamilyIndex = flux_device_vk_graphics_family(d),
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

        EXPECT(flux_effect_blur(cmd, &bdesc, &out) == FLUX_ERROR_INVALID_ARGUMENT);
        EXPECT(out == nullptr);

        bdesc.type = FLUX_TYPE_EFFECT_BLUR_DESC;
        bdesc.input = nullptr;
        EXPECT(flux_effect_blur(cmd, &bdesc, &out) == FLUX_ERROR_INVALID_ARGUMENT);

        vkDestroyCommandPool(vk, pool, nullptr);
    }

    /* --- build input image --- */
    uint8_t pixels[BYTES];
    fill_edge_pattern(pixels);

    flux_image_desc idesc = FLUX_IMAGE_DESC_INIT;
    idesc.width = W;
    idesc.height = H;
    idesc.format = FLUX_FORMAT_RGBA8_UNORM;
    idesc.initial_data = pixels;
    flux_image *input = nullptr;
    EXPECT(flux_image_create(d, &idesc, &input) == FLUX_OK);

    /* --- host-visible readback buffer --- */
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    void *mapped = nullptr;
    {
        VkBufferCreateInfo bci = {
            .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .size = BYTES,
            .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        };
        EXPECT(vkCreateBuffer(vk, &bci, nullptr, &buffer) == VK_SUCCESS);
        VkMemoryRequirements mr;
        vkGetBufferMemoryRequirements(vk, buffer, &mr);
        uint32_t mt = test_helpers_find_memory_type(d, mr.memoryTypeBits,
                                                    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                        VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        EXPECT(mt != UINT32_MAX);
        VkMemoryAllocateInfo mai = {
            .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .allocationSize = mr.size,
            .memoryTypeIndex = mt,
        };
        EXPECT(vkAllocateMemory(vk, &mai, nullptr, &memory) == VK_SUCCESS);
        vkBindBufferMemory(vk, buffer, memory, 0);
        vkMapMemory(vk, memory, 0, VK_WHOLE_SIZE, 0, &mapped);
    }

    /* --- non-trivial blur softens the edge --- */
    {
        memset(mapped, 0xCD, BYTES);
        flux_image *out = nullptr;
        flux_effect_blur_desc bdesc = FLUX_EFFECT_BLUR_DESC_INIT;
        bdesc.input = input;
        bdesc.sigma = 4.0f;

        blur_record_ctx ctx = {.bdesc = &bdesc, .out = &out, .dst_buffer = buffer};
        run_one_shot(d, record_blur_and_readback, &ctx);

        const uint8_t *px = mapped;

        /* Far left of the image stays black; far right stays white. */
        EXPECT(px[(15 * W + 0) * 4 + 0] < 16);        /* near-black */
        EXPECT(px[(15 * W + (W - 1)) * 4 + 0] > 240); /* near-white */

        /* The column immediately left of the original sharp edge
         * was pure black in the input; with sigma=4 it should now
         * have picked up appreciable luminance from the white half. */
        uint8_t edge_left = px[(15 * W + (W / 2 - 1)) * 4 + 0];
        uint8_t edge_right = px[(15 * W + (W / 2)) * 4 + 0];
        EXPECT(edge_left > 32 && edge_left < 224); /* meaningfully softened */
        EXPECT(edge_right > 32 && edge_right < 224);
        EXPECT(edge_right > edge_left); /* gradient runs the right way */
    }

    /* --- sigma == 0 → output matches input exactly --- */
    {
        memset(mapped, 0xCD, BYTES);
        flux_image *out = nullptr;
        flux_effect_blur_desc bdesc = FLUX_EFFECT_BLUR_DESC_INIT;
        bdesc.input = input;
        bdesc.sigma = 0.0f;

        blur_record_ctx ctx = {.bdesc = &bdesc, .out = &out, .dst_buffer = buffer};
        run_one_shot(d, record_blur_and_readback, &ctx);

        const uint8_t *px = mapped;
        for (uint32_t i = 0; i < BYTES; ++i) {
            if (px[i] != pixels[i]) {
                fprintf(stderr, "FAIL sigma=0 mismatch at byte %u: got %u expected %u\n", i, px[i],
                        pixels[i]);
                EXPECT(px[i] == pixels[i]);
                break;
            }
        }
    }

    /* --- same-key calls in one epoch get exclusive writable slots --- */
    {
        flux_effect_blur_desc bdesc = FLUX_EFFECT_BLUR_DESC_INIT;
        bdesc.input = input;
        bdesc.sigma = 2.0f;
        flux_image *a = nullptr;
        flux_image *b = nullptr;

        VkCommandPool pool = VK_NULL_HANDLE;
        VkCommandPoolCreateInfo pci = {
            .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
            .queueFamilyIndex = flux_device_vk_graphics_family(d),
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
        EXPECT(flux_effect_blur(cmd, &bdesc, &a) == FLUX_OK);
        EXPECT(flux_effect_blur(cmd, &bdesc, &b) == FLUX_OK);
        EXPECT(a != nullptr && b != nullptr && a != b);
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
        vkQueueSubmit2(flux_device_vk_graphics_queue(d), 1, &si, fence);
        vkWaitForFences(vk, 1, &fence, VK_TRUE, UINT64_MAX);
        vkDestroyFence(vk, fence, nullptr);
        vkDestroyCommandPool(vk, pool, nullptr);
        flux_effect_reset(d);
    }

    /* --- promote: transient → caller-owned image with the same bytes --- */
    {
        memset(mapped, 0xCD, BYTES);
        flux_image *transient = nullptr;
        flux_effect_blur_desc bdesc = FLUX_EFFECT_BLUR_DESC_INIT;
        bdesc.input = input;
        bdesc.sigma = 4.0f;

        /* Run the blur to populate the transient. We do NOT read it
         * back here — that's the read-back path's job. We're going
         * to promote it, then read the promoted image instead. */
        VkCommandPool pool = VK_NULL_HANDLE;
        VkCommandPoolCreateInfo pci = {
            .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
            .queueFamilyIndex = flux_device_vk_graphics_family(d),
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
        EXPECT(flux_effect_blur(cmd, &bdesc, &transient) == FLUX_OK);
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
        vkQueueSubmit2(flux_device_vk_graphics_queue(d), 1, &si, fence);
        vkWaitForFences(vk, 1, &fence, VK_TRUE, UINT64_MAX);
        vkDestroyFence(vk, fence, nullptr);
        vkDestroyCommandPool(vk, pool, nullptr);

        /* Promote. Synchronous: returns after the copy completes. */
        flux_image *owned = nullptr;
        EXPECT(flux_effect_promote(transient, &owned) == FLUX_OK);
        EXPECT(owned != nullptr);
        /* The promoted image must have a fresh sampled bindless handle
         * (regular flux_image lifecycle), distinct from the transient. */
        EXPECT(flux_image_bindless_handle(owned) != FLUX_BINDLESS_INVALID);
        EXPECT(flux_image_bindless_handle(owned) != flux_image_bindless_handle(transient));

        /* Read the promoted image back via vkCmdCopyImageToBuffer.
         * Owned image is in SHADER_READ_ONLY_OPTIMAL after promote. */
        vkCreateCommandPool(vk, &pci, nullptr, &pool);
        vkAllocateCommandBuffers(vk, &cbai, &cmd);
        vkBeginCommandBuffer(cmd, &cbbi);

        VkImageMemoryBarrier2 to_xfer = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
            .srcStageMask = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT,
            .srcAccessMask = 0,
            .dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT,
            .dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            .image = flux_image_vk_image(owned),
            .subresourceRange =
                {
                    .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                    .levelCount = 1,
                    .layerCount = 1,
                },
        };
        VkDependencyInfo dito = {
            .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
            .imageMemoryBarrierCount = 1,
            .pImageMemoryBarriers = &to_xfer,
        };
        vkCmdPipelineBarrier2(cmd, &dito);

        VkBufferImageCopy region = {
            .imageSubresource = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .layerCount = 1},
            .imageExtent = {W, H, 1},
        };
        vkCmdCopyImageToBuffer(cmd, flux_image_vk_image(owned),
                               VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, 1, &region);

        VkMemoryBarrier2 mb = {
            .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
            .srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT,
            .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
            .dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT,
            .dstAccessMask = VK_ACCESS_2_HOST_READ_BIT,
        };
        VkDependencyInfo dim = {
            .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
            .memoryBarrierCount = 1,
            .pMemoryBarriers = &mb,
        };
        vkCmdPipelineBarrier2(cmd, &dim);
        vkEndCommandBuffer(cmd);

        cbsi.commandBuffer = cmd; /* fresh handle from the re-alloc above */
        vkCreateFence(vk, &fci, nullptr, &fence);
        vkQueueSubmit2(flux_device_vk_graphics_queue(d), 1, &si, fence);
        vkWaitForFences(vk, 1, &fence, VK_TRUE, UINT64_MAX);
        vkDestroyFence(vk, fence, nullptr);
        vkDestroyCommandPool(vk, pool, nullptr);

        const uint8_t *px = mapped;
        /* Same edge-softening assertions as the direct read-back path. */
        EXPECT(px[(15 * W + 0) * 4 + 0] < 16);
        EXPECT(px[(15 * W + (W - 1)) * 4 + 0] > 240);
        uint8_t edge_left = px[(15 * W + (W / 2 - 1)) * 4 + 0];
        uint8_t edge_right = px[(15 * W + (W / 2)) * 4 + 0];
        EXPECT(edge_left > 32 && edge_left < 224);
        EXPECT(edge_right > 32 && edge_right < 224);
        EXPECT(edge_right > edge_left);

        /* Owned image survives flux_effect_reset (which only releases
         * transients). After reset, we can still sample it. */
        flux_effect_reset(d);
        EXPECT(flux_image_vk_image(owned) != VK_NULL_HANDLE);
        flux_image_release(owned);
    }

    /* --- promote rejects non-effect-output images --- */
    {
        flux_image *owned = nullptr;
        EXPECT(flux_effect_promote(input, &owned) == FLUX_ERROR_INVALID_ARGUMENT);
        EXPECT(owned == nullptr);

        EXPECT(flux_effect_promote(nullptr, &owned) == FLUX_ERROR_INVALID_ARGUMENT);
    }

    /* --- 16F working-space input: HDR values survive the blur (ADR-0069) --- */
    {
        /* Left half 0.0, right half 2.0 (HDR highlight). An 8-bit
         * intermediate would clamp the highlight to 1.0; the rgba16f
         * path must carry it through. */
        static uint16_t f16pixels[W * H * 4];
        for (uint32_t y = 0; y < H; ++y)
            for (uint32_t x = 0; x < W; ++x) {
                float v = (x < W / 2) ? 0.0f : 2.0f;
                size_t at = (y * W + x) * 4;
                f16pixels[at + 0] = f32_to_f16(v);
                f16pixels[at + 1] = f32_to_f16(v);
                f16pixels[at + 2] = f32_to_f16(v);
                f16pixels[at + 3] = f32_to_f16(1.0f);
            }
        flux_image_desc f16desc = FLUX_IMAGE_DESC_INIT;
        f16desc.width = W;
        f16desc.height = H;
        f16desc.format = FLUX_FORMAT_RGBA16_SFLOAT;
        f16desc.initial_data = f16pixels;
        flux_image *f16input = nullptr;
        EXPECT(flux_image_create(d, &f16desc, &f16input) == FLUX_OK);

        VkBuffer buffer16 = VK_NULL_HANDLE;
        VkDeviceMemory memory16 = VK_NULL_HANDLE;
        void *mapped16 = nullptr;
        {
            VkBufferCreateInfo bci = {
                .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                .size = W * H * 8u,
                .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
            };
            EXPECT(vkCreateBuffer(vk, &bci, nullptr, &buffer16) == VK_SUCCESS);
            VkMemoryRequirements mr;
            vkGetBufferMemoryRequirements(vk, buffer16, &mr);
            uint32_t mt = test_helpers_find_memory_type(d, mr.memoryTypeBits,
                                                        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                            VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
            EXPECT(mt != UINT32_MAX);
            VkMemoryAllocateInfo mai = {
                .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                .allocationSize = mr.size,
                .memoryTypeIndex = mt,
            };
            EXPECT(vkAllocateMemory(vk, &mai, nullptr, &memory16) == VK_SUCCESS);
            vkBindBufferMemory(vk, buffer16, memory16, 0);
            vkMapMemory(vk, memory16, 0, VK_WHOLE_SIZE, 0, &mapped16);
        }

        flux_image *out = nullptr;
        flux_effect_blur_desc bdesc = FLUX_EFFECT_BLUR_DESC_INIT;
        bdesc.input = f16input;
        bdesc.sigma = 4.0f;
        blur16_record_ctx ctx = {.bdesc = &bdesc, .out = &out, .dst_buffer = buffer16};
        run_one_shot(d, record_blur16_and_readback, &ctx);

        if (ctx.result == FLUX_ERROR_UNSUPPORTED) {
            fprintf(stderr, "test_effect_blur: no rgba16f storage; 16F case skipped\n");
        } else {
            EXPECT(ctx.result == FLUX_OK);
            EXPECT(flux_image_format(out) == FLUX_FORMAT_RGBA16_SFLOAT);
            const uint16_t *hx = mapped16;
            float far_right = f16_to_f32(hx[(15 * W + (W - 1)) * 4 + 0]);
            float edge_left = f16_to_f32(hx[(15 * W + (W / 2 - 1)) * 4 + 0]);
            float edge_right = f16_to_f32(hx[(15 * W + (W / 2)) * 4 + 0]);
            /* The 2.0 highlight survived (8-bit would clamp at 1.0). */
            EXPECT(far_right > 1.9f && far_right < 2.1f);
            /* Symmetric kernel around the step: both sides of the edge
             * land on the linear midpoint 1.0 — above every 8-bit encode. */
            EXPECT(edge_left > 0.85f && edge_left < 1.15f);
            EXPECT(edge_right > 0.85f && edge_right < 1.15f);
        }

        if (mapped16)
            vkUnmapMemory(vk, memory16);
        if (memory16)
            vkFreeMemory(vk, memory16, nullptr);
        if (buffer16)
            vkDestroyBuffer(vk, buffer16, nullptr);
        flux_image_release(f16input);
        flux_effect_reset(d);
    }

    /* --- reset is safe to call repeatedly and on an empty pool --- */
    flux_effect_reset(d);
    flux_effect_reset(d);

    if (mapped)
        vkUnmapMemory(vk, memory);
    if (memory)
        vkFreeMemory(vk, memory, nullptr);
    if (buffer)
        vkDestroyBuffer(vk, buffer, nullptr);
    flux_image_release(input);
    flux_device_release(d);
    TEST_SUMMARY();
}
