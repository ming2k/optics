/*
 * Acrylic material implementation.
 */

#include "prism_material_frame.h"
#include "prism_observe.h"
#include <prism/acrylic.h>

#include <flux/compute.h>
#include <flux/profile.h>
#include <flux/vulkan.h>

#include <math.h>
#include <stdalign.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#if defined(__has_embed) && !defined(PRISM_SHADER_NO_EMBED)
#if __has_embed("acrylic.comp.spv") && __has_embed("acrylic_16f.comp.spv") &&                      \
    __has_embed("storage_clear.comp.spv") && __has_embed("storage_clear_16f.comp.spv")
#define PRISM_ACRYLIC_SHADERS_EMBED 1
#endif
#endif

#ifdef PRISM_ACRYLIC_SHADERS_EMBED
alignas(uint32_t) static const unsigned char acrylic_spv[] = {
#embed "acrylic.comp.spv"
};
alignas(uint32_t) static const unsigned char acrylic_16f_spv[] = {
#embed "acrylic_16f.comp.spv"
};
alignas(uint32_t) static const unsigned char storage_clear_spv[] = {
#embed "storage_clear.comp.spv"
};
alignas(uint32_t) static const unsigned char storage_clear_16f_spv[] = {
#embed "storage_clear_16f.comp.spv"
};
#else
#include "acrylic_16f_spv.h"
#include "acrylic_spv.h"
#include "storage_clear_16f_spv.h"
#include "storage_clear_spv.h"
#endif

#define ACRYLIC_MAX_GROUPS 64u

typedef struct acrylic_region {
    uint32_t x;
    uint32_t y;
    uint32_t width;
    uint32_t height;
} acrylic_region;

typedef struct acrylic_push {
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
    float tint_strength;
    float luminance_plate;
    float noise_intensity;
    float border_width;
    float border_alpha;
    float shadow_alpha;
    float shadow_blur;
    float shadow_offset_y;
    uint32_t tint_color;
    uint32_t group_width;
    uint32_t group_height;
    float slab_origin_x;
    float slab_origin_y;
    float capture_width;
    float capture_height;
} acrylic_push;

typedef struct storage_clear_push {
    uint32_t output_handle;
    uint32_t width;
    uint32_t height;
    uint32_t origin_x;
    uint32_t origin_y;
    uint32_t region_width;
    uint32_t region_height;
} storage_clear_push;

static_assert(sizeof(acrylic_push) == 116, "acrylic_push size mismatch");
static_assert(sizeof(storage_clear_push) == 28, "storage_clear_push size mismatch");

#define PRISM_WG 16u

static flux_format prism_output_format(const flux_image *input) {
    return flux_image_format(input) == FLUX_FORMAT_RGBA16_SFLOAT ? FLUX_FORMAT_RGBA16_SFLOAT
                                                                 : FLUX_FORMAT_RGBA8_UNORM;
}

typedef struct acrylic_filter_slot {
    uint32_t width;
    uint32_t height;
    flux_format format;
    flux_image *output;
    bool initialized;
    uint32_t slab_x;
    uint32_t slab_y;
    uint32_t capture_width;
    uint32_t capture_height;
    uint32_t previous_count;
    acrylic_region previous[ACRYLIC_MAX_GROUPS];
} acrylic_filter_slot;

struct prism_acrylic_filter {
    atomic_uint ref_count;
    flux_device *device;
    flux_compute_pipeline *acrylic_pipelines[2];
    flux_compute_pipeline *clear_pipelines[2];
    acrylic_filter_slot slots[FLUX_MAX_FRAMES_IN_FLIGHT];
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

flux_result prism_acrylic_filter_create(flux_device *device, prism_acrylic_filter **out) {
    if (!device || !out)
        return FLUX_ERROR_INVALID_ARGUMENT;
    *out = nullptr;
    /* Caller's allocator, attributed to prism (ADR-0135). */
    prism_acrylic_filter *filter = nullptr;
    if (flux_device_alloc(device,
                          &(flux_device_alloc_opts){
                              .type = FLUX_TYPE_DEVICE_ALLOC_OPTS,
                              .bytes = sizeof(*filter),
                              .owner = PRISM_OBSERVE_OWNER,
                              .kind = PRISM_OBSERVE_KIND_ACRYLIC,
                              .scope = FLUX_SCOPE_DEVICE,
                          },
                          (void **)&filter) != FLUX_OK)
        return FLUX_ERROR_OUT_OF_MEMORY;
    atomic_init(&filter->ref_count, 1u);
    filter->device = flux_device_retain(device);
    *out = filter;
    return FLUX_OK;
}

prism_acrylic_filter *prism_acrylic_filter_retain(prism_acrylic_filter *filter) {
    if (filter)
        atomic_fetch_add_explicit(&filter->ref_count, 1u, memory_order_relaxed);
    return filter;
}

void prism_acrylic_filter_release(prism_acrylic_filter *filter) {
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
        if (filter->acrylic_pipelines[i])
            flux_compute_pipeline_release(filter->acrylic_pipelines[i]);
        if (filter->clear_pipelines[i])
            flux_compute_pipeline_release(filter->clear_pipelines[i]);
    }
    flux_device_free(device, &(flux_device_free_opts){
                                 .type = FLUX_TYPE_DEVICE_FREE_OPTS,
                                 .ptr = filter,
                                 .owner = PRISM_OBSERVE_OWNER,
                                 .kind = PRISM_OBSERVE_KIND_ACRYLIC,
                                 .scope = FLUX_SCOPE_DEVICE,
                             });
    flux_device_release(device);
}

static flux_result acrylic_ensure_slot(prism_acrylic_filter *filter, uint32_t index,
                                       const flux_image *input, uint32_t footprint_w,
                                       uint32_t footprint_h) {
    acrylic_filter_slot *slot = &filter->slots[index];
    uint32_t width = 0u;
    uint32_t height = 0u;
    prism_slab_for_footprint(footprint_w, footprint_h, &width, &height);
    flux_format format = prism_output_format(input);
    if (slot->output && slot->width == width && slot->height == height && slot->format == format)
        return FLUX_OK;
    if (slot->output)
        flux_image_release(slot->output);
    *slot = (acrylic_filter_slot){0};
    flux_result r =
        flux_image_create_compute_writable(filter->device, width, height, format, &slot->output);
    if (r != FLUX_OK)
        return r;
    slot->width = width;
    slot->height = height;
    slot->format = format;
    return FLUX_OK;
}

static flux_result acrylic_ensure_pipelines(prism_acrylic_filter *filter, bool is16f,
                                            bool need_clear, bool need_acrylic) {
    const int cls = is16f ? 1 : 0;
    const bool missing = (need_clear && !filter->clear_pipelines[cls]) ||
                         (need_acrylic && !filter->acrylic_pipelines[cls]);
    if (!missing)
        return FLUX_OK;

    FLUX_PROFILE_ZONE(z_compile, "prism.acrylic.compile_pipelines");

    if (need_clear && !filter->clear_pipelines[cls]) {
        flux_compute_pipeline_desc pdesc = FLUX_COMPUTE_PIPELINE_DESC_INIT;
        pdesc.spirv = (const uint32_t *)(is16f ? storage_clear_16f_spv : storage_clear_spv);
        pdesc.spirv_word_count =
            (is16f ? sizeof(storage_clear_16f_spv) : sizeof(storage_clear_spv)) / sizeof(uint32_t);
        pdesc.entry_point = "main";
        pdesc.push_constant_bytes = sizeof(storage_clear_push);
        flux_result r =
            flux_compute_pipeline_create(filter->device, &pdesc, &filter->clear_pipelines[cls]);
        if (r != FLUX_OK) {
            FLUX_PROFILE_ZONE_END(z_compile);
            return r;
        }
    }
    if (need_acrylic && !filter->acrylic_pipelines[cls]) {
        flux_compute_pipeline_desc pdesc = FLUX_COMPUTE_PIPELINE_DESC_INIT;
        pdesc.spirv = (const uint32_t *)(is16f ? acrylic_16f_spv : acrylic_spv);
        pdesc.spirv_word_count =
            (is16f ? sizeof(acrylic_16f_spv) : sizeof(acrylic_spv)) / sizeof(uint32_t);
        pdesc.entry_point = "main";
        pdesc.push_constant_bytes = sizeof(acrylic_push);
        flux_result r =
            flux_compute_pipeline_create(filter->device, &pdesc, &filter->acrylic_pipelines[cls]);
        if (r != FLUX_OK) {
            FLUX_PROFILE_ZONE_END(z_compile);
            return r;
        }
    }
    FLUX_PROFILE_ZONE_END(z_compile);
    return FLUX_OK;
}

flux_result prism_acrylic_filter_warmup(prism_acrylic_filter *filter,
                                        const prism_acrylic_warmup_opts *opts) {
    if (!filter)
        return FLUX_ERROR_INVALID_ARGUMENT;

    bool is16f = false;
    if (opts) {
        if (opts->format == FLUX_FORMAT_RGBA16_SFLOAT)
            is16f = true;
    }

    return acrylic_ensure_pipelines(filter, is16f, true, true);
}

static bool acrylic_group_dispatch_bounds(const prism_acrylic_group *group, uint32_t image_width,
                                          uint32_t image_height, acrylic_region *out) {
    if (!group || !out || image_width == 0 || image_height == 0)
        return false;
    float pad = fmaxf(group->shadow_blur * 2.0f, 2.0f);
    int64_t x0 = (int64_t)floorf(group->shape.bounds.x - pad);
    int64_t y0 = (int64_t)floorf(group->shape.bounds.y - pad);
    int64_t x1 = (int64_t)ceilf(group->shape.bounds.x + group->shape.bounds.w + pad);
    int64_t y1 = (int64_t)ceilf(group->shape.bounds.y + group->shape.bounds.h + pad +
                                fmaxf(group->shadow_offset_y, 0.0f));

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

flux_result prism_acrylic_filter_apply(prism_acrylic_filter *filter, flux_frame *frame,
                                       const prism_acrylic_desc *desc, prism_material_output *out) {
    if (!filter || !frame || !desc || !out)
        return FLUX_ERROR_INVALID_ARGUMENT;
    if (desc->type != PRISM_TYPE_ACRYLIC_DESC)
        return FLUX_ERROR_INVALID_ARGUMENT;
    if (!desc->input || !desc->blurred_input)
        return FLUX_ERROR_INVALID_ARGUMENT;
    if (desc->group_count > ACRYLIC_MAX_GROUPS)
        return FLUX_ERROR_INVALID_ARGUMENT;
    if (desc->group_count > 0 && !desc->groups)
        return FLUX_ERROR_INVALID_ARGUMENT;

    *out = (prism_material_output){0};
    if (flux_frame_get_state(frame) != FLUX_FRAME_STATE_RECORDING ||
        flux_frame_has_active_pass(frame))
        return FLUX_ERROR_INVALID_STATE;
    if (flux_image_bindless_handle(desc->input) == FLUX_BINDLESS_INVALID ||
        flux_image_bindless_handle(desc->blurred_input) == FLUX_BINDLESS_INVALID)
        return FLUX_ERROR_UNSUPPORTED;

    const uint32_t capture_width = flux_image_width(desc->input);
    const uint32_t capture_height = flux_image_height(desc->input);

    FLUX_PROFILE_ZONE(z_acrylic, "prism.acrylic.apply");

    uint32_t current_count = 0;
    acrylic_region current_regions[ACRYLIC_MAX_GROUPS];
    uint32_t footprint_x0 = UINT32_MAX, footprint_y0 = UINT32_MAX;
    uint32_t footprint_x1 = 0u, footprint_y1 = 0u;

    for (uint32_t i = 0; i < desc->group_count; ++i) {
        acrylic_region reg;
        if (acrylic_group_dispatch_bounds(&desc->groups[i], capture_width, capture_height, &reg)) {
            current_regions[current_count++] = reg;
            if (reg.x < footprint_x0)
                footprint_x0 = reg.x;
            if (reg.y < footprint_y0)
                footprint_y0 = reg.y;
            if (reg.x + reg.width > footprint_x1)
                footprint_x1 = reg.x + reg.width;
            if (reg.y + reg.height > footprint_y1)
                footprint_y1 = reg.y + reg.height;
        }
    }

    uint32_t footprint_w = 1u;
    uint32_t footprint_h = 1u;
    if (current_count > 0u) {
        footprint_w = footprint_x1 - footprint_x0;
        footprint_h = footprint_y1 - footprint_y0;
    }

    uint32_t slot_index = flux_frame_index(frame);
    flux_result r = acrylic_ensure_slot(filter, slot_index, desc->input, footprint_w, footprint_h);
    if (r != FLUX_OK)
        return r;
    acrylic_filter_slot *slot = &filter->slots[slot_index];

    const uint32_t slab_x = current_count > 0u ? footprint_x0 : 0u;
    const uint32_t slab_y = current_count > 0u ? footprint_y0 : 0u;
    bool slab_moved = slot->initialized && (slot->slab_x != slab_x || slot->slab_y != slab_y);

    for (uint32_t i = 0; i < current_count; ++i) {
        current_regions[i].x -= slab_x;
        current_regions[i].y -= slab_y;
    }
    slot->slab_x = slab_x;
    slot->slab_y = slab_y;
    slot->capture_width = capture_width;
    slot->capture_height = capture_height;

    bool is16f = slot->format == FLUX_FORMAT_RGBA16_SFLOAT;
    r = acrylic_ensure_pipelines(filter, is16f, true, desc->group_count > 0);
    if (r != FLUX_OK)
        return r;

    VkCommandBuffer cmd = flux_frame_vk_command_buffer(frame);
    VkImage out_vk = flux_image_vk_image(slot->output);

    barrier_reuse_to_compute_write(cmd, out_vk);

    const int cls = is16f ? 1 : 0;

    // Clear previous footprints or initialize full output on first use or slab movement
    if (!slot->initialized || slab_moved) {
        if (filter->clear_pipelines[cls]) {
            storage_clear_push pc = {
                .output_handle = flux_image_bindless_storage_handle(slot->output),
                .width = slot->width,
                .height = slot->height,
                .origin_x = 0,
                .origin_y = 0,
                .region_width = slot->width,
                .region_height = slot->height,
            };
            uint32_t gx = (slot->width + PRISM_WG - 1u) / PRISM_WG;
            uint32_t gy = (slot->height + PRISM_WG - 1u) / PRISM_WG;
            flux_compute_dispatch(cmd, filter->clear_pipelines[cls], &pc, sizeof(pc), gx, gy, 1);
            barrier_compute_write_to_read_write(cmd, out_vk);
        }
    } else if (filter->clear_pipelines[cls]) {
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
            uint32_t gx = (pc.region_width + PRISM_WG - 1u) / PRISM_WG;
            uint32_t gy = (pc.region_height + PRISM_WG - 1u) / PRISM_WG;
            flux_compute_dispatch(cmd, filter->clear_pipelines[cls], &pc, sizeof(pc), gx, gy, 1);
        }
        for (uint32_t i = 0; i < current_count; ++i) {
            storage_clear_push pc = {
                .output_handle = flux_image_bindless_storage_handle(slot->output),
                .width = slot->width,
                .height = slot->height,
                .origin_x = current_regions[i].x,
                .origin_y = current_regions[i].y,
                .region_width = current_regions[i].width,
                .region_height = current_regions[i].height,
            };
            uint32_t gx = (pc.region_width + PRISM_WG - 1u) / PRISM_WG;
            uint32_t gy = (pc.region_height + PRISM_WG - 1u) / PRISM_WG;
            flux_compute_dispatch(cmd, filter->clear_pipelines[cls], &pc, sizeof(pc), gx, gy, 1);
        }
        if ((slot->previous_count > 0 || current_count > 0) && desc->group_count > 0)
            barrier_compute_write_to_read_write(cmd, out_vk);
    }

    // Render acrylic bodies
    if (desc->group_count > 0 && filter->acrylic_pipelines[cls]) {
        for (uint32_t i = 0; i < desc->group_count; ++i) {
            const prism_acrylic_group *g = &desc->groups[i];
            acrylic_region reg;
            if (!acrylic_group_dispatch_bounds(g, capture_width, capture_height, &reg))
                continue;

            acrylic_push pc = {
                .input_handle = flux_image_bindless_handle(desc->input),
                .blurred_handle = flux_image_bindless_handle(desc->blurred_input),
                .sampler_handle = flux_device_default_sampler_handle(filter->device),
                .output_handle = flux_image_bindless_storage_handle(slot->output),
                .width = slot->width,
                .height = slot->height,
                .origin_x = reg.x - slab_x,
                .origin_y = reg.y - slab_y,
                .bounds = {g->shape.bounds.x, g->shape.bounds.y, g->shape.bounds.w,
                           g->shape.bounds.h},
                .corner_radius = g->shape.corner_radius,
                .opacity = g->opacity * desc->opacity,
                .tint_strength = g->tint_strength >= 0.0f ? g->tint_strength : desc->tint_strength,
                .luminance_plate =
                    g->luminance_plate >= 0.0f ? g->luminance_plate : desc->luminance_plate,
                .noise_intensity =
                    g->noise_intensity >= 0.0f ? g->noise_intensity : desc->noise_intensity,
                .border_width = g->border_width >= 0.0f ? g->border_width : desc->border_width,
                .border_alpha = g->border_alpha >= 0.0f ? g->border_alpha : desc->border_alpha,
                .shadow_alpha = g->shadow_alpha,
                .shadow_blur = g->shadow_blur,
                .shadow_offset_y = g->shadow_offset_y,
                .tint_color = g->tint_color,
                .group_width = reg.width,
                .group_height = reg.height,
                .slab_origin_x = (float)slot->slab_x,
                .slab_origin_y = (float)slot->slab_y,
                .capture_width = (float)slot->capture_width,
                .capture_height = (float)slot->capture_height,
            };

            uint32_t gx = (reg.width + PRISM_WG - 1) / PRISM_WG;
            uint32_t gy = (reg.height + PRISM_WG - 1) / PRISM_WG;
            flux_compute_dispatch(cmd, filter->acrylic_pipelines[cls], &pc, sizeof(pc), gx, gy, 1);

            if (i + 1 < desc->group_count)
                barrier_compute_write_to_read_write(cmd, out_vk);
        }
    }

    barrier_compute_write_to_read(cmd, out_vk);

    slot->initialized = true;
    slot->previous_count = current_count;
    memcpy(slot->previous, current_regions, current_count * sizeof(acrylic_region));

    *out = (prism_material_output){
        .image = slot->output,
        .origin_x = (float)slot->slab_x,
        .origin_y = (float)slot->slab_y,
        .width = slot->width,
        .height = slot->height,
    };
    FLUX_PROFILE_ZONE_END(z_acrylic);
    return FLUX_OK;
}
