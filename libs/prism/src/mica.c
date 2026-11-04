/*
 * prism/mica.c — mica foundation material implementation.
 *
 * Evaluates screen-anchored wallpaper sampling, luminosity plate balance,
 * tinting, procedural noise, border rim, drop shadow, and inactive state fallback.
 */

#include "prism_material_frame.h"
#include "prism_observe.h"
#include <prism/mica.h>

#include <flux/compute.h>
#include <flux/flux.h>
#include <flux/profile.h>
#include <flux/vulkan.h>
#include <vulkan/vulkan.h>

#include <assert.h>
#include <math.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#if defined(__has_embed)
#if __has_embed("mica.comp.spv") && __has_embed("mica_16f.comp.spv") &&                            \
    __has_embed("storage_clear.comp.spv") && __has_embed("storage_clear_16f.comp.spv")
#define PRISM_MICA_SHADERS_EMBED 1
#endif
#endif

#ifdef PRISM_MICA_SHADERS_EMBED
alignas(uint32_t) static const unsigned char mica_spv[] = {
#embed "mica.comp.spv"
};
alignas(uint32_t) static const unsigned char mica_16f_spv[] = {
#embed "mica_16f.comp.spv"
};
alignas(uint32_t) static const unsigned char storage_clear_spv[] = {
#embed "storage_clear.comp.spv"
};
alignas(uint32_t) static const unsigned char storage_clear_16f_spv[] = {
#embed "storage_clear_16f.comp.spv"
};
#else
#include "mica_16f_spv.h"
#include "mica_spv.h"
#include "storage_clear_16f_spv.h"
#include "storage_clear_spv.h"
#endif

#define MICA_MAX_GROUPS 64u

typedef struct mica_region {
    uint32_t x;
    uint32_t y;
    uint32_t width;
    uint32_t height;
} mica_region;

typedef struct mica_push {
    uint32_t wallpaper_handle;
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
    float tint_opacity;
    float luminosity_plate;
    float luminosity_opacity;
    float noise_intensity;
    float border_width;
    float border_alpha;
    float shadow_alpha;
    float shadow_blur;
    float shadow_offset_y;
    uint32_t tint_color;
    uint32_t group_width;
    uint32_t group_height;
    float screen_origin_x;
    float screen_origin_y;
    float screen_width;
    float screen_height;
    uint32_t fallback_color;
    float fallback_weight;
    uint32_t is_alt;
    float slab_origin_x;
    float slab_origin_y;
    float capture_width;
    float capture_height;
} mica_push;

typedef struct storage_clear_push {
    uint32_t output_handle;
    uint32_t width;
    uint32_t height;
    uint32_t origin_x;
    uint32_t origin_y;
    uint32_t region_width;
    uint32_t region_height;
} storage_clear_push;

static_assert(sizeof(mica_push) == 148, "mica_push size mismatch");
static_assert(sizeof(storage_clear_push) == 28, "storage_clear_push size mismatch");

#define PRISM_WG 16u

static flux_format prism_output_format(const flux_image *input) {
    return flux_image_format(input) == FLUX_FORMAT_RGBA16_SFLOAT ? FLUX_FORMAT_RGBA16_SFLOAT
                                                                 : FLUX_FORMAT_RGBA8_UNORM;
}

typedef struct mica_filter_slot {
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
    mica_region previous[MICA_MAX_GROUPS];
} mica_filter_slot;

struct prism_mica_filter {
    atomic_uint ref_count;
    flux_device *device;
    flux_compute_pipeline *mica_pipelines[2];
    flux_compute_pipeline *clear_pipelines[2];
    mica_filter_slot slots[FLUX_MAX_FRAMES_IN_FLIGHT];
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
        .srcAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT,
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

flux_result prism_mica_filter_create(flux_device *device, prism_mica_filter **out) {
    if (!device || !out)
        return FLUX_ERROR_INVALID_ARGUMENT;
    *out = nullptr;
    /* Caller's allocator, attributed to prism (ADR-0135). */
    prism_mica_filter *filter = nullptr;
    if (flux_device_alloc(device,
                          &(flux_device_alloc_opts){
                              .type = FLUX_TYPE_DEVICE_ALLOC_OPTS,
                              .bytes = sizeof(*filter),
                              .owner = PRISM_OBSERVE_OWNER,
                              .kind = PRISM_OBSERVE_KIND_MICA,
                              .scope = FLUX_SCOPE_DEVICE,
                          },
                          (void **)&filter) != FLUX_OK)
        return FLUX_ERROR_OUT_OF_MEMORY;
    atomic_init(&filter->ref_count, 1u);
    filter->device = flux_device_retain(device);
    *out = filter;
    return FLUX_OK;
}

prism_mica_filter *prism_mica_filter_retain(prism_mica_filter *filter) {
    if (filter)
        atomic_fetch_add_explicit(&filter->ref_count, 1u, memory_order_relaxed);
    return filter;
}

void prism_mica_filter_release(prism_mica_filter *filter) {
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
        if (filter->mica_pipelines[i])
            flux_compute_pipeline_release(filter->mica_pipelines[i]);
        if (filter->clear_pipelines[i])
            flux_compute_pipeline_release(filter->clear_pipelines[i]);
    }
    flux_device_free(device, &(flux_device_free_opts){
                                 .type = FLUX_TYPE_DEVICE_FREE_OPTS,
                                 .ptr = filter,
                                 .owner = PRISM_OBSERVE_OWNER,
                                 .kind = PRISM_OBSERVE_KIND_MICA,
                                 .scope = FLUX_SCOPE_DEVICE,
                             });
    flux_device_release(device);
}

static flux_result mica_ensure_slot(prism_mica_filter *filter, uint32_t index,
                                    const flux_image *input, uint32_t footprint_w,
                                    uint32_t footprint_h) {
    mica_filter_slot *slot = &filter->slots[index];
    uint32_t width = 0u;
    uint32_t height = 0u;
    prism_slab_for_footprint(footprint_w, footprint_h, &width, &height);
    flux_format format = prism_output_format(input);
    if (slot->output && slot->width == width && slot->height == height && slot->format == format)
        return FLUX_OK;
    if (slot->output)
        flux_image_release(slot->output);
    *slot = (mica_filter_slot){0};
    flux_result r =
        flux_image_create_compute_writable(filter->device, width, height, format, &slot->output);
    if (r != FLUX_OK)
        return r;
    slot->width = width;
    slot->height = height;
    slot->format = format;
    return FLUX_OK;
}

static flux_result mica_ensure_pipelines(prism_mica_filter *filter, bool is16f, bool need_clear,
                                         bool need_mica) {
    const int cls = is16f ? 1 : 0;
    const bool missing = (need_clear && !filter->clear_pipelines[cls]) ||
                         (need_mica && !filter->mica_pipelines[cls]);
    if (!missing)
        return FLUX_OK;

    FLUX_PROFILE_ZONE(z_compile, "prism.mica.compile_pipelines");

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
    if (need_mica && !filter->mica_pipelines[cls]) {
        flux_compute_pipeline_desc pdesc = FLUX_COMPUTE_PIPELINE_DESC_INIT;
        pdesc.spirv = (const uint32_t *)(is16f ? mica_16f_spv : mica_spv);
        pdesc.spirv_word_count =
            (is16f ? sizeof(mica_16f_spv) : sizeof(mica_spv)) / sizeof(uint32_t);
        pdesc.entry_point = "main";
        pdesc.push_constant_bytes = sizeof(mica_push);
        flux_result r =
            flux_compute_pipeline_create(filter->device, &pdesc, &filter->mica_pipelines[cls]);
        if (r != FLUX_OK) {
            FLUX_PROFILE_ZONE_END(z_compile);
            return r;
        }
    }
    FLUX_PROFILE_ZONE_END(z_compile);
    return FLUX_OK;
}

flux_result prism_mica_filter_warmup(prism_mica_filter *filter,
                                     const prism_mica_warmup_opts *opts) {
    if (!filter)
        return FLUX_ERROR_INVALID_ARGUMENT;

    bool is16f = false;
    if (opts) {
        if (opts->format == FLUX_FORMAT_RGBA16_SFLOAT)
            is16f = true;
    }

    return mica_ensure_pipelines(filter, is16f, true, true);
}

static bool mica_group_dispatch_bounds(const prism_mica_group *group, uint32_t image_width,
                                       uint32_t image_height, mica_region *out) {
    if (!group || !out || image_width == 0 || image_height == 0)
        return false;
    float pad = fmaxf(group->shadow_blur * 2.0f, 2.0f);
    int64_t x0 = (int64_t)floorf(group->shape.bounds.x - pad);
    int64_t y0 = (int64_t)floorf(group->shape.bounds.y - pad);
    if (group->shadow_offset_y < 0.0f)
        y0 += (int64_t)floorf(group->shadow_offset_y);
    int64_t x1 = (int64_t)ceilf(group->shape.bounds.x + group->shape.bounds.w + pad);
    int64_t y1 = (int64_t)ceilf(group->shape.bounds.y + group->shape.bounds.h + pad);
    if (group->shadow_offset_y > 0.0f)
        y1 += (int64_t)ceilf(group->shadow_offset_y);

    if (x1 <= 0 || y1 <= 0 || x0 >= (int64_t)image_width || y0 >= (int64_t)image_height)
        return false;

    uint32_t clamped_x0 = (uint32_t)fmaxf((float)x0, 0.0f);
    uint32_t clamped_y0 = (uint32_t)fmaxf((float)y0, 0.0f);
    uint32_t clamped_x1 = (uint32_t)fminf((float)x1, (float)image_width);
    uint32_t clamped_y1 = (uint32_t)fminf((float)y1, (float)image_height);

    if (clamped_x1 <= clamped_x0 || clamped_y1 <= clamped_y0)
        return false;

    out->x = clamped_x0;
    out->y = clamped_y0;
    out->width = clamped_x1 - clamped_x0;
    out->height = clamped_y1 - clamped_y0;
    return true;
}

/* Everything one mica submission needs to replay its dispatches later
 * (ADR-0147). The per-group push constants are captured as prepared, so the
 * replay performs no per-group policy resolution and cannot disagree with what
 * was already decided here. */
typedef struct mica_plan {
    flux_device *device;
    struct prism_mica_filter *filter;
    flux_image *wallpaper;
    flux_image *blurred_wallpaper;
    flux_image *output;
    uint32_t output_width;
    uint32_t output_height;
    bool is16f;
    bool clear_full;
    uint32_t slab_x;
    uint32_t slab_y;
    mica_region current[MICA_MAX_GROUPS];
    uint32_t current_count;
    mica_region previous[MICA_MAX_GROUPS];
    uint32_t previous_count;
    mica_push pushes[MICA_MAX_GROUPS];
    uint32_t push_count;
} mica_plan;

static void mica_plan_record(void *raw, VkCommandBuffer cmd) {
    const mica_plan *p = raw;
    if (!p || cmd == VK_NULL_HANDLE || !p->filter)
        return;
    prism_mica_filter *filter = p->filter;
    const int cls = p->is16f ? 1 : 0;
    VkImage out_vk = flux_image_vk_image(p->output);

    barrier_reuse_to_compute_write(cmd, out_vk);

    if (p->clear_full) {
        storage_clear_push pc = {
            .output_handle = flux_image_bindless_storage_handle(p->output),
            .width = p->output_width,
            .height = p->output_height,
            .origin_x = 0,
            .origin_y = 0,
            .region_width = p->output_width,
            .region_height = p->output_height,
        };
        uint32_t gx = (pc.region_width + PRISM_WG - 1u) / PRISM_WG;
        uint32_t gy = (pc.region_height + PRISM_WG - 1u) / PRISM_WG;
        flux_compute_dispatch(cmd, filter->clear_pipelines[cls], &pc, sizeof(pc), gx, gy, 1);
        barrier_compute_write_to_read_write(cmd, out_vk);
    } else {
        for (uint32_t i = 0; i < p->previous_count; ++i) {
            const mica_region *prev = &p->previous[i];
            storage_clear_push pc = {
                .output_handle = flux_image_bindless_storage_handle(p->output),
                .width = p->output_width,
                .height = p->output_height,
                .origin_x = prev->x,
                .origin_y = prev->y,
                .region_width = prev->width,
                .region_height = prev->height,
            };
            uint32_t gx = (prev->width + PRISM_WG - 1u) / PRISM_WG;
            uint32_t gy = (prev->height + PRISM_WG - 1u) / PRISM_WG;
            flux_compute_dispatch(cmd, filter->clear_pipelines[cls], &pc, sizeof(pc), gx, gy, 1);
            barrier_compute_write_to_read_write(cmd, out_vk);
        }
        for (uint32_t i = 0; i < p->current_count; ++i) {
            const mica_region *cur = &p->current[i];
            storage_clear_push pc = {
                .output_handle = flux_image_bindless_storage_handle(p->output),
                .width = p->output_width,
                .height = p->output_height,
                .origin_x = cur->x,
                .origin_y = cur->y,
                .region_width = cur->width,
                .region_height = cur->height,
            };
            uint32_t gx = (cur->width + PRISM_WG - 1u) / PRISM_WG;
            uint32_t gy = (cur->height + PRISM_WG - 1u) / PRISM_WG;
            flux_compute_dispatch(cmd, filter->clear_pipelines[cls], &pc, sizeof(pc), gx, gy, 1);
            barrier_compute_write_to_read_write(cmd, out_vk);
        }
    }

    for (uint32_t i = 0; i < p->push_count; ++i) {
        const mica_region *bounds = &p->current[i];
        uint32_t gx = (bounds->width + PRISM_WG - 1u) / PRISM_WG;
        uint32_t gy = (bounds->height + PRISM_WG - 1u) / PRISM_WG;
        flux_compute_dispatch(cmd, filter->mica_pipelines[cls], &p->pushes[i], sizeof(mica_push),
                              gx, gy, 1);
        if (i + 1 < p->push_count)
            barrier_compute_write_to_read_write(cmd, out_vk);
    }

    barrier_compute_write_to_read(cmd, out_vk);
}

flux_result prism_mica_filter_apply(prism_mica_filter *filter, flux_frame *frame,
                                    const prism_mica_desc *desc, prism_material_output *out) {
    if (!filter || !frame || !desc || !out)
        return FLUX_ERROR_INVALID_ARGUMENT;
    if (desc->type != PRISM_TYPE_MICA_DESC)
        return FLUX_ERROR_INVALID_ARGUMENT;
    if (!desc->wallpaper && !desc->blurred_wallpaper)
        return FLUX_ERROR_INVALID_ARGUMENT;
    if (desc->group_count > MICA_MAX_GROUPS)
        return FLUX_ERROR_INVALID_ARGUMENT;
    if (desc->group_count > 0 && !desc->groups)
        return FLUX_ERROR_INVALID_ARGUMENT;

    *out = (prism_material_output){0};
    if (flux_frame_get_state(frame) != FLUX_FRAME_STATE_RECORDING ||
        flux_frame_has_active_pass(frame))
        return FLUX_ERROR_INVALID_STATE;

    /* ADR-0147: `blurred_wallpaper` is normally a deferred flux_blur_filter
     * output; materialize it so its dispatches precede ours. */
    if (desc->blurred_wallpaper) {
        flux_result mat = flux_frame_materialize_image(frame, desc->blurred_wallpaper);
        if (mat != FLUX_OK)
            return mat;
    }

    const flux_image *ref_input =
        desc->blurred_wallpaper ? desc->blurred_wallpaper : desc->wallpaper;
    const uint32_t capture_width = flux_image_width(ref_input);
    const uint32_t capture_height = flux_image_height(ref_input);
    if (capture_width == 0 || capture_height == 0)
        return FLUX_ERROR_INVALID_ARGUMENT;

    FLUX_PROFILE_ZONE(z_mica, "prism.mica.apply");

    uint32_t current_count = 0;
    mica_region current_regions[MICA_MAX_GROUPS];
    uint32_t footprint_x0 = UINT32_MAX, footprint_y0 = UINT32_MAX;
    uint32_t footprint_x1 = 0u, footprint_y1 = 0u;

    for (uint32_t i = 0; i < desc->group_count; ++i) {
        mica_region bounds;
        if (mica_group_dispatch_bounds(&desc->groups[i], capture_width, capture_height, &bounds)) {
            current_regions[current_count++] = bounds;
            if (bounds.x < footprint_x0)
                footprint_x0 = bounds.x;
            if (bounds.y < footprint_y0)
                footprint_y0 = bounds.y;
            if (bounds.x + bounds.width > footprint_x1)
                footprint_x1 = bounds.x + bounds.width;
            if (bounds.y + bounds.height > footprint_y1)
                footprint_y1 = bounds.y + bounds.height;
        }
    }

    uint32_t footprint_w = 1u;
    uint32_t footprint_h = 1u;
    if (current_count > 0u) {
        footprint_w = footprint_x1 - footprint_x0;
        footprint_h = footprint_y1 - footprint_y0;
    }

    uint32_t slot_idx = flux_frame_index(frame);
    flux_result r = mica_ensure_slot(filter, slot_idx, ref_input, footprint_w, footprint_h);
    if (r != FLUX_OK)
        return r;

    mica_filter_slot *slot = &filter->slots[slot_idx];

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

    bool is16f = (slot->format == FLUX_FORMAT_RGBA16_SFLOAT);

    r = mica_ensure_pipelines(filter, is16f, slot->initialized || desc->group_count > 0,
                              desc->group_count > 0);
    if (r != FLUX_OK)
        return r;

    mica_plan plan = {
        .device = filter->device,
        .filter = filter,
        .wallpaper = desc->wallpaper,
        .blurred_wallpaper = desc->blurred_wallpaper,
        .output = slot->output,
        .output_width = slot->width,
        .output_height = slot->height,
        .is16f = is16f,
        .clear_full = !slot->initialized || slab_moved,
        .slab_x = slab_x,
        .slab_y = slab_y,
        .current_count = current_count,
        .previous_count = (!slot->initialized || slab_moved) ? 0u : slot->previous_count,
    };
    memcpy(plan.current, current_regions, current_count * sizeof(mica_region));
    if (plan.previous_count > 0u)
        memcpy(plan.previous, slot->previous, plan.previous_count * sizeof(mica_region));

    uint32_t wp_handle =
        desc->wallpaper ? flux_image_bindless_handle(desc->wallpaper) : 0xFFFFFFFFu;
    uint32_t bl_handle =
        desc->blurred_wallpaper ? flux_image_bindless_handle(desc->blurred_wallpaper) : 0xFFFFFFFFu;
    uint32_t samp_handle = flux_device_default_sampler_handle(filter->device);
    uint32_t out_handle = flux_image_bindless_storage_handle(slot->output);

    for (uint32_t i = 0; i < current_count; ++i) {
        const prism_mica_group *g = &desc->groups[i];
        const mica_region *bounds = &plan.current[i];

        float eff_tint_opacity = (g->tint_opacity >= 0.0f) ? g->tint_opacity : desc->tint_opacity;
        float eff_lum_plate =
            (g->luminosity_plate >= 0.0f) ? g->luminosity_plate : desc->luminosity_plate;
        float eff_lum_opacity =
            (g->luminosity_opacity >= 0.0f) ? g->luminosity_opacity : desc->luminosity_opacity;
        float eff_noise = (g->noise_intensity >= 0.0f) ? g->noise_intensity : desc->noise_intensity;
        float eff_bwidth = (g->border_width >= 0.0f) ? g->border_width : desc->border_width;
        float eff_balpha = (g->border_alpha >= 0.0f) ? g->border_alpha : desc->border_alpha;

        plan.pushes[plan.push_count++] = (mica_push){
            .wallpaper_handle = wp_handle,
            .blurred_handle = bl_handle,
            .sampler_handle = samp_handle,
            .output_handle = out_handle,
            .width = slot->width,
            .height = slot->height,
            .origin_x = bounds->x,
            .origin_y = bounds->y,
            .bounds = {g->shape.bounds.x, g->shape.bounds.y, g->shape.bounds.w, g->shape.bounds.h},
            .corner_radius = g->shape.corner_radius,
            .opacity = g->opacity * desc->opacity,
            .tint_opacity = eff_tint_opacity,
            .luminosity_plate = eff_lum_plate,
            .luminosity_opacity = eff_lum_opacity,
            .noise_intensity = eff_noise,
            .border_width = eff_bwidth,
            .border_alpha = eff_balpha,
            .shadow_alpha = g->shadow_alpha,
            .shadow_blur = g->shadow_blur,
            .shadow_offset_y = g->shadow_offset_y,
            .tint_color = g->tint_color,
            .group_width = bounds->width,
            .group_height = bounds->height,
            .screen_origin_x = desc->screen_origin_x,
            .screen_origin_y = desc->screen_origin_y,
            .screen_width = desc->screen_width,
            .screen_height = desc->screen_height,
            .fallback_color = desc->fallback_color,
            .fallback_weight = desc->fallback_weight,
            .is_alt = (desc->kind == PRISM_MICA_ALT) ? 1u : 0u,
            .slab_origin_x = (float)slab_x,
            .slab_origin_y = (float)slab_y,
            .capture_width = (float)capture_width,
            .capture_height = (float)capture_height,
        };
    }

    slot->initialized = true;
    slot->previous_count = current_count;
    memcpy(slot->previous, current_regions, current_count * sizeof(mica_region));

    *out = (prism_material_output){
        .image = slot->output,
        .origin_x = (float)slot->slab_x,
        .origin_y = (float)slot->slab_y,
        .width = slot->width,
        .height = slot->height,
    };

    /* Defer the dispatches (ADR-0147). The handle above is valid now; the work
     * replays only if something requires the output. mica reads the wallpaper as
     * an input so the plan orders it after any wallpaper-producing pass. */
    prism_deferred_pass pass = PRISM_DEFERRED_PASS_INIT;
    pass.output = slot->output;
    pass.label = "prism.mica";
    if (desc->wallpaper) {
        pass.inputs[pass.input_count] = desc->wallpaper;
        pass.input_count++;
    }
    if (desc->blurred_wallpaper && desc->blurred_wallpaper != desc->wallpaper) {
        pass.inputs[pass.input_count] = desc->blurred_wallpaper;
        pass.input_count++;
    }
    flux_deferred_token *token = nullptr;
    flux_result reg =
        prism_deferred_register(frame, &pass, mica_plan_record, &plan, sizeof(plan), &token);
    if (reg != FLUX_OK) {
        mica_plan_record(&plan, flux_frame_vk_command_buffer(frame));
        token = nullptr;
    }
    (void)prism_deferred_require(frame, token);
    FLUX_PROFILE_ZONE_END(z_mica);
    return FLUX_OK;
}
