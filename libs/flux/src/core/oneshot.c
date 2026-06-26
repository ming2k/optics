/*
 * One-shot GPU submission helpers.
 *
 * Used by:
 *   - flux_vk_upload_to_buffer / flux_vk_upload_to_image (initial mesh /
 *     image uploads and image sub-region updates)
 *   - flux_vk_transition_image_layout (no-data layout transitions)
 *   - flux_surface_read_pixels (offscreen readback in surface.c)
 *
 * The submit-and-wait + transient command-buffer allocators are
 * exported through internal.h because readback (in surface.c) and the
 * upload paths share them.
 */
#include "internal.h"

#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/*  Submit infrastructure                                             */
/*                                                                    */
/*  Shared with surface.c (readback). Bodies file-local.              */
/* ------------------------------------------------------------------ */

/* Submit `cmd` on `queue` with optional wait/signal semaphores, then
 * wait on a fresh fence with a finite timeout. Returns VK_TIMEOUT if
 * the GPU does not complete within FLUX_DEFAULT_FRAME_TIMEOUT_NS. */
static VkResult submit_and_wait(flux_device *d, VkQueue queue, VkCommandBuffer cmd,
                                VkSemaphore wait_sem, VkPipelineStageFlags2 wait_stage,
                                VkSemaphore signal_sem, VkPipelineStageFlags2 signal_stage) {
    VkFence fence = VK_NULL_HANDLE;
    VkFenceCreateInfo fci = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkResult fr = vkCreateFence(d->device, &fci, nullptr, &fence);
    if (fr != VK_SUCCESS)
        return fr;

    VkSemaphoreSubmitInfo wait_info = {
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
        .semaphore = wait_sem,
        .stageMask = wait_stage,
    };
    VkSemaphoreSubmitInfo signal_info = {
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
        .semaphore = signal_sem,
        .stageMask = signal_stage,
    };
    VkCommandBufferSubmitInfo cbsi = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
        .commandBuffer = cmd,
    };
    VkSubmitInfo2 si = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
        .waitSemaphoreInfoCount = wait_sem ? 1 : 0,
        .pWaitSemaphoreInfos = wait_sem ? &wait_info : nullptr,
        .signalSemaphoreInfoCount = signal_sem ? 1 : 0,
        .pSignalSemaphoreInfos = signal_sem ? &signal_info : nullptr,
        .commandBufferInfoCount = 1,
        .pCommandBufferInfos = &cbsi,
    };
    pthread_mutex_lock(&d->queue_lock);
    VkResult vr = vkQueueSubmit2(queue, 1, &si, fence);
    pthread_mutex_unlock(&d->queue_lock);
    if (vr == VK_SUCCESS) {
        vr = vkWaitForFences(d->device, 1, &fence, VK_TRUE, FLUX_DEFAULT_FRAME_TIMEOUT_NS);
    }
    vkDestroyFence(d->device, fence, nullptr);
    return vr;
}

/* Convenience for the common single-queue case. */
VkResult flux_vk_submit_one_shot_and_wait(flux_device *d, VkCommandBuffer cmd) {
    return submit_and_wait(d, d->graphics_queue, cmd, VK_NULL_HANDLE, 0, VK_NULL_HANDLE, 0);
}

/* Pick the queue family + queue for one-shot uploads. When a
 * dedicated transfer queue exists, prefer it; the destination
 * resource needs a queue-family ownership transfer barrier to the
 * graphics family before the next graphics-side use. */
bool flux_vk_prefer_transfer_queue(const flux_device *d) {
    return d->transfer_dedicated && d->transfer_family != d->graphics_family;
}

/* Allocate a transient pool + one primary command buffer on the
 * given queue family. Out params zeroed on failure. */
VkResult flux_vk_new_transient_cmd(flux_device *d, uint32_t family, VkCommandPool *out_pool,
                                   VkCommandBuffer *out_cmd) {
    *out_pool = VK_NULL_HANDLE;
    *out_cmd = VK_NULL_HANDLE;
    VkCommandPoolCreateInfo pci = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .queueFamilyIndex = family,
        .flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT,
    };
    VkResult vr = vkCreateCommandPool(d->device, &pci, nullptr, out_pool);
    if (vr != VK_SUCCESS)
        return vr;
    VkCommandBufferAllocateInfo cbai = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = *out_pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };
    vr = vkAllocateCommandBuffers(d->device, &cbai, out_cmd);
    if (vr != VK_SUCCESS) {
        vkDestroyCommandPool(d->device, *out_pool, nullptr);
        *out_pool = VK_NULL_HANDLE;
        return vr;
    }
    VkCommandBufferBeginInfo cbbi = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    vr = vkBeginCommandBuffer(*out_cmd, &cbbi);
    if (vr != VK_SUCCESS) {
        vkDestroyCommandPool(d->device, *out_pool, nullptr);
        *out_pool = VK_NULL_HANDLE;
        *out_cmd = VK_NULL_HANDLE;
        return vr;
    }
    return VK_SUCCESS;
}

/* ------------------------------------------------------------------ */
/*  One-shot buffer upload                                            */
/* ------------------------------------------------------------------ */

flux_result flux_vk_upload_to_buffer(flux_device *d, VkBuffer dst, VkDeviceSize offset,
                                     const void *data, VkDeviceSize size) {
    if (size == 0)
        return FLUX_OK;

    VkBuffer staging = VK_NULL_HANDLE;
    flux_vk_alloc staging_alloc = {0};
    VkCommandPool xfer_pool = VK_NULL_HANDLE;
    VkCommandBuffer xfer_cmd = VK_NULL_HANDLE;
    VkCommandPool gfx_pool = VK_NULL_HANDLE;
    VkCommandBuffer gfx_cmd = VK_NULL_HANDLE;
    VkSemaphore handoff = VK_NULL_HANDLE;
    VkResult vr = VK_SUCCESS;
    flux_result r = flux_vk_alloc_buffer(d, size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                         VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                             VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                                         /*wants_device_address=*/false, &staging, &staging_alloc);
    if (r != FLUX_OK)
        return r;

    /* HOST_VISIBLE staging buffers are always pre-mapped by the
     * allocator (one mapping per VkDeviceMemory, set at block-create
     * time). If this is NULL the allocator invariant is broken. */
    if (!staging_alloc.mapped) {
        FLUX_FAIL(FLUX_ERROR_BACKEND_FAILURE,
                  "staging allocation came back un-mapped (allocator invariant violated)");
        vr = VK_ERROR_UNKNOWN;
        goto fail;
    }
    memcpy(staging_alloc.mapped, data, size);

    bool use_xfer = flux_vk_prefer_transfer_queue(d);

    vr = flux_vk_new_transient_cmd(d, use_xfer ? d->transfer_family : d->graphics_family,
                                   &xfer_pool, &xfer_cmd);
    if (vr != VK_SUCCESS) {
        FLUX_FAIL_VK(FLUX_ERROR_BACKEND_FAILURE, "upload cmd alloc failed", vr);
        goto fail;
    }

    VkBufferCopy region = {.srcOffset = 0, .dstOffset = offset, .size = size};
    vkCmdCopyBuffer(xfer_cmd, staging, dst, 1, &region);

    if (use_xfer) {
        /* Release ownership transfer-family -> graphics-family. */
        VkBufferMemoryBarrier2 rel = {
            .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2,
            .srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT,
            .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
            .dstStageMask = 0, /* required for release half of QFOT */
            .dstAccessMask = 0,
            .srcQueueFamilyIndex = d->transfer_family,
            .dstQueueFamilyIndex = d->graphics_family,
            .buffer = dst,
            .offset = offset,
            .size = size,
        };
        VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                               .bufferMemoryBarrierCount = 1,
                               .pBufferMemoryBarriers = &rel};
        vkCmdPipelineBarrier2(xfer_cmd, &di);
    }

    vr = vkEndCommandBuffer(xfer_cmd);
    if (vr != VK_SUCCESS) {
        FLUX_FAIL_VK(FLUX_ERROR_BACKEND_FAILURE, "vkEndCommandBuffer (upload) failed", vr);
        goto fail;
    }

    if (use_xfer) {
        VkSemaphoreCreateInfo sci = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        vr = vkCreateSemaphore(d->device, &sci, nullptr, &handoff);
        if (vr != VK_SUCCESS) {
            FLUX_FAIL_VK(FLUX_ERROR_BACKEND_FAILURE, "vkCreateSemaphore (upload handoff) failed",
                         vr);
            goto fail;
        }
        /* Acquire on graphics. */
        vr = flux_vk_new_transient_cmd(d, d->graphics_family, &gfx_pool, &gfx_cmd);
        if (vr != VK_SUCCESS) {
            FLUX_FAIL_VK(FLUX_ERROR_BACKEND_FAILURE, "upload acquire cmd alloc failed", vr);
            goto fail;
        }
        VkBufferMemoryBarrier2 acq = {
            .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2,
            .srcStageMask = 0, /* required for acquire half of QFOT */
            .srcAccessMask = 0,
            .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
            .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT,
            .srcQueueFamilyIndex = d->transfer_family,
            .dstQueueFamilyIndex = d->graphics_family,
            .buffer = dst,
            .offset = offset,
            .size = size,
        };
        VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                               .bufferMemoryBarrierCount = 1,
                               .pBufferMemoryBarriers = &acq};
        vkCmdPipelineBarrier2(gfx_cmd, &di);
        vr = vkEndCommandBuffer(gfx_cmd);
        if (vr != VK_SUCCESS) {
            FLUX_FAIL_VK(FLUX_ERROR_BACKEND_FAILURE, "vkEndCommandBuffer (acquire) failed", vr);
            goto fail;
        }
        /* Submit transfer-then-graphics with the handoff semaphore. */
        vr = submit_and_wait(d, d->transfer_queue, xfer_cmd, VK_NULL_HANDLE, 0, handoff,
                             VK_PIPELINE_STAGE_2_COPY_BIT);
        if (vr != VK_SUCCESS) {
            FLUX_FAIL_VK(FLUX_ERROR_BACKEND_FAILURE, "transfer-queue submit failed", vr);
            goto fail;
        }
        vr = submit_and_wait(d, d->graphics_queue, gfx_cmd, handoff,
                             VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_NULL_HANDLE, 0);
        if (vr != VK_SUCCESS) {
            FLUX_FAIL_VK(FLUX_ERROR_BACKEND_FAILURE, "graphics-queue acquire submit failed", vr);
            goto fail;
        }
    } else {
        vr = flux_vk_submit_one_shot_and_wait(d, xfer_cmd);
        if (vr != VK_SUCCESS) {
            FLUX_FAIL_VK(FLUX_ERROR_BACKEND_FAILURE, "upload submit failed", vr);
            goto fail;
        }
    }

fail:
    if (handoff)
        vkDestroySemaphore(d->device, handoff, nullptr);
    if (gfx_pool)
        vkDestroyCommandPool(d->device, gfx_pool, nullptr);
    if (xfer_pool)
        vkDestroyCommandPool(d->device, xfer_pool, nullptr);
    if (staging)
        vkDestroyBuffer(d->device, staging, nullptr);
    if (staging_alloc.memory)
        flux_vk_deallocate(d, &staging_alloc);
    return vr == VK_SUCCESS ? FLUX_OK : FLUX_ERROR_BACKEND_FAILURE;
}

/* ------------------------------------------------------------------ */
/*  One-shot image upload                                             */
/* ------------------------------------------------------------------ */

flux_result flux_vk_upload_to_image(flux_device *d, VkImage dst, int32_t offset_x, int32_t offset_y,
                                    uint32_t width, uint32_t height, VkImageLayout old_layout,
                                    const void *data, size_t bytes) {
    if (bytes == 0)
        return FLUX_OK;

    VkBuffer staging = VK_NULL_HANDLE;
    flux_vk_alloc staging_alloc = {0};
    VkCommandPool xfer_pool = VK_NULL_HANDLE;
    VkCommandBuffer xfer_cmd = VK_NULL_HANDLE;
    VkCommandPool gfx_pool = VK_NULL_HANDLE;
    VkCommandBuffer gfx_cmd = VK_NULL_HANDLE;
    VkSemaphore handoff = VK_NULL_HANDLE;
    VkResult vr = VK_SUCCESS;
    flux_result r = flux_vk_alloc_buffer(d, bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                         VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                             VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                                         /*wants_device_address=*/false, &staging, &staging_alloc);
    if (r != FLUX_OK)
        return r;

    if (!staging_alloc.mapped) {
        FLUX_FAIL(FLUX_ERROR_BACKEND_FAILURE,
                  "image staging allocation came back un-mapped (allocator invariant violated)");
        vr = VK_ERROR_UNKNOWN;
        goto fail;
    }
    memcpy(staging_alloc.mapped, data, bytes);

    /* When the image is already SHADER_READ_ONLY_OPTIMAL we're updating
     * a resource that may still be sampled by an in-flight frame on the
     * graphics queue. Same-queue submissions are implicitly ordered, so
     * a graphics-queue submission correctly waits on the prior frame's
     * fragment-shader reads before the layout transition. A dedicated
     * transfer queue has *no* implicit ordering against the graphics
     * queue, so the SHADER_READ → TRANSFER_DST transition would race
     * with in-flight reads. The proper fix on a transfer queue is a
     * graphics-release → transfer-acquire QFOT dance, but that costs
     * two extra submissions + two semaphores + four barriers — strictly
     * worse than just recording the update on the graphics queue, which
     * is what every production renderer does for live-image updates.
     * Initial-upload path (old_layout==UNDEFINED) keeps the transfer
     * queue since the resource is not yet in flight. */
    bool use_xfer = flux_vk_prefer_transfer_queue(d) && old_layout == VK_IMAGE_LAYOUT_UNDEFINED;

    vr = flux_vk_new_transient_cmd(d, use_xfer ? d->transfer_family : d->graphics_family,
                                   &xfer_pool, &xfer_cmd);
    if (vr != VK_SUCCESS) {
        FLUX_FAIL_VK(FLUX_ERROR_BACKEND_FAILURE, "image upload cmd alloc failed", vr);
        goto fail;
    }

    /* On the transfer/graphics command buffer: old_layout -> TRANSFER_DST,
     * copy, then either TRANSFER_DST -> SHADER_READ_ONLY (single-queue)
     * or TRANSFER_DST release-ownership barrier (dual-queue path). The
     * srcAccessMask covers prior shader reads when updating an
     * already-sampled image. */
    {
        bool from_shader = old_layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        VkImageMemoryBarrier2 b = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
            .srcStageMask = from_shader ? VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT
                                        : VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT,
            .srcAccessMask = from_shader ? VK_ACCESS_2_SHADER_SAMPLED_READ_BIT : 0,
            .dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT,
            .dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
            .oldLayout = old_layout,
            .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            .image = dst,
            .subresourceRange =
                {
                    .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                    .levelCount = 1,
                    .layerCount = 1,
                },
        };
        VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                               .imageMemoryBarrierCount = 1,
                               .pImageMemoryBarriers = &b};
        vkCmdPipelineBarrier2(xfer_cmd, &di);
    }

    VkBufferImageCopy region = {
        .bufferOffset = 0,
        .imageSubresource =
            {
                .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                .mipLevel = 0,
                .baseArrayLayer = 0,
                .layerCount = 1,
            },
        .imageOffset = {offset_x, offset_y, 0},
        .imageExtent = {width, height, 1},
    };
    vkCmdCopyBufferToImage(xfer_cmd, staging, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                           &region);

    if (use_xfer) {
        /* Release-ownership barrier on the transfer queue. The
         * matching acquire (+ final layout transition to
         * SHADER_READ_ONLY) is recorded on the graphics queue. */
        VkImageMemoryBarrier2 rel = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
            .srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT,
            .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
            .dstStageMask = 0,
            .dstAccessMask = 0,
            .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            .srcQueueFamilyIndex = d->transfer_family,
            .dstQueueFamilyIndex = d->graphics_family,
            .image = dst,
            .subresourceRange =
                {
                    .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                    .levelCount = 1,
                    .layerCount = 1,
                },
        };
        VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                               .imageMemoryBarrierCount = 1,
                               .pImageMemoryBarriers = &rel};
        vkCmdPipelineBarrier2(xfer_cmd, &di);
    } else {
        /* Single-queue path: also transition to SHADER_READ_ONLY here. */
        VkImageMemoryBarrier2 b = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
            .srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT,
            .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
            .dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
            .dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            .image = dst,
            .subresourceRange =
                {
                    .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                    .levelCount = 1,
                    .layerCount = 1,
                },
        };
        VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                               .imageMemoryBarrierCount = 1,
                               .pImageMemoryBarriers = &b};
        vkCmdPipelineBarrier2(xfer_cmd, &di);
    }

    vr = vkEndCommandBuffer(xfer_cmd);
    if (vr != VK_SUCCESS) {
        FLUX_FAIL_VK(FLUX_ERROR_BACKEND_FAILURE, "vkEndCommandBuffer (image upload) failed", vr);
        goto fail;
    }

    if (use_xfer) {
        VkSemaphoreCreateInfo sci = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        vr = vkCreateSemaphore(d->device, &sci, nullptr, &handoff);
        if (vr != VK_SUCCESS) {
            FLUX_FAIL_VK(FLUX_ERROR_BACKEND_FAILURE, "vkCreateSemaphore (image handoff) failed",
                         vr);
            goto fail;
        }
        /* Graphics-queue: acquire + final layout transition. */
        vr = flux_vk_new_transient_cmd(d, d->graphics_family, &gfx_pool, &gfx_cmd);
        if (vr != VK_SUCCESS) {
            FLUX_FAIL_VK(FLUX_ERROR_BACKEND_FAILURE, "image acquire cmd alloc failed", vr);
            goto fail;
        }
        VkImageMemoryBarrier2 acq = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
            .srcStageMask = 0,
            .srcAccessMask = 0,
            .dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
            .dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            .srcQueueFamilyIndex = d->transfer_family,
            .dstQueueFamilyIndex = d->graphics_family,
            .image = dst,
            .subresourceRange =
                {
                    .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                    .levelCount = 1,
                    .layerCount = 1,
                },
        };
        VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                               .imageMemoryBarrierCount = 1,
                               .pImageMemoryBarriers = &acq};
        vkCmdPipelineBarrier2(gfx_cmd, &di);
        vr = vkEndCommandBuffer(gfx_cmd);
        if (vr != VK_SUCCESS) {
            FLUX_FAIL_VK(FLUX_ERROR_BACKEND_FAILURE, "vkEndCommandBuffer (acquire) failed", vr);
            goto fail;
        }
        vr = submit_and_wait(d, d->transfer_queue, xfer_cmd, VK_NULL_HANDLE, 0, handoff,
                             VK_PIPELINE_STAGE_2_COPY_BIT);
        if (vr != VK_SUCCESS) {
            FLUX_FAIL_VK(FLUX_ERROR_BACKEND_FAILURE, "transfer-queue image submit failed", vr);
            goto fail;
        }
        vr = submit_and_wait(d, d->graphics_queue, gfx_cmd, handoff,
                             VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_NULL_HANDLE, 0);
        if (vr != VK_SUCCESS) {
            FLUX_FAIL_VK(FLUX_ERROR_BACKEND_FAILURE, "graphics-queue image acquire failed", vr);
            goto fail;
        }
    } else {
        vr = flux_vk_submit_one_shot_and_wait(d, xfer_cmd);
        if (vr != VK_SUCCESS) {
            FLUX_FAIL_VK(FLUX_ERROR_BACKEND_FAILURE, "image upload submit failed", vr);
            goto fail;
        }
    }

fail:
    if (handoff)
        vkDestroySemaphore(d->device, handoff, nullptr);
    if (gfx_pool)
        vkDestroyCommandPool(d->device, gfx_pool, nullptr);
    if (xfer_pool)
        vkDestroyCommandPool(d->device, xfer_pool, nullptr);
    if (staging)
        vkDestroyBuffer(d->device, staging, nullptr);
    if (staging_alloc.memory)
        flux_vk_deallocate(d, &staging_alloc);
    return vr == VK_SUCCESS ? FLUX_OK : FLUX_ERROR_BACKEND_FAILURE;
}

/* ------------------------------------------------------------------ */
/*  One-shot layout transition                                        */
/* ------------------------------------------------------------------ */

flux_result flux_vk_transition_image_layout(flux_device *d, VkImage img, VkImageLayout old_layout,
                                            VkImageLayout new_layout) {
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkResult vr;

    VkCommandPoolCreateInfo pci = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .queueFamilyIndex = d->graphics_family,
        .flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT,
    };
    vr = vkCreateCommandPool(d->device, &pci, nullptr, &pool);
    if (vr != VK_SUCCESS) {
        FLUX_FAIL_VK(FLUX_ERROR_BACKEND_FAILURE, "transition vkCreateCommandPool failed", vr);
        goto fail;
    }
    VkCommandBufferAllocateInfo cbai = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };
    vr = vkAllocateCommandBuffers(d->device, &cbai, &cmd);
    if (vr != VK_SUCCESS) {
        FLUX_FAIL_VK(FLUX_ERROR_BACKEND_FAILURE, "transition vkAllocateCommandBuffers failed", vr);
        goto fail;
    }
    VkCommandBufferBeginInfo cbbi = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    vr = vkBeginCommandBuffer(cmd, &cbbi);
    if (vr != VK_SUCCESS) {
        FLUX_FAIL_VK(FLUX_ERROR_BACKEND_FAILURE, "transition vkBeginCommandBuffer failed", vr);
        goto fail;
    }

    VkImageMemoryBarrier2 b = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .srcStageMask = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT,
        .srcAccessMask = 0,
        .dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
        .dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
        .oldLayout = old_layout,
        .newLayout = new_layout,
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

    vr = vkEndCommandBuffer(cmd);
    if (vr != VK_SUCCESS) {
        FLUX_FAIL_VK(FLUX_ERROR_BACKEND_FAILURE, "transition vkEndCommandBuffer failed", vr);
        goto fail;
    }

    vr = flux_vk_submit_one_shot_and_wait(d, cmd);

fail:
    if (pool)
        vkDestroyCommandPool(d->device, pool, nullptr);
    if (vr != VK_SUCCESS) {
        FLUX_FAIL_VK(FLUX_ERROR_BACKEND_FAILURE, "image layout transition failed", vr);
        return FLUX_ERROR_BACKEND_FAILURE;
    }
    return FLUX_OK;
}
