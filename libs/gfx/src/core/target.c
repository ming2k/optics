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
#include "image_internal.h"
#include "internal.h"
#include <optics/gfx/vulkan.h>

#include <stdatomic.h>

optics_gfx_result optics_gfx_target_create(optics_gfx_device *d, const optics_gfx_target_desc *desc,
                                           optics_gfx_target **out) {
    if (!d || !desc || !out)
        return OPTICS_GFX_ERROR_INVALID_ARGUMENT;
    if (desc->type != OPTICS_GFX_TYPE_TARGET_DESC) {
        OPTICS_GFX_FAIL(OPTICS_GFX_ERROR_INVALID_ARGUMENT, "desc->type != FLUX_TYPE_TARGET_DESC");
        return OPTICS_GFX_ERROR_INVALID_ARGUMENT;
    }
    if (desc->width == 0 || desc->height == 0) {
        OPTICS_GFX_FAIL(OPTICS_GFX_ERROR_INVALID_ARGUMENT, "target width/height is 0");
        return OPTICS_GFX_ERROR_INVALID_ARGUMENT;
    }
    bool is_depth = desc->usage & OPTICS_GFX_TARGET_DEPTH;
    bool is_color = desc->usage & OPTICS_GFX_TARGET_COLOR;
    if (!is_depth && !is_color) {
        OPTICS_GFX_FAIL(OPTICS_GFX_ERROR_INVALID_ARGUMENT,
                        "target usage must set FLUX_TARGET_DEPTH or FLUX_TARGET_COLOR");
        return OPTICS_GFX_ERROR_INVALID_ARGUMENT;
    }
    if (desc->format == OPTICS_GFX_FORMAT_UNDEFINED) {
        OPTICS_GFX_FAIL(OPTICS_GFX_ERROR_INVALID_ARGUMENT,
                        "target format is FLUX_FORMAT_UNDEFINED");
        return OPTICS_GFX_ERROR_INVALID_ARGUMENT;
    }
    *out = nullptr;

    optics_gfx_target *t = optics_gfx_internal_alloc(d, sizeof(*t));
    if (!t)
        return OPTICS_GFX_ERROR_OUT_OF_MEMORY;
    atomic_init(&t->ref_count, 1u);
    t->device = optics_gfx_device_retain(d);
    t->image = VK_NULL_HANDLE;
    t->view = VK_NULL_HANDLE;
    t->format = optics_gfx_format_to_vk(desc->format);
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
    optics_gfx_result r = optics_gfx_vk_alloc_image(d, &ici, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                                                    &t->image, &t->alloc);
    if (r != OPTICS_GFX_OK)
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
        OPTICS_GFX_FAIL(OPTICS_GFX_ERROR_BACKEND_FAILURE, "target vkCreateImageView failed");
        r = OPTICS_GFX_ERROR_BACKEND_FAILURE;
        goto fail;
    }

    *out = t;
    OPTICS_GFX_OBSERVE_ACQUIRE(d, OPTICS_GFX_SCOPE_DEVICE, OPTICS_GFX_LIFETIME_CONTEXT, "vk_target",
                               "flux/core", 0, OPTICS_GFX_SITE_VK_TARGET);
    return OPTICS_GFX_OK;

fail:
    if (t->view)
        vkDestroyImageView(d->device, t->view, nullptr);
    if (t->image)
        vkDestroyImage(d->device, t->image, nullptr);
    if (t->alloc.memory)
        optics_gfx_vk_deallocate(d, &t->alloc);
    optics_gfx_device_release(d);
    optics_gfx_internal_free(d, t);
    return r;
}

optics_gfx_result optics_gfx_target_create_cpu(const optics_gfx_cpu_target_desc *desc,
                                               optics_gfx_target **out) {
    if (out)
        *out = nullptr;
    if (!desc || !out)
        return OPTICS_GFX_ERROR_INVALID_ARGUMENT;
    if (desc->type != OPTICS_GFX_TYPE_CPU_TARGET_DESC) {
        OPTICS_GFX_FAIL(OPTICS_GFX_ERROR_INVALID_ARGUMENT,
                        "desc->type != FLUX_TYPE_CPU_TARGET_DESC");
        return OPTICS_GFX_ERROR_INVALID_ARGUMENT;
    }
    if (desc->width == 0 || desc->height == 0) {
        OPTICS_GFX_FAIL(OPTICS_GFX_ERROR_INVALID_ARGUMENT, "target width/height is 0");
        return OPTICS_GFX_ERROR_INVALID_ARGUMENT;
    }
    if (desc->format != OPTICS_GFX_FORMAT_UNDEFINED &&
        desc->format != OPTICS_GFX_FORMAT_RGBA8_UNORM) {
        OPTICS_GFX_FAIL(OPTICS_GFX_ERROR_UNSUPPORTED, "CPU targets require RGBA8_UNORM");
        return OPTICS_GFX_ERROR_UNSUPPORTED;
    }
    size_t stride = desc->stride_bytes ? desc->stride_bytes : (size_t)desc->width * 4;
    if (desc->width > UINT32_MAX / 4 || stride < (size_t)desc->width * 4 || stride > UINT32_MAX ||
        desc->height > SIZE_MAX / stride) {
        OPTICS_GFX_FAIL(OPTICS_GFX_ERROR_OUT_OF_RANGE, "CPU target stride or extent overflow");
        return OPTICS_GFX_ERROR_OUT_OF_RANGE;
    }
    if (desc->user_buffer &&
        desc->buffer_bytes < (size_t)(desc->height - 1) * stride + (size_t)desc->width * 4) {
        OPTICS_GFX_FAIL(OPTICS_GFX_ERROR_OUT_OF_RANGE, "CPU target external buffer is too small");
        return OPTICS_GFX_ERROR_OUT_OF_RANGE;
    }
    *out = nullptr;

    optics_gfx_target *t = calloc(1, sizeof(*t));
    if (!t)
        return OPTICS_GFX_ERROR_OUT_OF_MEMORY;
    atomic_init(&t->ref_count, 1u);
    t->is_cpu = true;
    t->width = desc->width;
    t->height = desc->height;
    t->usage = OPTICS_GFX_TARGET_COLOR;
    t->cpu_stride = desc->stride_bytes ? desc->stride_bytes : (size_t)desc->width * 4u;

    if (desc->user_buffer) {
        t->cpu_buffer = (uint8_t *)desc->user_buffer;
        t->owns_cpu_buffer = false;
    } else {
        t->cpu_buffer = calloc(t->height, t->cpu_stride);
        if (!t->cpu_buffer) {
            free(t);
            return OPTICS_GFX_ERROR_OUT_OF_MEMORY;
        }
        t->owns_cpu_buffer = true;
    }
    *out = t;
    return OPTICS_GFX_OK;
}

optics_gfx_result optics_gfx_target_create_from_image(optics_gfx_device *d, optics_gfx_image *image,
                                                      optics_gfx_target **out) {
    if (out)
        *out = nullptr;
    if (!d || !image || !out || image->device != d)
        return OPTICS_GFX_ERROR_INVALID_ARGUMENT;
    *out = nullptr;

    optics_gfx_target *t = optics_gfx_internal_alloc(d, sizeof(*t));
    if (!t)
        return OPTICS_GFX_ERROR_OUT_OF_MEMORY;
    atomic_init(&t->ref_count, 1u);
    t->device = optics_gfx_device_retain(d);
    t->from_image = optics_gfx_image_retain(image);
    t->image = image->image;
    t->view = image->view;
    t->width = image->width;
    t->height = image->height;
    t->usage = OPTICS_GFX_TARGET_COLOR;
    *out = t;
    return OPTICS_GFX_OK;
}

const uint8_t *optics_gfx_target_cpu_pixels(const optics_gfx_target *t, uint32_t *out_width,
                                            uint32_t *out_height, uint32_t *out_stride) {
    if (!t || !t->is_cpu || t->in_use)
        return nullptr;
    if (out_width)
        *out_width = t->width;
    if (out_height)
        *out_height = t->height;
    if (out_stride)
        *out_stride = (uint32_t)t->cpu_stride;
    return t->cpu_buffer;
}

optics_gfx_target *optics_gfx_target_retain(optics_gfx_target *t) {
    if (t)
        atomic_fetch_add_explicit(&t->ref_count, 1u, memory_order_relaxed);
    return t;
}

void optics_gfx_target_release(optics_gfx_target *t) {
    if (!t)
        return;
    if (atomic_fetch_sub_explicit(&t->ref_count, 1u, memory_order_acq_rel) != 1u)
        return;

    if (t->is_cpu) {
        if (t->owns_cpu_buffer && t->cpu_buffer)
            free(t->cpu_buffer);
        free(t);
        return;
    }

    if (t->from_image) {
        optics_gfx_image_release(t->from_image);
        optics_gfx_device *d = t->device;
        optics_gfx_internal_free(d, t);
        optics_gfx_device_release(d);
        return;
    }

    optics_gfx_device *d = t->device;
    /* Same in-flight hazard as flux_image_release: the target may still
     * be an attachment of batches executing on the graphics queue. Park
     * the pieces on the device retire queue — a target owns no bindless
     * slots and no imported memory. */
    optics_gfx_device_retire_image(d, t->view, t->image, &t->alloc, VK_NULL_HANDLE, 0,
                                   OPTICS_GFX_BINDLESS_INVALID, OPTICS_GFX_BINDLESS_INVALID);
    OPTICS_GFX_OBSERVE_RELEASE(d, OPTICS_GFX_SCOPE_DEVICE, "vk_target", "flux/core", 0);
    optics_gfx_internal_free(d, t);
    optics_gfx_device_release(d);
}

uint32_t optics_gfx_target_width(const optics_gfx_target *t) {
    return t ? t->width : 0;
}
uint32_t optics_gfx_target_height(const optics_gfx_target *t) {
    return t ? t->height : 0;
}

void optics_gfx_frame_prepare_target(optics_gfx_frame *f, const optics_gfx_target *t) {
    if (!f || !t || !t->image)
        return;
    VkCommandBuffer cmd = optics_gfx_frame_vk_command_buffer(f);
    if (!cmd)
        return;

    bool depth = (t->usage & OPTICS_GFX_TARGET_DEPTH) != 0;
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

VkImage optics_gfx_target_vk_image(const optics_gfx_target *t) {
    return t ? t->image : VK_NULL_HANDLE;
}
VkImageView optics_gfx_target_vk_view(const optics_gfx_target *t) {
    return t ? t->view : VK_NULL_HANDLE;
}
