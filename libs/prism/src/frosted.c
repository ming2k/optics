/*
 * Classic non-distorting frosted glass material implementation.
 */

#include <prism/frosted.h>

#include <flux/compute.h>
#include <flux/vulkan.h>

#include <math.h>
#include <stdalign.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#if defined(__has_embed) && !defined(PRISM_SHADER_NO_EMBED)
#if __has_embed("frosted.comp.spv") && __has_embed("frosted_16f.comp.spv") &&                       \
    __has_embed("storage_clear.comp.spv") && __has_embed("storage_clear_16f.comp.spv")
#define PRISM_FROSTED_SHADERS_EMBED 1
#endif
#endif

#ifdef PRISM_FROSTED_SHADERS_EMBED
alignas(uint32_t) static const unsigned char frosted_spv[] = {
#embed "frosted.comp.spv"
};
alignas(uint32_t) static const unsigned char frosted_16f_spv[] = {
#embed "frosted_16f.comp.spv"
};
alignas(uint32_t) static const unsigned char storage_clear_spv[] = {
#embed "storage_clear.comp.spv"
};
alignas(uint32_t) static const unsigned char storage_clear_16f_spv[] = {
#embed "storage_clear_16f.comp.spv"
};
#else
#include "frosted_16f_spv.h"
#include "frosted_spv.h"
#include "storage_clear_16f_spv.h"
#include "storage_clear_spv.h"
#endif

#define FROSTED_MAX_GROUPS 64u

typedef struct frosted_region {
    uint32_t x;
    uint32_t y;
    uint32_t width;
    uint32_t height;
} frosted_region;

typedef struct frosted_push {
    uint32_t input_handle;
    uint32_t blurred_handle;
    uint32_t sampler_handle;
    uint32_t output_handle;
    uint32_t width;
    uint32_t height;
    uint32_t origin_x;
    uint32_t origin_y;
    float bounds[4];
    float corner_radius;
    float opacity;
    float saturation;
    float tint_strength;
    float noise_intensity;
    float shadow_alpha;
    float shadow_blur;
    float shadow_offset_y;
    uint32_t tint_color;
    uint32_t group_width;
    uint32_t group_height;
} frosted_push;

typedef struct storage_clear_push {
    uint32_t output_handle;
    uint32_t width;
    uint32_t height;
    uint32_t origin_x;
    uint32_t origin_y;
    uint32_t region_width;
    uint32_t region_height;
} storage_clear_push;

static_assert(sizeof(frosted_push) == 92, "frosted_push size mismatch");
static_assert(sizeof(storage_clear_push) == 28, "storage_clear_push size mismatch");

#define PRISM_WG 16u

static flux_format prism_output_format(const flux_image *input) {
    return flux_image_format(input) == FLUX_FORMAT_RGBA16_SFLOAT ? FLUX_FORMAT_RGBA16_SFLOAT
                                                                 : FLUX_FORMAT_RGBA8_UNORM;
}

typedef struct frosted_filter_slot {
    uint32_t width;
    uint32_t height;
    flux_format format;
    flux_image *output;
    bool initialized;
    uint32_t previous_count;
    frosted_region previous[FROSTED_MAX_GROUPS];
} frosted_filter_slot;

struct prism_frosted_filter {
    atomic_uint ref_count;
    flux_device *device;
    flux_compute_pipeline *frosted_pipelines[2];
    flux_compute_pipeline *clear_pipelines[2];
    frosted_filter_slot slots[FLUX_MAX_FRAMES_IN_FLIGHT];
};

static void barrier_compute_write_to_read(VkCommandBuffer cmd, VkImage image) {
    VkImageMemoryBarrier2 b = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        .srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
        .dstStageMask =
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
        .dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
        .newLayout = VK_IMAGE_LAYOUT_GENERAL,
        .image = image,
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
}

static void barrier_reuse_to_compute_write(VkCommandBuffer cmd, VkImage image) {
    VkImageMemoryBarrier2 b = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .srcStageMask =
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
        .srcAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        .dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
        .newLayout = VK_IMAGE_LAYOUT_GENERAL,
        .image = image,
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
}

static void barrier_compute_write_to_read_write(VkCommandBuffer cmd, VkImage image) {
    VkImageMemoryBarrier2 barrier = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        .srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        .dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
        .newLayout = VK_IMAGE_LAYOUT_GENERAL,
        .image = image,
        .subresourceRange =
            {
                .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                .levelCount = 1,
                .layerCount = 1,
            },
    };
    VkDependencyInfo info = {
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .imageMemoryBarrierCount = 1,
        .pImageMemoryBarriers = &barrier,
    };
    vkCmdPipelineBarrier2(cmd, &info);
}

flux_result prism_frosted_filter_create(flux_device *device, prism_frosted_filter **out) {
    if (!device || !out)
        return FLUX_ERROR_INVALID_ARGUMENT;
    *out = nullptr;
    prism_frosted_filter *filter = calloc(1, sizeof(*filter));
    if (!filter)
        return FLUX_ERROR_OUT_OF_MEMORY;
    atomic_init(&filter->ref_count, 1u);
    filter->device = flux_device_retain(device);
    *out = filter;
    return FLUX_OK;
}

prism_frosted_filter *prism_frosted_filter_retain(prism_frosted_filter *filter) {
    if (filter)
        atomic_fetch_add_explicit(&filter->ref_count, 1u, memory_order_relaxed);
    return filter;
}

void prism_frosted_filter_release(prism_frosted_filter *filter) {
    if (!filter)
        return;
    if (atomic_fetch_sub_explicit(&filter->ref_count, 1u, memory_order_acq_rel) != 1u)
        return;
    flux_device *device = filter->device;
    for (uint32_t i = 0; i < FLUX_MAX_FRAMES_IN_FLIGHT; ++i) {
        if (filter->slots[i].output)
            flux_image_release(filter->slots[i].output);
    }
    for (int i = 0; i < 2; ++i) {
        if (filter->frosted_pipelines[i])
            flux_compute_pipeline_release(filter->frosted_pipelines[i]);
        if (filter->clear_pipelines[i])
            flux_compute_pipeline_release(filter->clear_pipelines[i]);
    }
    free(filter);
    flux_device_release(device);
}

static flux_result frosted_ensure_slot(prism_frosted_filter *filter, uint32_t index,
                                       const flux_image *input) {
    frosted_filter_slot *slot = &filter->slots[index];
    uint32_t width = flux_image_width(input);
    uint32_t height = flux_image_height(input);
    flux_format format = prism_output_format(input);
    if (slot->output && slot->width == width && slot->height == height && slot->format == format)
        return FLUX_OK;
    if (slot->output)
        flux_image_release(slot->output);
    *slot = (frosted_filter_slot){0};
    flux_result r =
        flux_image_create_compute_writable(filter->device, width, height, format, &slot->output);
    if (r != FLUX_OK)
        return r;
    slot->width = width;
    slot->height = height;
    slot->format = format;
    return FLUX_OK;
}

static flux_result frosted_ensure_pipelines(prism_frosted_filter *filter, bool is16f,
                                            bool need_clear, bool need_frosted) {
    const int cls = is16f ? 1 : 0;
    if (need_clear && !filter->clear_pipelines[cls]) {
        flux_compute_pipeline_desc pdesc = FLUX_COMPUTE_PIPELINE_DESC_INIT;
        pdesc.spirv = (const uint32_t *)(is16f ? storage_clear_16f_spv : storage_clear_spv);
        pdesc.spirv_word_count =
            (is16f ? sizeof(storage_clear_16f_spv) : sizeof(storage_clear_spv)) / sizeof(uint32_t);
        pdesc.entry_point = "main";
        pdesc.push_constant_bytes = sizeof(storage_clear_push);
        flux_result r =
            flux_compute_pipeline_create(filter->device, &pdesc, &filter->clear_pipelines[cls]);
        if (r != FLUX_OK)
            return r;
    }
    if (need_frosted && !filter->frosted_pipelines[cls]) {
        flux_compute_pipeline_desc pdesc = FLUX_COMPUTE_PIPELINE_DESC_INIT;
        pdesc.spirv = (const uint32_t *)(is16f ? frosted_16f_spv : frosted_spv);
        pdesc.spirv_word_count =
            (is16f ? sizeof(frosted_16f_spv) : sizeof(frosted_spv)) / sizeof(uint32_t);
        pdesc.entry_point = "main";
        pdesc.push_constant_bytes = sizeof(frosted_push);
        flux_result r =
            flux_compute_pipeline_create(filter->device, &pdesc, &filter->frosted_pipelines[cls]);
        if (r != FLUX_OK)
            return r;
    }
    return FLUX_OK;
}

static bool frosted_group_dispatch_bounds(const prism_frosted_group *group, uint32_t image_width,
                                          uint32_t image_height, frosted_region *out) {
    if (!group || !out || image_width == 0 || image_height == 0)
        return false;
    float pad = fmaxf(group->shadow_blur * 2.0f, 2.0f);
    int64_t x0 = (int64_t)floorf(group->shape.bounds.x - pad);
    int64_t y0 = (int64_t)floorf(group->shape.bounds.y - pad);
    int64_t x1 = (int64_t)ceilf(group->shape.bounds.x + group->shape.bounds.w + pad);
    int64_t y1 = (int64_t)ceilf(group->shape.bounds.y + group->shape.bounds.h + pad + fmaxf(group->shadow_offset_y, 0.0f));

    x0 = x0 < 0 ? 0 : x0;
    y0 = y0 < 0 ? 0 : y0;
    x1 = x1 > (int64_t)image_width ? (int64_t)image_width : x1;
    y1 = y1 > (int64_t)image_height ? (int64_t)image_height : y1;
    if (x1 <= x0 || y1 <= y0)
        return false;

    out->x = (uint32_t)x0;
    out->y = (uint32_t)y0;
    out->width = (uint32_t)(x1 - x0);
    out->height = (uint32_t)(y1 - y0);
    return true;
}

flux_result prism_frosted_filter_apply(prism_frosted_filter *filter, flux_frame *frame,
                                       const prism_frosted_desc *desc, flux_image **out) {
    if (!filter || !frame || !desc || !out)
        return FLUX_ERROR_INVALID_ARGUMENT;
    if (desc->type != PRISM_TYPE_FROSTED_DESC)
        return FLUX_ERROR_INVALID_ARGUMENT;
    if (!desc->input || !desc->blurred_input)
        return FLUX_ERROR_INVALID_ARGUMENT;
    if (desc->group_count > FROSTED_MAX_GROUPS)
        return FLUX_ERROR_INVALID_ARGUMENT;
    if (desc->group_count > 0 && !desc->groups)
        return FLUX_ERROR_INVALID_ARGUMENT;

    *out = nullptr;
    if (flux_frame_get_state(frame) != FLUX_FRAME_STATE_RECORDING ||
        flux_frame_has_active_pass(frame))
        return FLUX_ERROR_INVALID_STATE;

    uint32_t slot_index = flux_frame_index(frame);
    flux_result r = frosted_ensure_slot(filter, slot_index, desc->input);
    if (r != FLUX_OK)
        return r;
    frosted_filter_slot *slot = &filter->slots[slot_index];

    bool is16f = slot->format == FLUX_FORMAT_RGBA16_SFLOAT;
    r = frosted_ensure_pipelines(filter, is16f, slot->initialized || desc->group_count > 0,
                                 desc->group_count > 0);
    if (r != FLUX_OK)
        return r;

    VkCommandBuffer cmd = flux_frame_vk_command_buffer(frame);
    VkImage out_vk = flux_image_vk_image(slot->output);

    barrier_reuse_to_compute_write(cmd, out_vk);

    const int cls = is16f ? 1 : 0;
    uint32_t current_count = 0;
    frosted_region current_regions[FROSTED_MAX_GROUPS];

    for (uint32_t i = 0; i < desc->group_count; ++i) {
        frosted_region reg;
        if (frosted_group_dispatch_bounds(&desc->groups[i], slot->width, slot->height, &reg)) {
            current_regions[current_count++] = reg;
        }
    }

    // Clear previous footprints
    if (slot->initialized && filter->clear_pipelines[cls]) {
        for (uint32_t i = 0; i < slot->previous_count; ++i) {
            storage_clear_push pc = {
                .output_handle = flux_image_bindless_storage_handle(slot->output),
                .width = slot->width,
                .height = slot->height,
                .origin_x = slot->previous[i].x,
                .origin_y = slot->previous[i].y,
                .region_width = slot->previous[i].width,
                .region_height = slot->previous[i].height,
            };
            uint32_t gx = (pc.region_width + PRISM_WG - 1) / PRISM_WG;
            uint32_t gy = (pc.region_height + PRISM_WG - 1) / PRISM_WG;
            flux_compute_dispatch(cmd, filter->clear_pipelines[cls], &pc, sizeof(pc), gx, gy, 1);
        }
        if (slot->previous_count > 0 && desc->group_count > 0)
            barrier_compute_write_to_read_write(cmd, out_vk);
    }

    // Render frosted bodies
    if (desc->group_count > 0 && filter->frosted_pipelines[cls]) {
        for (uint32_t i = 0; i < desc->group_count; ++i) {
            const prism_frosted_group *g = &desc->groups[i];
            frosted_region reg;
            if (!frosted_group_dispatch_bounds(g, slot->width, slot->height, &reg))
                continue;

            frosted_push pc = {
                .input_handle = flux_image_bindless_storage_handle(desc->input),
                .blurred_handle = flux_image_bindless_storage_handle(desc->blurred_input),
                .sampler_handle = flux_device_default_sampler_handle(filter->device),
                .output_handle = flux_image_bindless_storage_handle(slot->output),
                .width = slot->width,
                .height = slot->height,
                .origin_x = reg.x,
                .origin_y = reg.y,
                .bounds = {g->shape.bounds.x, g->shape.bounds.y, g->shape.bounds.w, g->shape.bounds.h},
                .corner_radius = g->shape.corner_radius,
                .opacity = g->opacity * desc->opacity,
                .saturation = g->saturation >= 0.0f ? g->saturation : desc->saturation,
                .tint_strength = g->tint_strength >= 0.0f ? g->tint_strength : desc->tint_strength,
                .noise_intensity = g->noise_intensity >= 0.0f ? g->noise_intensity : desc->noise_intensity,
                .shadow_alpha = g->shadow_alpha,
                .shadow_blur = g->shadow_blur,
                .shadow_offset_y = g->shadow_offset_y,
                .tint_color = g->tint_color,
                .group_width = reg.width,
                .group_height = reg.height,
            };

            uint32_t gx = (reg.width + PRISM_WG - 1) / PRISM_WG;
            uint32_t gy = (reg.height + PRISM_WG - 1) / PRISM_WG;
            flux_compute_dispatch(cmd, filter->frosted_pipelines[cls], &pc, sizeof(pc), gx, gy, 1);

            if (i + 1 < desc->group_count)
                barrier_compute_write_to_read_write(cmd, out_vk);
        }
    }

    barrier_compute_write_to_read(cmd, out_vk);

    slot->initialized = true;
    slot->previous_count = current_count;
    memcpy(slot->previous, current_regions, current_count * sizeof(frosted_region));

    *out = slot->output;
    return FLUX_OK;
}
