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

#include "prism_observe.h"
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

/* Shader bytecode: C23 #embed, or generated headers (tools/build/spv2h.py). */
#if defined(__has_embed) && !defined(PRISM_SHADER_NO_EMBED)
#if __has_embed("liquid_glass.comp.spv") && __has_embed("storage_clear.comp.spv") &&               \
    __has_embed("backdrop_stats.comp.spv") && __has_embed("backdrop_frost.comp.spv") &&            \
    __has_embed("liquid_glass_16f.comp.spv") && __has_embed("storage_clear_16f.comp.spv") &&       \
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
 * bounds vec4 at byte 32 on both sides; `mode`/`reserved` close it. */
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
    uint32_t mode; /* backdrop_frost_mode */
    uint32_t reserved;
    /* This slab's origin in capture space and the capture's extent: the plate
     * samples the capture, but the slab is sized to the layer's footprints, so a
     * slab pixel's capture UV needs both. */
    float slab_origin_x;
    float slab_origin_y;
    float capture_width;
    float capture_height;
} backdrop_frost_push;

/* backdrop_frost.comp modes (keep in lockstep with the shader defines). */
typedef enum backdrop_frost_mode : uint32_t {
    BACKDROP_FROST_MODE_BASE = 0u,  /* resolve the opaque background */
    BACKDROP_FROST_MODE_PLATE = 1u, /* source-over one plate inside its coverage */
} backdrop_frost_mode;

static_assert(sizeof(storage_clear_push) == 28,
              "storage_clear_push no longer matches its shader block");
static_assert(sizeof(backdrop_stats_push) == 48,
              "backdrop_stats_push no longer matches its shader block");
static_assert(sizeof(backdrop_frost_push) == 96,
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
    flux_image *scratch;
    bool initialized;
    /* Clear footprints: previous and current frost rects + glass bodies. */
    uint32_t previous_count;
    liquid_glass_region previous[PRISM_BACKDROP_MAX_FROST_RECTS + PRISM_BACKDROP_MAX_GLASS_GROUPS];
    flux_buffer *stats;
    bool stats_submitted;
    uint32_t stats_group_count;
    /* Where this slab sits in capture space (0,0 while the layer is still
     * capture-sized; see prism_material_frame.h for the migration). */
    uint32_t slab_x;
    uint32_t slab_y;
    /* Backdrop layer cache (ADR-0123 / INV-COMP-04). `cached_revision` is the
     * caller-supplied prism_backdrop_layer_revision the current `output`
     * contents belong to; 0 means the slot holds nothing cacheable. */
    uint64_t cached_revision;
} backdrop_layer_slot;

/* Resolve the caller's content revision from the descriptor's `next` chain.
 * Returns FLUX_OK with *out_revision = 0 when the caller did not opt in, which
 * disables caching rather than guessing that the sources are static. */
static flux_result layer_resolve_revision(const prism_backdrop_layer_desc *desc,
                                          uint64_t *out_revision) {
    *out_revision = 0u;
    for (const prism_backdrop_layer_revision *ext = desc->next; ext; ext = ext->next) {
        /* The chain is type-tagged so a caller can skip an extension it does
         * not understand; prism only tolerates tags it defines. Unlike flux's
         * effect extensions, an unknown tag is an error rather than silently
         * ignored: a revision the material cannot read would otherwise make a
         * cache-enabled call behave like a cache-disabled one, and the caller
         * would never learn its caching assumption was dropped. */
        if (ext->type != PRISM_TYPE_BACKDROP_LAYER_REVISION)
            return FLUX_ERROR_INVALID_ARGUMENT;
        *out_revision = ext->revision;
    }
    return FLUX_OK;
}

struct prism_backdrop_layer_filter {
    atomic_uint ref_count;
    flux_device *device;                       /* retained; slot images hold weak device refs */
    flux_compute_pipeline *glass_pipelines[2]; /* [0] rgba8, [1] rgba16f */
    flux_compute_pipeline *clear_pipelines[2];
    flux_compute_pipeline *frost_pipelines[2];
    flux_compute_pipeline *stats_pipeline;
    /* Per-body parameters and per-region body ids (see prism_material_frame.h). */
    prism_body_table bodies;
    prism_body_ids ids;
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
static void layer_barrier_compute_write_to_sampled_and_storage(VkCommandBuffer cmd, VkImage image) {
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

/* Ensure the layer's slabs can hold `footprint_w` x `footprint_h` capture pixels.
 * Both the output and the frost scratch are slab-sized on the same size ladder as
 * every other prism material, so a 48-px chrome strip does not allocate three
 * capture-sized RGBA16F images. Re-allocation happens only when the quantised
 * extent changes, which also makes a resize sweep reuse the same images. */
static flux_result layer_ensure_slot(prism_backdrop_layer_filter *filter, uint32_t index,
                                     const flux_image *input, uint32_t footprint_w,
                                     uint32_t footprint_h, bool need_stats, bool need_scratch) {
    backdrop_layer_slot *slot = &filter->slots[index];
    uint32_t width = 0u;
    uint32_t height = 0u;
    prism_slab_for_footprint(footprint_w, footprint_h, &width, &height);
    flux_format format = prism_layer_output_format(input);
    const uint32_t cap_w = flux_image_width(input);
    const uint32_t cap_h = flux_image_height(input);
    if (slot->output && slot->width == width && slot->height == height && slot->format == format) {
        if (need_scratch && (!slot->scratch || flux_image_width(slot->scratch) != cap_w ||
                             flux_image_height(slot->scratch) != cap_h)) {
            if (slot->scratch)
                flux_image_release(slot->scratch);
            flux_result r = flux_image_create_compute_writable(filter->device, cap_w, cap_h, format,
                                                               &slot->scratch);
            if (r != FLUX_OK)
                return r;
        }
        return need_stats ? layer_ensure_stats_buffer(filter, slot) : FLUX_OK;
    }
    if (slot->output)
        flux_image_release(slot->output);
    if (slot->scratch)
        flux_image_release(slot->scratch);
    if (slot->stats)
        flux_buffer_release(slot->stats);
    *slot = (backdrop_layer_slot){0};
    flux_result r =
        flux_image_create_compute_writable(filter->device, width, height, format, &slot->output);
    if (r != FLUX_OK)
        return r;
    if (need_scratch) {
        r = flux_image_create_compute_writable(filter->device, cap_w, cap_h, format,
                                               &slot->scratch);
        if (r != FLUX_OK)
            return r;
    }
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
        pdesc.spirv_word_count =
            (is16f ? sizeof(backdrop_frost_16f_spv) : sizeof(backdrop_frost_spv)) /
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
/*  Base-material pass recording                                     */
/* ------------------------------------------------------------------ */

/* Record the two-stage base-material composite into `dst`:
 *
 *   1. one BASE dispatch per merged region, resolving the opaque background;
 *   2. one PLATE dispatch per (region, plate), in caller array order.
 *
 * Regions are disjoint by construction (`liquid_glass_add_clear_region`
 * merges transitively), so the BASE passes can never overwrite a plate and
 * need no ordering barrier between them. Plates within a region are ordered
 * by a single write→read/write barrier per (region, plate) boundary — the
 * same idiom the glass pass uses, because each plate reads the pixels the
 * previous one wrote.
 *
 * The caller publishes the finished image afterwards with the appropriate
 * barrier; this helper only records commands. */
static void layer_record_base_material(
    VkCommandBuffer command, const prism_backdrop_layer_filter *filter, const flux_image *input,
    const flux_image *blurred, flux_image *dst, uint32_t slab_width, uint32_t slab_height,
    uint32_t slab_x, uint32_t slab_y, uint32_t capture_width, uint32_t capture_height,
    const prism_backdrop_layer_desc *desc, const liquid_glass_region *plate_regions,
    uint32_t plate_region_count, bool is16f) {
    flux_compute_pipeline *pipeline = filter->frost_pipelines[is16f ? 1 : 0];
    VkImage dst_image = flux_image_vk_image(dst);
    uint32_t dst_storage_handle = flux_image_bindless_storage_handle(dst);
    uint32_t input_handle = flux_image_bindless_handle(input);
    uint32_t blurred_handle = flux_image_bindless_handle(blurred);
    uint32_t sampler_handle = flux_device_default_sampler_handle(filter->device);

    /* Stage 1 — resolve the background over the WHOLE slab, not just the plate
     * footprints.
     *
     * This image is the optical lens's sample source, and a lens taps up to
     * `refraction` pixels away from its own silhouette. If the background were
     * only resolved where a plate sits, every tap that landed past a plate would
     * read a cleared (transparent, premultiplied-zero) pixel and the body would
     * darken along its rim — the exact "transparent hole" failure the opaque-base
     * stage exists to prevent. Covering the slab makes every tap land on a true
     * background colour.
     *
     * Cheap by construction: the slab is already the union of the layer's
     * footprints, so this is one dispatch over at most the footprint bounding box,
     * never the capture. */
    const uint32_t gx_full = (slab_width + PRISM_GLASS_WG - 1u) / PRISM_GLASS_WG;
    const uint32_t gy_full = (slab_height + PRISM_GLASS_WG - 1u) / PRISM_GLASS_WG;
    backdrop_frost_push base = {
        .input_handle = input_handle,
        .blurred_handle = blurred_handle,
        .sampler_handle = sampler_handle,
        .output_handle = dst_storage_handle,
        .width = slab_width,
        .height = slab_height,
        .region_width = slab_width,
        .region_height = slab_height,
        .mode = BACKDROP_FROST_MODE_BASE,
        .slab_origin_x = (float)slab_x,
        .slab_origin_y = (float)slab_y,
        .capture_width = (float)capture_width,
        .capture_height = (float)capture_height,
    };
    flux_compute_dispatch(command, pipeline, &base, sizeof(base), gx_full, gy_full, 1u);

    /* Base and plates write overlapping pixels (each plate reads back what the
     * base wrote), so the base must be visible to the first plate's imageLoad.
     * One barrier per destination, not per plate. */
    if (plate_region_count > 0u)
        layer_barrier_compute_write_to_read_write(command, dst_image);

    /* Stage 2 — plates, in caller order, source-over inside their own coverage. */
    for (uint32_t r = 0; r < plate_region_count; ++r) {
        const liquid_glass_region region = plate_regions[r];
        if (!liquid_glass_region_is_dispatchable(region, slab_width, slab_height))
            continue;
        const uint32_t gx = (region.width + PRISM_GLASS_WG - 1u) / PRISM_GLASS_WG;
        const uint32_t gy = (region.height + PRISM_GLASS_WG - 1u) / PRISM_GLASS_WG;
        for (uint32_t f = 0; f < desc->frost_count; ++f) {
            const prism_base_material *plate = &desc->frost[f];
            backdrop_frost_push push = {
                .input_handle = input_handle,
                .blurred_handle = blurred_handle,
                .sampler_handle = sampler_handle,
                .output_handle = dst_storage_handle,
                .width = slab_width,
                .height = slab_height,
                .origin_x = region.x,
                .origin_y = region.y,
                .bounds = {plate->bounds.x, plate->bounds.y, plate->bounds.w, plate->bounds.h},
                .corner_radius = fmaxf(plate->corner_radius, 0.0f),
                .opacity = fminf(fmaxf(plate->opacity, 0.0f), 1.0f),
                .tint_color = plate->tint_color & 0x00FFFFFFu,
                .tint_strength = fminf(fmaxf(plate->tint_strength, 0.0f), 1.0f),
                .region_width = region.width,
                .region_height = region.height,
                .mode = BACKDROP_FROST_MODE_PLATE,
                .slab_origin_x = (float)slab_x,
                .slab_origin_y = (float)slab_y,
                .capture_width = (float)capture_width,
                .capture_height = (float)capture_height,
            };
            flux_compute_dispatch(command, pipeline, &push, sizeof(push), gx, gy, 1u);
            /* Each plate is read back by the next one. */
            if (f + 1u < desc->frost_count || r + 1u < plate_region_count)
                layer_barrier_compute_write_to_read_write(command, dst_image);
        }
    }
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
        !isfinite(desc->tint_strength) || !isfinite(desc->frost_strength) ||
        !isfinite(desc->curvature) || !isfinite(desc->contact_ao) ||
        !isfinite(desc->ambient_fresnel))
        return false;
    for (uint32_t i = 0; i < desc->frost_count; ++i) {
        const prism_backdrop_frost *frost = &desc->frost[i];
        if (!isfinite(frost->bounds.x) || !isfinite(frost->bounds.y) ||
            !isfinite(frost->bounds.w) || !isfinite(frost->bounds.h) ||
            !isfinite(frost->corner_radius) || !isfinite(frost->opacity) ||
            !isfinite(frost->tint_strength) || frost->bounds.w <= 0.0f || frost->bounds.h <= 0.0f)
            return false;
    }
    for (uint32_t i = 0; i < desc->group_count; ++i) {
        if (!liquid_glass_group_is_valid(&desc->groups[i]))
            return false;
    }
    return true;
}

static bool backdrop_frost_dispatch_bounds(const prism_backdrop_frost *frost, uint32_t image_width,
                                           uint32_t image_height, liquid_glass_region *out) {
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
    /* Caller's allocator, attributed to prism (ADR-0135). */
    prism_backdrop_layer_filter *filter = nullptr;
    if (flux_device_alloc(device,
                          &(flux_device_alloc_opts){
                              .type = FLUX_TYPE_DEVICE_ALLOC_OPTS,
                              .bytes = sizeof(*filter),
                              .owner = PRISM_OBSERVE_OWNER,
                              .kind = PRISM_OBSERVE_KIND_BACKDROP_LAYER,
                              .scope = FLUX_SCOPE_DEVICE,
                          },
                          (void **)&filter) != FLUX_OK)
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
        if (filter->slots[i].scratch)
            flux_image_release(filter->slots[i].scratch);
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
    prism_body_table_release(&filter->bodies);
    prism_body_ids_release(&filter->ids);
    flux_device_free(device, &(flux_device_free_opts){
                                 .type = FLUX_TYPE_DEVICE_FREE_OPTS,
                                 .ptr = filter,
                                 .owner = PRISM_OBSERVE_OWNER,
                                 .kind = PRISM_OBSERVE_KIND_BACKDROP_LAYER,
                                 .scope = FLUX_SCOPE_DEVICE,
                             });
    flux_device_release(device);
}

flux_result prism_backdrop_layer_filter_apply(prism_backdrop_layer_filter *filter,
                                              flux_frame *frame,
                                              const prism_backdrop_layer_desc *desc,
                                              prism_material_output *out) {
    if (!filter || !frame || !desc || !out)
        return FLUX_ERROR_INVALID_ARGUMENT;
    *out = (prism_material_output){0};
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
    const bool need_scratch = (desc->frost_count > 0u && desc->group_count > 0u);

    const uint32_t capture_width = flux_image_width(input);
    const uint32_t capture_height = flux_image_height(input);

    /* 1. Footprints in capture space: plates first, then bodies — the same order
     * the passes run in, so the merged clear set covers exactly what this
     * submission writes. */
    liquid_glass_region current[PRISM_BACKDROP_MAX_FROST_RECTS + PRISM_BACKDROP_MAX_GLASS_GROUPS];
    uint32_t current_count = 0u;
    uint32_t footprint_x0 = UINT32_MAX, footprint_y0 = UINT32_MAX;
    uint32_t footprint_x1 = 0u, footprint_y1 = 0u;
    for (uint32_t i = 0; i < desc->frost_count; ++i) {
        liquid_glass_region region;
        if (!backdrop_frost_dispatch_bounds(&desc->frost[i], capture_width, capture_height,
                                            &region))
            continue;
        current[current_count++] = region;
        if (region.x < footprint_x0)
            footprint_x0 = region.x;
        if (region.y < footprint_y0)
            footprint_y0 = region.y;
        if (region.x + region.width > footprint_x1)
            footprint_x1 = region.x + region.width;
        if (region.y + region.height > footprint_y1)
            footprint_y1 = region.y + region.height;
    }
    uint32_t current_group_indices[PRISM_BACKDROP_MAX_GLASS_GROUPS];
    uint32_t current_group_count = 0u;
    for (uint32_t i = 0; i < desc->group_count; ++i) {
        const prism_liquid_glass_group *group = &desc->groups[i];
        float shadow_reach = group->shadow_alpha > 0.0f ? fmaxf(group->shadow_offset_y, 0.0f) +
                                                              2.0f * fmaxf(group->shadow_blur, 0.0f)
                                                        : 0.0f;
        liquid_glass_region region;
        if (!liquid_glass_group_dispatch_bounds(group, shadow_reach, capture_width, capture_height,
                                                &region))
            continue;
        current[current_count++] = region;
        current_group_indices[current_group_count++] = i;
        if (region.x < footprint_x0)
            footprint_x0 = region.x;
        if (region.y < footprint_y0)
            footprint_y0 = region.y;
        if (region.x + region.width > footprint_x1)
            footprint_x1 = region.x + region.width;
        if (region.y + region.height > footprint_y1)
            footprint_y1 = region.y + region.height;
    }

    uint32_t footprint_w = 1u;
    uint32_t footprint_h = 1u;
    if (current_count > 0u) {
        footprint_w = footprint_x1 - footprint_x0;
        footprint_h = footprint_y1 - footprint_y0;
    }
    flux_result result = layer_ensure_slot(filter, index, input, footprint_w, footprint_h,
                                           desc->group_count > 0u, need_scratch);
    if (result != FLUX_OK)
        return result;

    backdrop_layer_slot *slot = &filter->slots[index];

    /* The slab covers every footprint, so its origin IS the footprint origin and
     * regions are stored in slab space from here on (including the ones kept for
     * the next submission's clear). */
    const uint32_t image_width = slot->width;
    const uint32_t image_height = slot->height;
    const uint32_t slab_x = current_count > 0u ? footprint_x0 : 0u;
    const uint32_t slab_y = current_count > 0u ? footprint_y0 : 0u;
    for (uint32_t i = 0; i < current_count; ++i) {
        current[i].x -= slab_x;
        current[i].y -= slab_y;
    }
    slot->slab_x = slab_x;
    slot->slab_y = slab_y;

    uint64_t revision = 0u;
    result = layer_resolve_revision(desc, &revision);
    if (result != FLUX_OK)
        return result;

    if (revision != 0u && slot->initialized && slot->cached_revision == revision) {
        /* Material Backdrop Boundary cache hit (ADR-0123 / INV-COMP-04): the
         * caller declared this submission's sources and parameters identical to
         * the ones that produced the current slot output, so the clear, stats,
         * base-material and glass passes are skipped and the persistent image
         * is reused. The key is the caller's revision, not image handles: a
         * capture target and a pooled blur output keep their handles across
         * repaints, so a handle test would freeze stale content. */
        *out = (prism_material_output){
            .image = slot->output,
            .origin_x = (float)slot->slab_x,
            .origin_y = (float)slot->slab_y,
            .width = slot->width,
            .height = slot->height,
        };
        return FLUX_OK;
    }

    const uint32_t clear_capacity =
        PRISM_BACKDROP_MAX_FROST_RECTS + PRISM_BACKDROP_MAX_GLASS_GROUPS;
    liquid_glass_region clear_regions[clear_capacity];
    uint32_t clear_count = 0u;
    if (!liquid_glass_build_clear_regions(
            slot->initialized, image_width, image_height, slot->previous, slot->previous_count,
            current, current_count, clear_regions, clear_capacity, &clear_count))
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
        if (need_scratch && slot->scratch) {
            VkImage scratch_image = flux_image_vk_image(slot->scratch);
            if (slot->initialized)
                layer_barrier_reuse_to_compute_write(command, scratch_image);
            for (uint32_t i = 0; i < clear_count; ++i) {
                liquid_glass_region region = clear_regions[i];
                storage_clear_push push = {
                    .output_handle = flux_image_bindless_storage_handle(slot->scratch),
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
                layer_barrier_compute_write_to_read_write(command, scratch_image);
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
                liquid_glass_group_stats_bounds(&desc->groups[i], capture_width, capture_height);
            backdrop_stats_push spush = {
                .input_handle = flux_image_bindless_handle(input),
                .blurred_handle = flux_image_bindless_handle(blurred),
                .sampler_handle = flux_device_default_sampler_handle(filter->device),
                .width = capture_width,
                .height = capture_height,
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

    /* 3. Base-material layer.
     *
     * The layer is a COMPLETE opaque background carrying base plates, and it
     * is composed in two stages over each merged footprint region:
     *
     *   BASE  — resolve the sharp capture, alpha 1, once per region. The
     *           optical lens above samples this image wherever its bend taps,
     *           so pixels between and outside plates must already hold a true
     *           background colour (never a cleared hole and never a
     *           premultiplied fragment).
     *   PLATE — one source-over dispatch per (region, plate), in caller array
     *           order, writing only inside that plate's analytic coverage.
     *
     * Splitting the stages is what makes multiple plates composable: a single
     * unconditional "write opaque pixel" pass per plate made the last plate in
     * the array erase every earlier plate inside the shared region.
     *
     * Two destinations exist because the glass pass must not sample the image
     * it writes:
     *   - slot->scratch (opaque base + plates) is what the lens samples when
     *     glass bodies are present;
     *   - slot->output receives the same composite when there is no glass, so
     *     a layer with plates only still produces the finished material.
     * When the descriptor carries no plates the stage is skipped entirely: the
     * lens samples the sharp capture directly.
     */
    if (desc->frost_count > 0u && current_count > 0u) {
        /* Plate footprints are the leading run of `current` (plates are appended
         * before bodies). The base stage resolves the whole slab and paints only
         * these. */
        liquid_glass_region plate_regions[PRISM_BACKDROP_MAX_FROST_RECTS];
        uint32_t plate_count = 0u;
        for (uint32_t i = 0; i < desc->frost_count; ++i) {
            liquid_glass_region region;
            if (!backdrop_frost_dispatch_bounds(&desc->frost[i], capture_width, capture_height,
                                                &region))
                continue;
            /* `current` is in slab space; translate this plate the same way. */
            region.x -= slab_x;
            region.y -= slab_y;
            plate_regions[plate_count++] = region;
        }
        /* Merge all current footprints into disjoint dispatch regions. Same
         * overflow policy as liquid_glass_build_clear_regions: a footprint set
         * that cannot coalesce into the dispatch budget degrades to one
         * full-image region rather than failing a legal descriptor. */
        const uint32_t dispatch_capacity =
            PRISM_BACKDROP_MAX_FROST_RECTS + PRISM_BACKDROP_MAX_GLASS_GROUPS;
        liquid_glass_region dispatch_regions[dispatch_capacity];
        uint32_t dispatch_count = 0u;
        for (uint32_t i = 0; i < current_count; ++i) {
            if (!liquid_glass_add_clear_region(current[i], dispatch_regions, dispatch_capacity,
                                               &dispatch_count)) {
                dispatch_regions[0] = (liquid_glass_region){0u, 0u, image_width, image_height};
                dispatch_count = 1u;
                break;
            }
        }

        const bool draw_scratch = need_scratch && slot->scratch;
        if (draw_scratch) {
            VkImage scratch_image = flux_image_vk_image(slot->scratch);
            layer_record_base_material(command, filter, input, blurred, slot->scratch,
                                       capture_width, capture_height, 0, 0, capture_width,
                                       capture_height, desc, plate_regions, plate_count, is16f);
            layer_barrier_compute_write_to_sampled_and_storage(command, scratch_image);
        }
        /* The output always carries the finished plate composite. When glass bodies
         * also exist, the glass pass reads the plates from slot->scratch and blends
         * on top of slot->output. */
        layer_record_base_material(command, filter, input, blurred, slot->output, image_width,
                                   image_height, slab_x, slab_y, capture_width, capture_height,
                                   desc, plate_regions, plate_count, is16f);
        if (current_group_count > 0u)
            layer_barrier_compute_write_to_read_write(command, output_image);
    }

    /* 4. Glass layer:
     * When frost exists, the lens samples the frosted layer (slot->scratch).
     * When no frost exists, the lens samples the sharp capture (desc->input).
     * In both cases, input != slot->output, guaranteeing zero in-place cross-workgroup race
     * hazards. */
    const flux_image *glass_input = (need_scratch && slot->scratch) ? slot->scratch : input;
    /* `draw_scratch` in step 3 and `glass_input` here must agree: the lens may
     * only sample the scratch when the plate composite was actually written
     * into it. */
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
        .curvature = desc->curvature,
        .contact_ao = desc->contact_ao,
        .ambient_fresnel = desc->ambient_fresnel,
    };
    /* Batched glass: coalesce the body footprints into disjoint regions and
     * evaluate each region's bodies in one dispatch, in caller order.
     *
     * This is what removes both the per-body dispatch and the per-body barrier.
     * Bodies that share pixels necessarily coalesce into one region, so their
     * source-over order is the shader's loop order; disjoint regions never write
     * each other's pixels, so no image barrier is required. See liquid_glass.c
     * for the same construction and the reasoning about the Vulkan memory model. */
    prism_region_batch glass_batches[PRISM_MAX_BATCHES];
    uint32_t glass_batch_ids[PRISM_MAX_BODIES];
    uint32_t glass_batch_ids_used = 0u;
    liquid_glass_region glass_merged[PRISM_MAX_BATCHES];
    uint32_t glass_merged_count = 0u;
    /* Body footprints in capture space for this submission. */
    liquid_glass_region glass_regions[PRISM_BACKDROP_MAX_GLASS_GROUPS];
    uint32_t glass_region_body[PRISM_BACKDROP_MAX_GLASS_GROUPS];
    uint32_t glass_region_count = 0u;
    bool glass_dispatched = false;
    for (uint32_t i = 0; i < current_group_count; ++i) {
        const prism_liquid_glass_group *group = &desc->groups[current_group_indices[i]];
        float shadow_reach = group->shadow_alpha > 0.0f ? fmaxf(group->shadow_offset_y, 0.0f) +
                                                              2.0f * fmaxf(group->shadow_blur, 0.0f)
                                                        : 0.0f;
        liquid_glass_region region;
        if (!liquid_glass_group_dispatch_bounds(group, shadow_reach, capture_width, capture_height,
                                                &region))
            continue;
        region.x -= slab_x;
        region.y -= slab_y;
        glass_regions[glass_region_count] = region;
        glass_region_body[glass_region_count] = i;
        ++glass_region_count;
    }
    if (glass_region_count > 0u) {
        for (uint32_t i = 0; i < glass_region_count; ++i) {
            if (!liquid_glass_add_clear_region(glass_regions[i], glass_merged, PRISM_MAX_BATCHES,
                                               &glass_merged_count)) {
                glass_merged[0] = (liquid_glass_region){0u, 0u, image_width, image_height};
                glass_merged_count = 1u;
                break;
            }
        }
        uint32_t glass_owner[PRISM_BACKDROP_MAX_GLASS_GROUPS];
        for (uint32_t i = 0; i < glass_region_count; ++i) {
            glass_owner[i] = UINT32_MAX;
            for (uint32_t r = 0; r < glass_merged_count; ++r) {
                if (liquid_glass_regions_overlap(glass_regions[i], glass_merged[r])) {
                    glass_owner[i] = r;
                    break;
                }
            }
        }
        uint32_t glass_batch_count = 0u;
        for (uint32_t r = 0; r < glass_merged_count; ++r) {
            uint32_t first = glass_batch_ids_used;
            for (uint32_t i = 0; i < glass_region_count; ++i) {
                if (glass_owner[i] == r)
                    glass_batch_ids[glass_batch_ids_used++] =
                        current_group_indices[glass_region_body[i]];
            }
            glass_batches[glass_batch_count++] = (prism_region_batch){
                .x = glass_merged[r].x,
                .y = glass_merged[r].y,
                .width = glass_merged[r].width,
                .height = glass_merged[r].height,
                .ids_first = first,
                .id_count = glass_batch_ids_used - first,
            };
        }

        uint32_t rows = desc->group_count;
        for (uint32_t i = 0; i < rows; ++i) {
            void *row = prism_body_table_row(filter->device, &filter->bodies, rows, i);
            if (!row)
                return FLUX_ERROR_OUT_OF_MEMORY;
            *(prism_glass_body_row *)row = prism_glass_make_row(&desc->groups[i], &policy);
        }
        uint32_t *ids = prism_body_ids_slot(filter->device, &filter->ids, glass_batch_ids_used);
        if (!ids)
            return FLUX_ERROR_OUT_OF_MEMORY;
        for (uint32_t i = 0; i < glass_batch_ids_used; ++i)
            ids[i] = glass_batch_ids[i];
        /* `ids_first` is a slot index into this filter's id table; make it the
         * absolute offset every later use expects. */
        for (uint32_t r = 0; r < glass_batch_count; ++r)
            glass_batches[r].ids_first = glass_batches[r].ids_first;

        const prism_glass_copy copy = {
            .sharp = glass_input,
            .blurred = blurred,
            .capture_width = (float)capture_width,
            .capture_height = (float)capture_height,
            .slab_origin_x = (float)slab_x,
            .slab_origin_y = (float)slab_y,
        };
        for (uint32_t r = 0; r < glass_batch_count; ++r) {
            if (!liquid_glass_region_is_dispatchable(glass_merged[r], image_width, image_height))
                continue;
            prism_glass_record_batch(command, filter->glass_pipelines[is16f ? 1 : 0],
                                     filter->device, &copy, slot->output, image_width, image_height,
                                     &glass_batches[r], &policy, filter->bodies.address,
                                     filter->ids.address +
                                         glass_batches[r].ids_first * sizeof(uint32_t));
            glass_dispatched = true;
        }
    }

    /* 5. Publish the persistent output to following stages. */
    if (clear_count > 0u || current_count > 0u || glass_dispatched)
        layer_barrier_compute_write_to_read(command, output_image);

    memcpy(slot->previous, current, current_count * sizeof(current[0]));
    slot->previous_count = current_count;
    slot->initialized = true;
    slot->stats_submitted = true;
    slot->stats_group_count = desc->group_count;
    slot->cached_revision = revision;
    *out = (prism_material_output){
        .image = slot->output,
        .origin_x = (float)slab_x,
        .origin_y = (float)slab_y,
        .width = slot->width,
        .height = slot->height,
    };
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
