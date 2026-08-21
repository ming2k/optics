/*
 * Refcounted render-target image + view.
 *
 * The depth/colour attachment a caller owns and feeds to a pass via
 * flux_pass_desc (peer-defined attachments, ADR-0001). flux owns the
 * VkImage, the backing GPU-allocator memory (flux_vk_alloc_image), and
 * the VkImageView; the refcount lifecycle mirrors flux_buffer /
 * flux_mesh. This is the replacement for the raw Vulkan
 * image/memory/view plumbing every depth-using example used to carry.
 */
#include "internal.h"
#include <flux/vulkan.h>

#include <stdatomic.h>

struct flux_target {
    atomic_uint ref_count;
    flux_device *device; /* retained */
    VkImage image;
    VkImageView view;
    flux_vk_alloc alloc;
    VkFormat format;
    uint32_t usage;
    uint32_t width;
    uint32_t height;
};

flux_result flux_target_create(flux_device *d, const flux_target_desc *desc, flux_target **out) {
    if (!d || !desc || !out)
        return FLUX_ERROR_INVALID_ARGUMENT;
    if (desc->type != FLUX_TYPE_TARGET_DESC) {
        FLUX_FAIL(FLUX_ERROR_INVALID_ARGUMENT, "desc->type != FLUX_TYPE_TARGET_DESC");
        return FLUX_ERROR_INVALID_ARGUMENT;
    }
    if (desc->width == 0 || desc->height == 0) {
        FLUX_FAIL(FLUX_ERROR_INVALID_ARGUMENT, "target width/height is 0");
        return FLUX_ERROR_INVALID_ARGUMENT;
    }
    bool is_depth = desc->usage & FLUX_TARGET_DEPTH;
    bool is_color = desc->usage & FLUX_TARGET_COLOR;
    if (!is_depth && !is_color) {
        FLUX_FAIL(FLUX_ERROR_INVALID_ARGUMENT,
                  "target usage must set FLUX_TARGET_DEPTH or FLUX_TARGET_COLOR");
        return FLUX_ERROR_INVALID_ARGUMENT;
    }
    if (desc->format == FLUX_FORMAT_UNDEFINED) {
        FLUX_FAIL(FLUX_ERROR_INVALID_ARGUMENT, "target format is FLUX_FORMAT_UNDEFINED");
        return FLUX_ERROR_INVALID_ARGUMENT;
    }
    *out = nullptr;

    flux_target *t = flux_internal_alloc(d, sizeof(*t));
    if (!t)
        return FLUX_ERROR_OUT_OF_MEMORY;
    atomic_init(&t->ref_count, 1u);
    t->device = flux_device_retain(d);
    t->image = VK_NULL_HANDLE;
    t->view = VK_NULL_HANDLE;
    t->format = flux_format_to_vk(desc->format);
    t->usage = desc->usage;
    t->width = desc->width;
    t->height = desc->height;

    VkFormat vfmt = t->format;

    VkImageCreateInfo ici = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = vfmt,
        .extent = {desc->width, desc->height, 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = is_depth ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT
                          : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    flux_result r =
        flux_vk_alloc_image(d, &ici, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &t->image, &t->alloc);
    if (r != FLUX_OK)
        goto fail;

    VkImageAspectFlags aspect;
    if (is_depth) {
        /* Combined depth/stencil formats carry both aspects; pure depth
         * formats carry only DEPTH. */
        aspect = (vfmt == VK_FORMAT_D24_UNORM_S8_UINT || vfmt == VK_FORMAT_D32_SFLOAT_S8_UINT)
                     ? (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)
                     : (VkImageAspectFlags)VK_IMAGE_ASPECT_DEPTH_BIT;
    } else {
        aspect = VK_IMAGE_ASPECT_COLOR_BIT;
    }

    VkImageViewCreateInfo ivci = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = t->image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = vfmt,
        .subresourceRange =
            {
                .aspectMask = aspect,
                .baseMipLevel = 0,
                .levelCount = 1,
                .baseArrayLayer = 0,
                .layerCount = 1,
            },
    };
    if (vkCreateImageView(d->device, &ivci, nullptr, &t->view) != VK_SUCCESS) {
        FLUX_FAIL(FLUX_ERROR_BACKEND_FAILURE, "target vkCreateImageView failed");
        r = FLUX_ERROR_BACKEND_FAILURE;
        goto fail;
    }

    *out = t;
    return FLUX_OK;

fail:
    if (t->view)
        vkDestroyImageView(d->device, t->view, nullptr);
    if (t->image)
        vkDestroyImage(d->device, t->image, nullptr);
    if (t->alloc.memory)
        flux_vk_deallocate(d, &t->alloc);
    flux_device_release(d);
    flux_internal_free(d, t);
    return r;
}

flux_target *flux_target_retain(flux_target *t) {
    if (t)
        atomic_fetch_add_explicit(&t->ref_count, 1u, memory_order_relaxed);
    return t;
}

void flux_target_release(flux_target *t) {
    if (!t)
        return;
    if (atomic_fetch_sub_explicit(&t->ref_count, 1u, memory_order_acq_rel) != 1u)
        return;
    flux_device *d = t->device;
    /* Same in-flight hazard as flux_image_release: the target may still
     * be an attachment of batches executing on the graphics queue. Park
     * the pieces on the device retire queue — a target owns no bindless
     * slots and no imported memory. */
    flux_device_retire_image(d, t->view, t->image, &t->alloc, VK_NULL_HANDLE, 0,
                             FLUX_BINDLESS_INVALID, FLUX_BINDLESS_INVALID);
    flux_internal_free(d, t);
    flux_device_release(d);
}

uint32_t flux_target_width(const flux_target *t) {
    return t ? t->width : 0;
}
uint32_t flux_target_height(const flux_target *t) {
    return t ? t->height : 0;
}

void flux_frame_prepare_target(flux_frame *f, const flux_target *t) {
    if (!f || !t || !t->image)
        return;
    VkCommandBuffer cmd = flux_frame_vk_command_buffer(f);
    if (!cmd)
        return;

    bool depth = (t->usage & FLUX_TARGET_DEPTH) != 0;
    VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT;
    VkImageLayout layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    VkPipelineStageFlags2 stage = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkAccessFlags2 access = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
    if (depth) {
        aspect = VK_IMAGE_ASPECT_DEPTH_BIT;
        if (t->format == VK_FORMAT_D24_UNORM_S8_UINT || t->format == VK_FORMAT_D32_SFLOAT_S8_UINT)
            aspect |= VK_IMAGE_ASPECT_STENCIL_BIT;
        layout = (aspect & VK_IMAGE_ASPECT_STENCIL_BIT)
                     ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL
                     : VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
        stage = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
                VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
        access = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                 VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    }

    VkImageMemoryBarrier2 barrier = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .srcStageMask = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT,
        .dstStageMask = stage,
        .dstAccessMask = access,
        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .newLayout = layout,
        .image = t->image,
        .subresourceRange = {.aspectMask = aspect, .levelCount = 1, .layerCount = 1},
    };
    VkDependencyInfo dep = {
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .imageMemoryBarrierCount = 1,
        .pImageMemoryBarriers = &barrier,
    };
    vkCmdPipelineBarrier2(cmd, &dep);
}

VkImage flux_target_vk_image(const flux_target *t) {
    return t ? t->image : VK_NULL_HANDLE;
}
VkImageView flux_target_vk_view(const flux_target *t) {
    return t ? t->view : VK_NULL_HANDLE;
}
