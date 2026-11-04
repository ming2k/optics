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
#include <flux/profile.h>
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
    const bool missing = (need_clear && !filter->clear_pipelines[cls]) ||
                         (need_frost && !filter->frost_pipelines[cls]) ||
                         (need_glass && !filter->glass_pipelines[cls]) ||
                         (need_stats && !filter->stats_pipeline);
    if (!missing)
        return FLUX_OK;

    FLUX_PROFILE_ZONE(z_compile, "prism.backdrop_layer.compile_pipelines");

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
        if (r != FLUX_OK) {
            FLUX_PROFILE_ZONE_END(z_compile);
            return r;
        }
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
        if (r != FLUX_OK) {
            FLUX_PROFILE_ZONE_END(z_compile);
            return r;
        }
    }
    if (need_stats && !filter->stats_pipeline) {
        flux_compute_pipeline_desc pdesc = FLUX_COMPUTE_PIPELINE_DESC_INIT;
        pdesc.spirv = (const uint32_t *)backdrop_stats_spv;
        pdesc.spirv_word_count = sizeof(backdrop_stats_spv) / sizeof(uint32_t);
        pdesc.entry_point = "main";
        pdesc.push_constant_bytes = sizeof(backdrop_stats_push);
        flux_result r =
            flux_compute_pipeline_create(filter->device, &pdesc, &filter->stats_pipeline);
        if (r != FLUX_OK) {
            FLUX_PROFILE_ZONE_END(z_compile);
            return r;
        }
    }
    FLUX_PROFILE_ZONE_END(z_compile);
    return FLUX_OK;
}

flux_result prism_backdrop_layer_filter_warmup(prism_backdrop_layer_filter *filter,
                                               const prism_backdrop_layer_warmup_opts *opts) {
    if (!filter)
        return FLUX_ERROR_INVALID_ARGUMENT;

    bool is16f = false;
    bool stats = true;
    if (opts) {
        if (opts->format == FLUX_FORMAT_RGBA16_SFLOAT)
            is16f = true;
        stats = opts->stats;
    }

    /* Pre-allocate body table and id list storage buffers to eliminate driver host-visible buffer
     * allocation hitch (ADR-0141) */
    void *dummy_row = prism_body_table_row(filter->device, &filter->bodies, 1u, 0u);
    uint32_t *dummy_ids = prism_body_ids_slot(filter->device, &filter->ids, 1u);
    if (!dummy_row || !dummy_ids)
        return FLUX_ERROR_OUT_OF_MEMORY;

    return layer_ensure_pipelines(filter, is16f, true, true, true, stats);
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

/* Everything one layered-backdrop submission needs to replay its dispatches
 * later (ADR-0147). This is the widest capture domain of the five materials: the
 * composite is three stages (clear, base+plates, glass) over two destinations
 * (the layer output and the opaque scratch the lens samples), so the plan holds
 * every intermediate region and the prepared push arrays rather than recomputing
 * them — recomputation at replay time could disagree with what planning decided,
 * and those decisions are what the stages' barriers depend on. */
#define LAYER_PLAN_MAX_REGIONS (PRISM_BACKDROP_MAX_FROST_RECTS + PRISM_BACKDROP_MAX_GLASS_GROUPS)
#define LAYER_PLAN_MAX_PLATES PRISM_BACKDROP_MAX_FROST_RECTS

typedef struct layer_plan {
    flux_device *device;
    struct prism_backdrop_layer_filter *filter;
    flux_image *input;
    flux_image *blurred;
    flux_image *output;
    flux_image *scratch; /* null when no glass means no opaque base is needed */
    uint32_t image_width;
    uint32_t image_height;
    uint32_t capture_width;
    uint32_t capture_height;
    uint32_t slab_x;
    uint32_t slab_y;
    bool is16f;
    bool output_reuse_barrier;
    bool scratch_reuse_barrier;

    liquid_glass_region clear_regions[LAYER_PLAN_MAX_REGIONS];
    uint32_t clear_count;

    liquid_glass_region plate_regions[LAYER_PLAN_MAX_PLATES];
    uint32_t plate_count;

    bool draw_plates;
    bool draw_scratch;
    bool output_after_plates_barrier;

    liquid_glass_region glass_merged[PRISM_MAX_BATCHES];
    uint32_t glass_merged_count;
    prism_region_batch glass_batches[PRISM_MAX_BATCHES];
    uint32_t glass_batch_count;
    prism_glass_policy policy;
    prism_glass_copy copy;
    uint64_t bodies_address;
    uint64_t ids_address;

    liquid_glass_region stats_bounds[PRISM_BACKDROP_MAX_GLASS_GROUPS];
    uint32_t stats_group_count;
    uint64_t stats_address;
    VkBuffer stats_buffer;

    const prism_backdrop_layer_desc *desc; /* retained by the frame; see below */
} layer_plan;

static void layer_plan_record(void *raw, VkCommandBuffer command) {
    const layer_plan *p = raw;
    if (!p || command == VK_NULL_HANDLE || !p->filter)
        return;
    prism_backdrop_layer_filter *filter = p->filter;
    VkImage output_image = flux_image_vk_image(p->output);
    bool glass_dispatched = false;

    if (p->clear_count > 0u) {
        if (p->output_reuse_barrier)
            layer_barrier_reuse_to_compute_write(command, output_image);
        for (uint32_t i = 0; i < p->clear_count; ++i) {
            const liquid_glass_region *region = &p->clear_regions[i];
            storage_clear_push push = {
                .output_handle = flux_image_bindless_storage_handle(p->output),
                .width = p->image_width,
                .height = p->image_height,
                .origin_x = region->x,
                .origin_y = region->y,
                .region_width = region->width,
                .region_height = region->height,
            };
            uint32_t gx = (region->width + PRISM_GLASS_WG - 1u) / PRISM_GLASS_WG;
            uint32_t gy = (region->height + PRISM_GLASS_WG - 1u) / PRISM_GLASS_WG;
            flux_compute_dispatch(command, filter->clear_pipelines[p->is16f ? 1 : 0], &push,
                                  sizeof(push), gx, gy, 1u);
        }
        if (p->scratch) {
            VkImage scratch_image = flux_image_vk_image(p->scratch);
            if (p->scratch_reuse_barrier)
                layer_barrier_reuse_to_compute_write(command, scratch_image);
            for (uint32_t i = 0; i < p->clear_count; ++i) {
                const liquid_glass_region *region = &p->clear_regions[i];
                storage_clear_push push = {
                    .output_handle = flux_image_bindless_storage_handle(p->scratch),
                    .width = p->image_width,
                    .height = p->image_height,
                    .origin_x = region->x,
                    .origin_y = region->y,
                    .region_width = region->width,
                    .region_height = region->height,
                };
                uint32_t gx = (region->width + PRISM_GLASS_WG - 1u) / PRISM_GLASS_WG;
                uint32_t gy = (region->height + PRISM_GLASS_WG - 1u) / PRISM_GLASS_WG;
                flux_compute_dispatch(command, filter->clear_pipelines[p->is16f ? 1 : 0], &push,
                                      sizeof(push), gx, gy, 1u);
            }
            if (p->glass_batch_count > 0u)
                layer_barrier_compute_write_to_read_write(command, scratch_image);
        }
        if (p->glass_batch_count > 0u)
            layer_barrier_compute_write_to_read_write(command, output_image);
    }

    if (p->stats_group_count > 0u) {
        for (uint32_t i = 0; i < p->stats_group_count; ++i) {
            const liquid_glass_region *rect = &p->stats_bounds[i];
            backdrop_stats_push spush = {
                .input_handle = flux_image_bindless_handle(p->input),
                .blurred_handle = flux_image_bindless_handle(p->blurred),
                .sampler_handle = flux_device_default_sampler_handle(p->device),
                .width = p->capture_width,
                .height = p->capture_height,
                .rect_x = rect->x,
                .rect_y = rect->y,
                .rect_width = rect->width,
                .rect_height = rect->height,
                .group_index = i,
                .stats_address_lo = (uint32_t)(p->stats_address & 0xFFFFFFFFu),
                .stats_address_hi = (uint32_t)(p->stats_address >> 32u),
            };
            flux_compute_dispatch(command, filter->stats_pipeline, &spush, sizeof(spush), 1u, 1u,
                                  1u);
        }
        if (p->stats_buffer != VK_NULL_HANDLE)
            layer_barrier_stats_write_to_compute(command, p->stats_buffer);
    }

    if (p->draw_plates) {
        if (p->draw_scratch) {
            VkImage scratch_image = flux_image_vk_image(p->scratch);
            layer_record_base_material(command, filter, p->input, p->blurred, p->scratch,
                                       p->capture_width, p->capture_height, 0, 0, p->capture_width,
                                       p->capture_height, p->desc, p->plate_regions, p->plate_count,
                                       p->is16f);
            layer_barrier_compute_write_to_sampled_and_storage(command, scratch_image);
        }
        layer_record_base_material(command, filter, p->input, p->blurred, p->output, p->image_width,
                                   p->image_height, p->slab_x, p->slab_y, p->capture_width,
                                   p->capture_height, p->desc, p->plate_regions, p->plate_count,
                                   p->is16f);
        if (p->output_after_plates_barrier)
            layer_barrier_compute_write_to_read_write(command, output_image);
    }

    for (uint32_t r = 0; r < p->glass_batch_count; ++r) {
        const prism_region_batch *b = &p->glass_batches[r];
        if (!liquid_glass_region_is_dispatchable(
                (liquid_glass_region){b->x, b->y, b->width, b->height}, p->image_width,
                p->image_height))
            continue;
        prism_glass_record_batch(command, filter->glass_pipelines[p->is16f ? 1 : 0], p->device,
                                 &p->copy, p->output, p->image_width, p->image_height, b,
                                 &p->policy, p->bodies_address,
                                 p->ids_address + b->ids_first * sizeof(uint32_t));
        glass_dispatched = true;
    }

    if (p->clear_count > 0u || p->draw_plates || glass_dispatched)
        layer_barrier_compute_write_to_read(command, output_image);
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

    /* ADR-0147: `blurred` is normally a deferred flux_blur_filter output;
     * materialize it so its dispatches precede the layer's. */
    flux_result mat = flux_frame_materialize_image(frame, blurred);
    if (mat != FLUX_OK)
        return mat;

    uint32_t index = flux_frame_index(frame);
    if (index >= FLUX_MAX_FRAMES_IN_FLIGHT)
        return FLUX_ERROR_OUT_OF_RANGE;
    const bool need_scratch = (desc->frost_count > 0u && desc->group_count > 0u);

    const uint32_t capture_width = flux_image_width(input);
    const uint32_t capture_height = flux_image_height(input);

    FLUX_PROFILE_ZONE(z_layer, "prism.backdrop_layer.apply");

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

    /* ---- Planning: every decision the replay depends on is made here ---- */
    layer_plan plan = {
        .device = filter->device,
        .filter = filter,
        .input = input,
        .blurred = blurred,
        .output = slot->output,
        .scratch = (need_scratch && slot->scratch) ? slot->scratch : nullptr,
        .image_width = image_width,
        .image_height = image_height,
        .capture_width = capture_width,
        .capture_height = capture_height,
        .slab_x = slab_x,
        .slab_y = slab_y,
        .is16f = is16f,
        .output_reuse_barrier = slot->initialized,
        .scratch_reuse_barrier = slot->initialized,
        .draw_plates = desc->frost_count > 0u && current_count > 0u,
        .clear_count = clear_count,
        .stats_group_count = desc->group_count,
        .stats_address = slot->stats ? flux_buffer_device_address(slot->stats) : 0u,
        .stats_buffer = slot->stats ? flux_buffer_vk_buffer(slot->stats) : VK_NULL_HANDLE,
        .desc = desc,
        .policy =
            {
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
            },
    };
    memcpy(plan.clear_regions, clear_regions, clear_count * sizeof(clear_regions[0]));
    for (uint32_t i = 0; i < desc->group_count; ++i)
        plan.stats_bounds[i] =
            liquid_glass_group_stats_bounds(&desc->groups[i], capture_width, capture_height);

    /* Plate footprints, translated into slab space exactly as before. */
    if (plan.draw_plates) {
        for (uint32_t i = 0; i < desc->frost_count; ++i) {
            liquid_glass_region region;
            if (!backdrop_frost_dispatch_bounds(&desc->frost[i], capture_width, capture_height,
                                                &region))
                continue;
            region.x -= slab_x;
            region.y -= slab_y;
            plan.plate_regions[plan.plate_count++] = region;
        }
        plan.draw_scratch = plan.scratch != nullptr;
        plan.output_after_plates_barrier = current_group_count > 0u;
    }

    /* Glass batching: coalesce footprints, then assign bodies in caller order. */
    {
        const flux_image *glass_input = plan.scratch ? plan.scratch : input;
        (void)glass_input;
        liquid_glass_region glass_regions[PRISM_BACKDROP_MAX_GLASS_GROUPS];
        uint32_t glass_region_count = 0u;
        uint32_t glass_region_body[PRISM_BACKDROP_MAX_GLASS_GROUPS];
        for (uint32_t i = 0; i < current_group_count; ++i) {
            const prism_liquid_glass_group *group = &desc->groups[current_group_indices[i]];
            /* Shadow reach is per body: a body with no shadow contributes no
             * reach, which is what keeps its dispatch bounds tight. */
            float shadow_reach =
                group->shadow_alpha > 0.0f
                    ? fmaxf(group->shadow_offset_y, 0.0f) + 2.0f * fmaxf(group->shadow_blur, 0.0f)
                    : 0.0f;
            liquid_glass_region bounds;
            if (!liquid_glass_group_dispatch_bounds(group, shadow_reach, capture_width,
                                                    capture_height, &bounds))
                continue;
            bounds.x -= slab_x;
            bounds.y -= slab_y;
            glass_regions[glass_region_count] = bounds;
            glass_region_body[glass_region_count] = current_group_indices[i];
            glass_region_count++;
        }

        uint32_t glass_owner[PRISM_BACKDROP_MAX_GLASS_GROUPS];
        for (uint32_t i = 0; i < glass_region_count; ++i) {
            glass_owner[i] = UINT32_MAX;
            for (uint32_t r = 0; r < plan.glass_merged_count; ++r) {
                if (liquid_glass_regions_overlap(glass_regions[i], plan.glass_merged[r])) {
                    glass_owner[i] = r;
                    break;
                }
            }
            if (glass_owner[i] == UINT32_MAX) {
                glass_owner[i] = plan.glass_merged_count;
                plan.glass_merged[plan.glass_merged_count++] = glass_regions[i];
            }
        }

        uint32_t glass_batch_ids[PRISM_BACKDROP_MAX_GLASS_GROUPS];
        uint32_t glass_batch_ids_used = 0u;
        for (uint32_t r = 0; r < plan.glass_merged_count; ++r) {
            uint32_t first = glass_batch_ids_used;
            for (uint32_t i = 0; i < glass_region_count; ++i) {
                if (glass_owner[i] == r)
                    glass_batch_ids[glass_batch_ids_used++] = glass_region_body[i];
            }
            plan.glass_batches[plan.glass_batch_count++] = (prism_region_batch){
                .x = plan.glass_merged[r].x,
                .y = plan.glass_merged[r].y,
                .width = plan.glass_merged[r].width,
                .height = plan.glass_merged[r].height,
                .ids_first = first,
                .id_count = glass_batch_ids_used - first,
            };
        }

        if (plan.glass_batch_count > 0u) {
            uint32_t rows = desc->group_count;
            for (uint32_t i = 0; i < rows; ++i) {
                void *row = prism_body_table_row(filter->device, &filter->bodies, rows, i);
                if (!row)
                    return FLUX_ERROR_OUT_OF_MEMORY;
                *(prism_glass_body_row *)row = prism_glass_make_row(&desc->groups[i], &plan.policy);
            }
            uint32_t *ids = prism_body_ids_slot(filter->device, &filter->ids, glass_batch_ids_used);
            if (!ids)
                return FLUX_ERROR_OUT_OF_MEMORY;
            for (uint32_t i = 0; i < glass_batch_ids_used; ++i)
                ids[i] = glass_batch_ids[i];
            plan.bodies_address = filter->bodies.address;
            plan.ids_address = filter->ids.address;
            plan.copy = (prism_glass_copy){
                .sharp = glass_input,
                .blurred = blurred,
                .capture_width = (float)capture_width,
                .capture_height = (float)capture_height,
                .slab_origin_x = (float)slab_x,
                .slab_origin_y = (float)slab_y,
            };
        }
    }

    memcpy(slot->previous, current, current_count * sizeof(current[0]));
    slot->previous_count = current_count;
    slot->initialized = true;
    slot->stats_submitted = true;
    slot->stats_group_count = desc->group_count;

    *out = (prism_material_output){
        .image = slot->output,
        .origin_x = (float)slab_x,
        .origin_y = (float)slab_y,
        .width = slot->width,
        .height = slot->height,
    };

    /* Defer the compound submission (ADR-0147). The output handle above is valid
     * now; the three stages replay only if something requires the output. The
     * descriptor must outlive the frame's recording, which the public contract
     * already requires for `frost`/`groups` pointers. */
    prism_deferred_pass pass = PRISM_DEFERRED_PASS_INIT;
    pass.output = slot->output;
    pass.label = "prism.backdrop_layer";
    pass.inputs[0] = input;
    pass.inputs[1] = blurred;
    pass.input_count = 2;
    flux_deferred_token *token = nullptr;
    flux_result reg =
        prism_deferred_register(frame, &pass, layer_plan_record, &plan, sizeof(plan), &token);
    if (reg != FLUX_OK) {
        layer_plan_record(&plan, flux_frame_vk_command_buffer(frame));
        token = nullptr;
    }
    (void)prism_deferred_require(frame, token);
    FLUX_PROFILE_ZONE_END(z_layer);
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
