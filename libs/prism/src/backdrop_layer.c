/*
 * Layered backdrop compositor — frost sheet(s) carrying analytic
 * liquid-glass bodies, composed in one ordered dispatch sequence.
 *
 * See prism/backdrop_layer.h for the material contract. Implementation
 * layout mirrors liquid_glass.c: per-slot persistent transparent output,
 * footprint clearing across submissions, per-layer barriers, and glass
 * statistics reduced per group. The glass pass reuses the exact reference
 * recipe through glass_dispatch.h, so a layered body renders identically
 * to a standalone one — only its *sampled backdrop* differs (the frosted
 * layer image instead of the sharp capture).
 */

#include <prism/backdrop_layer.h>

#include <flux/compute.h>
#include <flux/vulkan.h>

#include "glass_dispatch.h"
#include "regions.h"

#include <math.h>
#include <stdalign.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

/* Shader bytecode: C23 #embed, or generated headers (tools/spv2h.py). */
#if defined(__has_embed) && !defined(PRISM_SHADER_NO_EMBED)
#if __has_embed("liquid_glass.comp.spv") && __has_embed("storage_clear.comp.spv") &&               \
    __has_embed("backdrop_stats.comp.spv") && __has_embed("backdrop_frost.comp.spv") &&           \
    __has_embed("liquid_glass_16f.comp.spv") && __has_embed("storage_clear_16f.comp.spv") &&      \
    __has_embed("backdrop_frost_16f.comp.spv")
#define PRISM_LAYER_SHADERS_EMBED 1
#endif
#endif

#ifdef PRISM_LAYER_SHADERS_EMBED
alignas(uint32_t) static const unsigned char liquid_glass_spv[] = {
#embed "liquid_glass.comp.spv"
};
alignas(uint32_t) static const unsigned char storage_clear_spv[] = {
#embed "storage_clear.comp.spv"
};
alignas(uint32_t) static const unsigned char backdrop_stats_spv[] = {
#embed "backdrop_stats.comp.spv"
};
alignas(uint32_t) static const unsigned char backdrop_frost_spv[] = {
#embed "backdrop_frost.comp.spv"
};
alignas(uint32_t) static const unsigned char liquid_glass_16f_spv[] = {
#embed "liquid_glass_16f.comp.spv"
};
alignas(uint32_t) static const unsigned char storage_clear_16f_spv[] = {
#embed "storage_clear_16f.comp.spv"
};
alignas(uint32_t) static const unsigned char backdrop_frost_16f_spv[] = {
#embed "backdrop_frost_16f.comp.spv"
};
#else
#include "backdrop_frost_16f_spv.h"
#include "backdrop_frost_spv.h"
#include "backdrop_stats_spv.h"
#include "liquid_glass_16f_spv.h"
#include "liquid_glass_spv.h"
#include "storage_clear_16f_spv.h"
#include "storage_clear_spv.h"
#endif

/* Push-constant block — must match storage_clear.comp exactly. */
typedef struct storage_clear_push {
    uint32_t output_handle;
    uint32_t width;
    uint32_t height;
    uint32_t origin_x;
    uint32_t origin_y;
    uint32_t region_width;
    uint32_t region_height;
} storage_clear_push;

/* Push-constant block — must match backdrop_stats.comp exactly. */
typedef struct backdrop_stats_push {
    uint32_t input_handle;
    uint32_t blurred_handle;
    uint32_t sampler_handle;
    uint32_t width;
    uint32_t height;
    uint32_t rect_x;
    uint32_t rect_y;
    uint32_t rect_width;
    uint32_t rect_height;
    uint32_t group_index;
    uint32_t stats_address_lo;
    uint32_t stats_address_hi;
} backdrop_stats_push;

/* Push-constant block — must match backdrop_frost.comp exactly. Eight
 * uint scalars lead the block so std430's 16-byte alignment places the
 * bounds vec4 at byte 32 on both sides. */
typedef struct backdrop_frost_push {
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
    uint32_t tint_color;
    float tint_strength;
    uint32_t region_width;
    uint32_t region_height;
} backdrop_frost_push;

static_assert(sizeof(storage_clear_push) == 28,
              "storage_clear_push no longer matches its shader block");
static_assert(sizeof(backdrop_stats_push) == 48,
              "backdrop_stats_push no longer matches its shader block");
static_assert(sizeof(backdrop_frost_push) == 72,
              "backdrop_frost_push no longer matches its shader block");

#define PRISM_STATS_BYTES_LAYER (PRISM_BACKDROP_MAX_GLASS_GROUPS * 2u * sizeof(float))

static flux_format prism_layer_output_format(const flux_image *input) {
    return flux_image_format(input) == FLUX_FORMAT_RGBA16_SFLOAT ? FLUX_FORMAT_RGBA16_SFLOAT
                                                                 : FLUX_FORMAT_RGBA8_UNORM;
}

typedef struct backdrop_layer_slot {
    uint32_t width;
    uint32_t height;
    flux_format format;
    flux_image *output;
    bool initialized;
    /* Clear footprints: previous and current frost rects + glass bodies. */
    uint32_t previous_count;
    liquid_glass_region previous[PRISM_BACKDROP_MAX_FROST_RECTS + PRISM_BACKDROP_MAX_GLASS_GROUPS];
    flux_buffer *stats;
    bool stats_submitted;
    uint32_t stats_group_count;
} backdrop_layer_slot;

struct prism_backdrop_layer_filter {
    atomic_uint ref_count;
    flux_device *device; /* retained; slot images hold weak device refs */
    flux_compute_pipeline *glass_pipelines[2];  /* [0] rgba8, [1] rgba16f */
    flux_compute_pipeline *clear_pipelines[2];
    flux_compute_pipeline *frost_pipelines[2];
    flux_compute_pipeline *stats_pipeline;
    backdrop_layer_slot slots[FLUX_MAX_FRAMES_IN_FLIGHT];
};

/* ------------------------------------------------------------------ */
/*  Barrier helpers (identical semantics to liquid_glass.c)           */
/* ------------------------------------------------------------------ */

static void layer_barrier_compute_write_to_read(VkCommandBuffer cmd, VkImage image) {
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

static void layer_barrier_reuse_to_compute_write(VkCommandBuffer cmd, VkImage image) {
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

static void layer_barrier_compute_write_to_read_write(VkCommandBuffer cmd, VkImage image) {
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

/* The layered glass pass is unique in prism: its sampled *input* is the same
 * image as its storage output (the frosted layer it composites into). Make
 * the frost writes visible to both access classes — texture() fetches
 * (SAMPLED_READ) and imageLoad/imageStore (STORAGE_READ | WRITE) — before
 * the first glass dispatch. The regular write→read-write barrier above does
 * not cover sampled reads; the standalone liquid-glass filter never needs
 * this because its input is a different image. */
static void layer_barrier_compute_write_to_sampled_and_storage(VkCommandBuffer cmd,
                                                               VkImage image) {
    VkImageMemoryBarrier2 barrier = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        .srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        .dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT |
                         VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
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

static void layer_barrier_stats_write_to_compute(VkCommandBuffer cmd, VkBuffer buffer) {
    VkBufferMemoryBarrier2 b = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2,
        .srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        .srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        .dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
        .buffer = buffer,
        .size = VK_WHOLE_SIZE,
    };
    VkDependencyInfo di = {
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .bufferMemoryBarrierCount = 1,
        .pBufferMemoryBarriers = &b,
    };
    vkCmdPipelineBarrier2(cmd, &di);
}

/* ------------------------------------------------------------------ */
/*  Slot / pipeline lifecycle                                        */
/* ------------------------------------------------------------------ */

static flux_result layer_ensure_stats_buffer(prism_backdrop_layer_filter *filter,
                                             backdrop_layer_slot *slot) {
    if (slot->stats)
        return FLUX_OK;
    flux_buffer_desc bdesc = FLUX_BUFFER_DESC_INIT;
    bdesc.size = PRISM_STATS_BYTES_LAYER;
    bdesc.usage = FLUX_BUFFER_USAGE_STORAGE;
    bdesc.location = FLUX_BUFFER_HOST_VISIBLE;
    bdesc.device_address = true;
    return flux_buffer_create(filter->device, &bdesc, &slot->stats);
}

static flux_result
layer_ensure_slot(prism_backdrop_layer_filter *filter, uint32_t index, const flux_image *input,
                  bool need_stats) {
    backdrop_layer_slot *slot = &filter->slots[index];
    uint32_t width = flux_image_width(input);
    uint32_t height = flux_image_height(input);
    flux_format format = prism_layer_output_format(input);
    if (slot->output && slot->width == width && slot->height == height && slot->format == format)
        return need_stats ? layer_ensure_stats_buffer(filter, slot) : FLUX_OK;
    if (slot->output)
        flux_image_release(slot->output);
    if (slot->stats)
        flux_buffer_release(slot->stats);
    *slot = (backdrop_layer_slot){0};
    flux_result r =
        flux_image_create_compute_writable(filter->device, width, height, format, &slot->output);
    if (r != FLUX_OK)
        return r;
    slot->width = width;
    slot->height = height;
    slot->format = format;
    if (need_stats) {
        r = layer_ensure_stats_buffer(filter, slot);
        if (r != FLUX_OK)
            return r;
    }
    return FLUX_OK;
}

static flux_result layer_ensure_pipelines(prism_backdrop_layer_filter *filter, bool is16f,
                                          bool need_clear, bool need_frost, bool need_glass,
                                          bool need_stats) {
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
    if (need_frost && !filter->frost_pipelines[cls]) {
        flux_compute_pipeline_desc pdesc = FLUX_COMPUTE_PIPELINE_DESC_INIT;
        pdesc.spirv = (const uint32_t *)(is16f ? backdrop_frost_16f_spv : backdrop_frost_spv);
        pdesc.spirv_word_count = (is16f ? sizeof(backdrop_frost_16f_spv) : sizeof(backdrop_frost_spv)) /
                                 sizeof(uint32_t);
        pdesc.entry_point = "main";
        pdesc.push_constant_bytes = sizeof(backdrop_frost_push);
        flux_result r =
            flux_compute_pipeline_create(filter->device, &pdesc, &filter->frost_pipelines[cls]);
        if (r != FLUX_OK)
            return r;
    }
    if (need_glass && !filter->glass_pipelines[cls]) {
        flux_compute_pipeline_desc pdesc = FLUX_COMPUTE_PIPELINE_DESC_INIT;
        pdesc.spirv = (const uint32_t *)(is16f ? liquid_glass_16f_spv : liquid_glass_spv);
        pdesc.spirv_word_count =
            (is16f ? sizeof(liquid_glass_16f_spv) : sizeof(liquid_glass_spv)) / sizeof(uint32_t);
        pdesc.entry_point = "main";
        pdesc.push_constant_bytes = sizeof(liquid_glass_push);
        flux_result r =
            flux_compute_pipeline_create(filter->device, &pdesc, &filter->glass_pipelines[cls]);
        if (r != FLUX_OK)
            return r;
    }
    if (need_stats && !filter->stats_pipeline) {
        flux_compute_pipeline_desc pdesc = FLUX_COMPUTE_PIPELINE_DESC_INIT;
        pdesc.spirv = (const uint32_t *)backdrop_stats_spv;
        pdesc.spirv_word_count = sizeof(backdrop_stats_spv) / sizeof(uint32_t);
        pdesc.entry_point = "main";
        pdesc.push_constant_bytes = sizeof(backdrop_stats_push);
        flux_result r =
            flux_compute_pipeline_create(filter->device, &pdesc, &filter->stats_pipeline);
        if (r != FLUX_OK)
            return r;
    }
    return FLUX_OK;
}

/* ------------------------------------------------------------------ */
/*  Validation and dispatch footprints                               */
/* ------------------------------------------------------------------ */

static bool valid_backdrop_layer_desc(const prism_backdrop_layer_desc *desc) {
    if (desc->type != PRISM_TYPE_BACKDROP_LAYER_DESC || !desc->input || !desc->blurred_input)
        return false;
    if (desc->frost_count > PRISM_BACKDROP_MAX_FROST_RECTS ||
        (desc->frost_count > 0u && !desc->frost))
        return false;
    if (desc->group_count > PRISM_BACKDROP_MAX_GLASS_GROUPS ||
        (desc->group_count > 0u && !desc->groups))
        return false;
    if (!isfinite(desc->refraction) || !isfinite(desc->chromatic_aberration) ||
        !isfinite(desc->saturation) || !isfinite(desc->brightness) || !isfinite(desc->edge_width) ||
        !isfinite(desc->rim_light) || !isfinite(desc->light_direction.x) ||
        !isfinite(desc->light_direction.y) || !isfinite(desc->opacity) ||
        !isfinite(desc->size_reference) || !isfinite(desc->size_scale_min) ||
        !isfinite(desc->tint_strength) || !isfinite(desc->frost_strength))
        return false;
    for (uint32_t i = 0; i < desc->frost_count; ++i) {
        const prism_backdrop_frost *frost = &desc->frost[i];
        if (!isfinite(frost->bounds.x) || !isfinite(frost->bounds.y) ||
            !isfinite(frost->bounds.w) || !isfinite(frost->bounds.h) ||
            !isfinite(frost->corner_radius) || !isfinite(frost->opacity) ||
            !isfinite(frost->tint_strength) ||
            frost->bounds.w <= 0.0f || frost->bounds.h <= 0.0f)
            return false;
    }
    for (uint32_t i = 0; i < desc->group_count; ++i) {
        if (!liquid_glass_group_is_valid(&desc->groups[i]))
            return false;
    }
    return true;
}

static bool backdrop_frost_dispatch_bounds(const prism_backdrop_frost *frost,
                                           uint32_t image_width, uint32_t image_height,
                                           liquid_glass_region *out) {
    if (!frost || !out || image_width == 0u || image_height == 0u)
        return false;
    int64_t x0 = (int64_t)floorf(frost->bounds.x);
    int64_t y0 = (int64_t)floorf(frost->bounds.y);
    int64_t x1 = (int64_t)ceilf(frost->bounds.x + frost->bounds.w);
    int64_t y1 = (int64_t)ceilf(frost->bounds.y + frost->bounds.h);
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

/* ------------------------------------------------------------------ */
/*  Public API                                                       */
/* ------------------------------------------------------------------ */

flux_result prism_backdrop_layer_filter_create(flux_device *device,
                                               prism_backdrop_layer_filter **out) {
    if (!device || !out)
        return FLUX_ERROR_INVALID_ARGUMENT;
    *out = nullptr;
    prism_backdrop_layer_filter *filter = calloc(1, sizeof(*filter));
    if (!filter)
        return FLUX_ERROR_OUT_OF_MEMORY;
    atomic_init(&filter->ref_count, 1u);
    filter->device = flux_device_retain(device);
    *out = filter;
    return FLUX_OK;
}

prism_backdrop_layer_filter *
prism_backdrop_layer_filter_retain(prism_backdrop_layer_filter *filter) {
    if (filter)
        atomic_fetch_add_explicit(&filter->ref_count, 1u, memory_order_relaxed);
    return filter;
}

void prism_backdrop_layer_filter_release(prism_backdrop_layer_filter *filter) {
    if (!filter)
        return;
    if (atomic_fetch_sub_explicit(&filter->ref_count, 1u, memory_order_acq_rel) != 1u)
        return;
    flux_device *device = filter->device;
    for (uint32_t i = 0; i < FLUX_MAX_FRAMES_IN_FLIGHT; ++i) {
        if (filter->slots[i].output)
            flux_image_release(filter->slots[i].output);
        if (filter->slots[i].stats)
            flux_buffer_release(filter->slots[i].stats);
    }
    for (int i = 0; i < 2; ++i) {
        if (filter->glass_pipelines[i])
            flux_compute_pipeline_release(filter->glass_pipelines[i]);
        if (filter->clear_pipelines[i])
            flux_compute_pipeline_release(filter->clear_pipelines[i]);
        if (filter->frost_pipelines[i])
            flux_compute_pipeline_release(filter->frost_pipelines[i]);
    }
    if (filter->stats_pipeline)
        flux_compute_pipeline_release(filter->stats_pipeline);
    free(filter);
    flux_device_release(device);
}

flux_result prism_backdrop_layer_filter_apply(prism_backdrop_layer_filter *filter,
                                              flux_frame *frame,
                                              const prism_backdrop_layer_desc *desc,
                                              flux_image **out) {
    if (!filter || !frame || !desc || !out)
        return FLUX_ERROR_INVALID_ARGUMENT;
    if (!valid_backdrop_layer_desc(desc))
        return FLUX_ERROR_INVALID_ARGUMENT;
    if (flux_frame_get_state(frame) != FLUX_FRAME_STATE_RECORDING ||
        flux_frame_has_active_pass(frame))
        return FLUX_ERROR_INVALID_STATE;
    flux_image *input = desc->input;
    flux_image *blurred = desc->blurred_input;
    if (flux_frame_device(frame) != filter->device || flux_image_device(input) != filter->device ||
        flux_image_device(blurred) != filter->device)
        return FLUX_ERROR_INVALID_ARGUMENT;
    const bool is16f = flux_image_format(input) == FLUX_FORMAT_RGBA16_SFLOAT;
    const flux_format want_blur = is16f ? FLUX_FORMAT_RGBA16_SFLOAT : FLUX_FORMAT_RGBA8_UNORM;
    if (flux_image_width(input) != flux_image_width(blurred) ||
        flux_image_height(input) != flux_image_height(blurred) ||
        (flux_image_format(input) != FLUX_FORMAT_RGBA8_UNORM &&
         flux_image_format(input) != FLUX_FORMAT_BGRA8_UNORM && !is16f) ||
        flux_image_format(blurred) != want_blur ||
        flux_image_bindless_handle(input) == FLUX_BINDLESS_INVALID ||
        flux_image_bindless_handle(blurred) == FLUX_BINDLESS_INVALID)
        return FLUX_ERROR_UNSUPPORTED;
    uint32_t index = flux_frame_index(frame);
    if (index >= FLUX_MAX_FRAMES_IN_FLIGHT)
        return FLUX_ERROR_OUT_OF_RANGE;
    flux_result result =
        layer_ensure_slot(filter, index, input, desc->group_count > 0u);
    if (result != FLUX_OK)
        return result;

    uint32_t image_width = flux_image_width(input);
    uint32_t image_height = flux_image_height(input);
    backdrop_layer_slot *slot = &filter->slots[index];

    /* Current footprints: frost rects first, glass bodies after — the same
     * order the dispatches run in, so the merged clear set covers exactly
     * what this submission will write. */
    liquid_glass_region current[PRISM_BACKDROP_MAX_FROST_RECTS + PRISM_BACKDROP_MAX_GLASS_GROUPS];
    uint32_t current_count = 0u;
    for (uint32_t i = 0; i < desc->frost_count; ++i) {
        liquid_glass_region region;
        if (!backdrop_frost_dispatch_bounds(&desc->frost[i], image_width, image_height, &region))
            continue;
        current[current_count++] = region;
    }
    uint32_t current_group_indices[PRISM_BACKDROP_MAX_GLASS_GROUPS];
    uint32_t current_group_count = 0u;
    for (uint32_t i = 0; i < desc->group_count; ++i) {
        const prism_liquid_glass_group *group = &desc->groups[i];
        float shadow_reach = group->shadow_alpha > 0.0f
                                 ? fmaxf(group->shadow_offset_y, 0.0f) +
                                       2.0f * fmaxf(group->shadow_blur, 0.0f)
                                 : 0.0f;
        liquid_glass_region region;
        if (!liquid_glass_group_dispatch_bounds(group, shadow_reach, image_width, image_height,
                                                &region))
            continue;
        current[current_count++] = region;
        current_group_indices[current_group_count++] = i;
    }

    const uint32_t clear_capacity =
        PRISM_BACKDROP_MAX_FROST_RECTS + PRISM_BACKDROP_MAX_GLASS_GROUPS;
    liquid_glass_region clear_regions[clear_capacity];
    uint32_t clear_count = 0u;
    if (!liquid_glass_build_clear_regions(slot->initialized, image_width, image_height,
                                          slot->previous, slot->previous_count, current,
                                          current_count, clear_regions, clear_capacity,
                                          &clear_count))
        return FLUX_ERROR_INVALID_STATE;

    result = layer_ensure_pipelines(filter, is16f, clear_count > 0u, desc->frost_count > 0u,
                                    current_group_count > 0u, desc->group_count > 0u);
    if (result != FLUX_OK)
        return result;

    VkCommandBuffer command = flux_frame_vk_command_buffer(frame);
    VkImage output_image = flux_image_vk_image(slot->output);

    /* 1. Clear previous + current footprints to transparent. */
    if (clear_count > 0u) {
        if (slot->initialized)
            layer_barrier_reuse_to_compute_write(command, output_image);
        for (uint32_t i = 0; i < clear_count; ++i) {
            liquid_glass_region region = clear_regions[i];
            storage_clear_push push = {
                .output_handle = flux_image_bindless_storage_handle(slot->output),
                .width = image_width,
                .height = image_height,
                .origin_x = region.x,
                .origin_y = region.y,
                .region_width = region.width,
                .region_height = region.height,
            };
            uint32_t gx = (region.width + PRISM_GLASS_WG - 1u) / PRISM_GLASS_WG;
            uint32_t gy = (region.height + PRISM_GLASS_WG - 1u) / PRISM_GLASS_WG;
            flux_compute_dispatch(command, filter->clear_pipelines[is16f ? 1 : 0], &push,
                                  sizeof(push), gx, gy, 1u);
        }
        if (current_count > 0u)
            layer_barrier_compute_write_to_read_write(command, output_image);
    }

    /* 2. Glass statistics over the *layer* image (what the glass actually
     * refracts) — same reduction contract as the standalone material. */
    if (desc->group_count > 0u) {
        uint64_t stats_address = flux_buffer_device_address(slot->stats);
        for (uint32_t i = 0; i < desc->group_count; ++i) {
            liquid_glass_region rect =
                liquid_glass_group_stats_bounds(&desc->groups[i], image_width, image_height);
            backdrop_stats_push spush = {
                .input_handle = flux_image_bindless_handle(input),
                .blurred_handle = flux_image_bindless_handle(blurred),
                .sampler_handle = flux_device_default_sampler_handle(filter->device),
                .width = image_width,
                .height = image_height,
                .rect_x = rect.x,
                .rect_y = rect.y,
                .rect_width = rect.width,
                .rect_height = rect.height,
                .group_index = i,
                .stats_address_lo = (uint32_t)(stats_address & 0xFFFFFFFFu),
                .stats_address_hi = (uint32_t)(stats_address >> 32u),
            };
            flux_compute_dispatch(command, filter->stats_pipeline, &spush, sizeof(spush), 1u, 1u,
                                  1u);
        }
        layer_barrier_stats_write_to_compute(command, flux_buffer_vk_buffer(slot->stats));
    }

    /* 3. Frost/base layer. The pass renders a COMPLETE opaque background
     * (the sharp capture, with each frost rect blending its frosted body
     * over it), so it must run over every footprint the frame needs —
     * every frost rect AND every glass body (a lens may sample anywhere
     * its bend reaches, including outside all frost rects). Frost rects
     * are deduplicated against the body footprints they fall inside by
     * merging into the dispatch set below; each dispatch carries its own
     * rect bounds, so a body-only region simply writes the sharp base. */
    if (current_count > 0u) {
        /* Merge all current footprints into disjoint dispatch regions. */
        liquid_glass_region dispatch_regions[PRISM_BACKDROP_MAX_FROST_RECTS +
                                             PRISM_BACKDROP_MAX_GLASS_GROUPS];
        uint32_t dispatch_count = 0u;
        for (uint32_t i = 0; i < current_count; ++i) {
            liquid_glass_region region = current[i];
            if (!liquid_glass_add_clear_region(region, dispatch_regions,
                                               PRISM_BACKDROP_MAX_FROST_RECTS +
                                                   PRISM_BACKDROP_MAX_GLASS_GROUPS,
                                               &dispatch_count)) {
                return FLUX_ERROR_INVALID_STATE;
            }
        }
        bool frost_dispatched = false;
        for (uint32_t r = 0; r < dispatch_count; ++r) {
            liquid_glass_region region = dispatch_regions[r];
            if (frost_dispatched)
                layer_barrier_compute_write_to_read_write(command, output_image);
            /* Every frost rect intersecting this dispatch region is drawn
             * by one dispatch: identical writes are idempotent because the
             * pass writes absolute colours (never accumulates). */
            for (uint32_t f = 0; f < desc->frost_count; ++f) {
                const prism_backdrop_frost *frost = &desc->frost[f];
                backdrop_frost_push push = {
                    .input_handle = flux_image_bindless_handle(input),
                    .blurred_handle = flux_image_bindless_handle(blurred),
                    .sampler_handle = flux_device_default_sampler_handle(filter->device),
                    .output_handle = flux_image_bindless_storage_handle(slot->output),
                    .width = image_width,
                    .height = image_height,
                    .origin_x = region.x,
                    .origin_y = region.y,
                    .bounds = {frost->bounds.x, frost->bounds.y, frost->bounds.w, frost->bounds.h},
                    .corner_radius = fmaxf(frost->corner_radius, 0.0f),
                    .opacity = fminf(fmaxf(frost->opacity, 0.0f), 1.0f),
                    .tint_color = frost->tint_color & 0x00FFFFFFu,
                    .tint_strength = fminf(fmaxf(frost->tint_strength, 0.0f), 1.0f),
                    .region_width = region.width,
                    .region_height = region.height,
                };
                uint32_t gx = (region.width + PRISM_GLASS_WG - 1u) / PRISM_GLASS_WG;
                uint32_t gy = (region.height + PRISM_GLASS_WG - 1u) / PRISM_GLASS_WG;
                flux_compute_dispatch(command, filter->frost_pipelines[is16f ? 1 : 0], &push,
                                      sizeof(push), gx, gy, 1u);
                frost_dispatched = true;
            }
            /* A dispatch region with no frost rect still needs the sharp
             * base written for the lens: run one pass with a degenerate
             * frost rect (zero coverage — pure base write). */
            if (!frost_dispatched) {
                backdrop_frost_push push = {
                    .input_handle = flux_image_bindless_handle(input),
                    .blurred_handle = flux_image_bindless_handle(blurred),
                    .sampler_handle = flux_device_default_sampler_handle(filter->device),
                    .output_handle = flux_image_bindless_storage_handle(slot->output),
                    .width = image_width,
                    .height = image_height,
                    .origin_x = region.x,
                    .origin_y = region.y,
                    .bounds = {0.0f, 0.0f, 0.0f, 0.0f},
                    .corner_radius = 0.0f,
                    .opacity = 0.0f,
                    .tint_color = 0xFFFFFFu,
                    .tint_strength = 0.0f,
                    .region_width = region.width,
                    .region_height = region.height,
                };
                uint32_t gx = (region.width + PRISM_GLASS_WG - 1u) / PRISM_GLASS_WG;
                uint32_t gy = (region.height + PRISM_GLASS_WG - 1u) / PRISM_GLASS_WG;
                flux_compute_dispatch(command, filter->frost_pipelines[is16f ? 1 : 0], &push,
                                      sizeof(push), gx, gy, 1u);
                frost_dispatched = true;
            }
        }
    }

    /* 4. Glass layer: the lens samples the layer image — this is the
     * nesting the standalone material cannot express. Input == output here,
     * so the barrier must publish the frost/base writes to sampled reads
     * too. */
    if (current_count > 0u && current_group_count > 0u)
        layer_barrier_compute_write_to_sampled_and_storage(command, output_image);
    const prism_glass_policy policy = {
        .refraction = desc->refraction,
        .chromatic_aberration = desc->chromatic_aberration,
        .saturation = desc->saturation,
        .brightness = desc->brightness,
        .edge_width = desc->edge_width,
        .rim_light = desc->rim_light,
        .light_x = desc->light_direction.x,
        .light_y = desc->light_direction.y,
        .opacity = desc->opacity,
        .size_reference = desc->size_reference,
        .size_scale_min = desc->size_scale_min,
        .tint_strength = desc->tint_strength,
        .frost_strength = desc->frost_strength,
    };
    bool glass_dispatched = false;
    for (uint32_t i = 0; i < current_group_count; ++i) {
        const prism_liquid_glass_group *group = &desc->groups[current_group_indices[i]];
        liquid_glass_region region;
        float shadow_reach = group->shadow_alpha > 0.0f
                                 ? fmaxf(group->shadow_offset_y, 0.0f) +
                                       2.0f * fmaxf(group->shadow_blur, 0.0f)
                                 : 0.0f;
        if (!liquid_glass_group_dispatch_bounds(group, shadow_reach, image_width, image_height,
                                                &region))
            continue;
        if (glass_dispatched)
            layer_barrier_compute_write_to_sampled_and_storage(command, output_image);
        prism_glass_record_group(command, filter->glass_pipelines[is16f ? 1 : 0], filter->device,
                                 slot->output /* the frosted layer */, blurred, slot->output,
                                 image_width, image_height, region, group, &policy);
        glass_dispatched = true;
    }

    /* 5. Publish the persistent output to following stages. */
    if (clear_count > 0u || current_count > 0u || glass_dispatched)
        layer_barrier_compute_write_to_read(command, output_image);

    memcpy(slot->previous, current, current_count * sizeof(current[0]));
    slot->previous_count = current_count;
    slot->initialized = true;
    slot->stats_submitted = true;
    slot->stats_group_count = desc->group_count;
    *out = slot->output;
    return FLUX_OK;
}

flux_result prism_backdrop_layer_filter_stats(prism_backdrop_layer_filter *filter,
                                              flux_frame *frame, prism_backdrop_stat *out,
                                              uint32_t max_groups, uint32_t *out_count) {
    if (!filter || !frame || !out_count || (max_groups > 0u && !out))
        return FLUX_ERROR_INVALID_ARGUMENT;
    *out_count = 0u;
    if (flux_frame_device(frame) != filter->device)
        return FLUX_ERROR_INVALID_ARGUMENT;
    if (flux_frame_get_state(frame) != FLUX_FRAME_STATE_RECORDING)
        return FLUX_ERROR_INVALID_STATE;
    uint32_t index = flux_frame_index(frame);
    if (index >= FLUX_MAX_FRAMES_IN_FLIGHT)
        return FLUX_ERROR_OUT_OF_RANGE;
    const backdrop_layer_slot *slot = &filter->slots[index];
    if (!slot->stats_submitted || !slot->stats)
        return FLUX_ERROR_INVALID_STATE;
    uint32_t count = slot->stats_group_count < max_groups ? slot->stats_group_count : max_groups;
    if (count > 0u)
        memcpy(out, flux_buffer_mapped(slot->stats), count * sizeof(*out));
    *out_count = count;
    return FLUX_OK;
}
