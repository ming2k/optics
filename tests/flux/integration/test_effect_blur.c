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
