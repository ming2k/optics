/*
 * Image-domain effects. v1 ships the Gaussian blur operator (see
 * ADR-0008). Module state hangs off flux_device via the same
 * pattern the canvas module uses (ADR-0002): a typed pointer + a
 * destroy callback set lazily on first use.
 *
 * Layout in this file:
 *   - module state struct + per-device init/destroy
 *   - transient-image pool (intermediate cache + output list)
 *   - compute pipeline lazy-build (single shader, axis push-const)
 *   - flux_effect_blur exact Gaussian entry point
 *   - fixed-cost multi-resolution filter for animated backdrops
 */

#include "../core/image_internal.h" /* struct flux_image + create_compute_writable */
#include "../compute/internal.h"      /* flux_compute_pipeline_make_device_weak */
#include "../core/internal.h"
#include <flux/compute.h>
#include <flux/effect.h>
#include <flux/vulkan.h>

#include <math.h>
#include <stdalign.h>
#include <stdlib.h>
#include <string.h>

alignas(uint32_t) static const unsigned char effect_blur_spv[] = {
#embed "effect_blur.comp.spv"
};

alignas(uint32_t) static const unsigned char effect_backdrop_spv[] = {
#embed "effect_backdrop.comp.spv"
};

alignas(uint32_t) static const unsigned char effect_liquid_glass_spv[] = {
#embed "effect_liquid_glass.comp.spv"
};

/* Push-constant block — must match the layout in effect_blur.comp. */
typedef struct effect_blur_push {
    uint32_t in_handle;
    uint32_t sampler_handle;
    uint32_t out_handle;
    uint32_t width;
    uint32_t height;
    int32_t radius;
    float sigma;
    uint32_t axis; /* 0 horizontal, 1 vertical */
} effect_blur_push;

/* Push-constant block — must match the layout in effect_backdrop.comp. */
typedef struct effect_backdrop_push {
    uint32_t in_handle;
    uint32_t sampler_handle;
    uint32_t out_handle;
    uint32_t input_width;
    uint32_t input_height;
    uint32_t output_width;
    uint32_t output_height;
    float offset;
    uint32_t mode; /* 0 downsample, 1 upsample, 2 copy */
} effect_backdrop_push;

/* Push-constant block — must match effect_liquid_glass.comp exactly. */
typedef struct effect_liquid_glass_push {
    uint32_t input_handle;
    uint32_t blurred_handle;
    uint32_t sampler_handle;
    uint32_t output_handle;
    uint32_t width;
    uint32_t height;
    uint32_t origin_x;
    uint32_t origin_y;
    float shape0[4];
    float shape1[4];
    float radius0;
    float radius1;
    float blend_radius;
    float opacity;
    float refraction;
    float chromatic_aberration;
    float saturation;
    float brightness;
    float edge_width;
    float glare;
    float light_x;
    float light_y;
    uint32_t group_width;
    uint32_t group_height;
    uint32_t shape_count;
    float shadow_alpha;
    float shadow_blur;
    float shadow_offset_y;
    float size_reference;
    float size_scale_min;
    float tint_strength;
    float frost_strength;
    uint32_t tint_color;
} effect_liquid_glass_push;

static_assert(sizeof(effect_blur_push) <= FLUX_DEVICE_REQUIRED_PUSH_BYTES,
              "effect_blur_push exceeds device-wide push budget");
static_assert(sizeof(effect_backdrop_push) <= FLUX_DEVICE_REQUIRED_PUSH_BYTES,
              "effect_backdrop_push exceeds device-wide push budget");
static_assert(sizeof(effect_liquid_glass_push) == 156,
              "effect_liquid_glass_push no longer matches its shader block");
static_assert(sizeof(effect_liquid_glass_push) <= FLUX_DEVICE_REQUIRED_PUSH_BYTES,
              "effect_liquid_glass_push exceeds device-wide push budget");

#define EFFECT_BLUR_WG 16u
#define EFFECT_BLUR_RADIUS_MAX 64
#define EFFECT_STORAGE_FORMAT FLUX_FORMAT_RGBA8_UNORM

/* ------------------------------------------------------------------ */
/*  Module state                                                      */
/* ------------------------------------------------------------------ */

/* Cached intermediate target keyed by (format, width, height). One
 * intermediate per key is enough — the horizontal pass writes it,
 * the vertical pass reads it, and the two passes are serialised
 * within a single flux_effect_blur call. Callers issuing concurrent
 * blurs on the same device are already required to serialise per
 * the header doc. */
typedef struct intermediate_entry {
    uint32_t width;
    uint32_t height;
    flux_format format;
    flux_image *image;
    bool leased;
    struct intermediate_entry *next;
} intermediate_entry;

/* Intermediate and output slots are exclusively leased within one reset epoch.
 * They are returned to the pool only at a caller-proven GPU quiescent point. */
typedef struct output_entry {
    uint32_t width;
    uint32_t height;
    flux_format format;
    flux_image *image;
    bool leased;
    struct output_entry *next;
} output_entry;

typedef struct effect_state {
    pthread_mutex_t lock;
    flux_compute_pipeline *blur_pipeline;         /* lazily built; shared across calls */
    flux_compute_pipeline *backdrop_pipeline;     /* fixed-cost live-compositor filter */
    flux_compute_pipeline *liquid_glass_pipeline; /* analytic SDF glass composite */
    intermediate_entry *intermediates;
    output_entry *outputs;
} effect_state;

typedef struct blur_filter_slot {
    uint32_t width;
    uint32_t height;
    flux_format format;
    flux_image *half;
    flux_image *quarter;
    flux_image *output;
} blur_filter_slot;

struct flux_blur_filter {
    atomic_uint ref_count;
    flux_device *device; /* retained; slot images hold weak device refs */
    blur_filter_slot slots[FLUX_MAX_FRAMES_IN_FLIGHT];
};

typedef struct liquid_glass_filter_slot {
    uint32_t width;
    uint32_t height;
    flux_image *output;
} liquid_glass_filter_slot;

struct flux_liquid_glass_filter {
    atomic_uint ref_count;
    flux_device *device;
    liquid_glass_filter_slot slots[FLUX_MAX_FRAMES_IN_FLIGHT];
};

static void effect_state_destroy(flux_device *d) {
    effect_state *st = d->effect_state;
    if (!st)
        return;

    for (output_entry *o = st->outputs; o;) {
        output_entry *next = o->next;
        if (o->image)
            flux_image_release(o->image);
        flux_internal_free(d, o);
        o = next;
    }
    for (intermediate_entry *e = st->intermediates; e;) {
        intermediate_entry *next = e->next;
        if (e->image)
            flux_image_release(e->image);
        flux_internal_free(d, e);
        e = next;
    }
    if (st->blur_pipeline)
        flux_compute_pipeline_release(st->blur_pipeline);
    if (st->backdrop_pipeline)
        flux_compute_pipeline_release(st->backdrop_pipeline);
    if (st->liquid_glass_pipeline)
        flux_compute_pipeline_release(st->liquid_glass_pipeline);
    pthread_mutex_destroy(&st->lock);
    flux_internal_free(d, st);
    d->effect_state = nullptr;
    d->effect_state_destroy = nullptr;
}

static effect_state *effect_state_get_or_init(flux_device *d) {
    pthread_mutex_lock(&d->module_state_lock);
    effect_state *published = d->effect_state;
    pthread_mutex_unlock(&d->module_state_lock);
    if (published)
        return published;

    effect_state *candidate = flux_internal_alloc(d, sizeof(*candidate));
    if (!candidate)
        return nullptr;
    if (pthread_mutex_init(&candidate->lock, nullptr) != 0) {
        flux_internal_free(d, candidate);
        return nullptr;
    }

    pthread_mutex_lock(&d->module_state_lock);
    if (!d->effect_state) {
        d->effect_state = candidate;
        d->effect_state_destroy = effect_state_destroy;
        published = candidate;
        candidate = nullptr;
    } else {
        published = d->effect_state;
    }
    pthread_mutex_unlock(&d->module_state_lock);

    if (candidate) {
        pthread_mutex_destroy(&candidate->lock);
        flux_internal_free(d, candidate);
    }
    return published;
}

/* ------------------------------------------------------------------ */
/*  Transient acquisition                                             */
/* ------------------------------------------------------------------ */

/* Look up or allocate the intermediate for this key. Returned image
 * is owned by the pool and lives until reset. */
static flux_result acquire_intermediate(flux_device *d, effect_state *st, uint32_t w, uint32_t h,
                                        flux_format fmt, flux_image **out,
                                        intermediate_entry **out_lease) {
    for (intermediate_entry *e = st->intermediates; e; e = e->next) {
        if (!e->leased && e->width == w && e->height == h && e->format == fmt) {
            e->leased = true;
            *out = e->image;
            *out_lease = e;
            return FLUX_OK;
        }
    }
    flux_image *img = nullptr;
    flux_result r = flux_image_create_compute_writable(d, w, h, fmt, &img);
    if (r != FLUX_OK)
        return r;

    intermediate_entry *e = flux_internal_alloc(d, sizeof(*e));
    if (!e) {
        flux_image_release(img);
        return FLUX_ERROR_OUT_OF_MEMORY;
    }
    e->width = w;
    e->height = h;
    e->format = fmt;
    e->image = img;
    e->leased = true;
    e->next = st->intermediates;
    st->intermediates = e;
    *out = img;
    *out_lease = e;
    return FLUX_OK;
}

/* Lease a same-key output, growing the pool to the epoch high-water mark. */
static flux_result acquire_output(flux_device *d, effect_state *st, uint32_t w, uint32_t h,
                                  flux_format fmt, flux_image **out, output_entry **out_lease) {
    for (output_entry *o = st->outputs; o; o = o->next) {
        if (!o->leased && o->width == w && o->height == h && o->format == fmt) {
            o->leased = true;
            *out = o->image;
            *out_lease = o;
            return FLUX_OK;
        }
    }
    flux_image *img = nullptr;
    flux_result r = flux_image_create_compute_writable(d, w, h, fmt, &img);
    if (r != FLUX_OK)
        return r;

    output_entry *o = flux_internal_alloc(d, sizeof(*o));
    if (!o) {
        flux_image_release(img);
        return FLUX_ERROR_OUT_OF_MEMORY;
    }
    o->width = w;
    o->height = h;
    o->format = fmt;
    o->image = img;
    o->leased = true;
    o->next = st->outputs;
    st->outputs = o;
    *out = img;
    *out_lease = o;
    return FLUX_OK;
}

/* ------------------------------------------------------------------ */
/*  Pipeline build                                                    */
/* ------------------------------------------------------------------ */

static flux_result ensure_blur_pipeline(flux_device *d, effect_state *st) {
    if (st->blur_pipeline)
        return FLUX_OK;
    flux_compute_pipeline_desc pdesc = FLUX_COMPUTE_PIPELINE_DESC_INIT;
    pdesc.spirv = (const uint32_t *)effect_blur_spv;
    pdesc.spirv_word_count = sizeof(effect_blur_spv) / sizeof(uint32_t);
    pdesc.entry_point = "main";
    pdesc.push_constant_bytes = sizeof(effect_blur_push);
    flux_result r = flux_compute_pipeline_create(d, &pdesc, &st->blur_pipeline);
    if (r != FLUX_OK)
        return r;
    /* The pipeline lives in per-device module state and is released
     * by effect_state_destroy inside flux_device_release; a strong
     * device ref would cycle and leak the whole device. */
    flux_compute_pipeline_make_device_weak(st->blur_pipeline);
    return FLUX_OK;
}

static flux_result ensure_backdrop_pipeline(flux_device *d, effect_state *st) {
    if (st->backdrop_pipeline)
        return FLUX_OK;
    flux_compute_pipeline_desc pdesc = FLUX_COMPUTE_PIPELINE_DESC_INIT;
    pdesc.spirv = (const uint32_t *)effect_backdrop_spv;
    pdesc.spirv_word_count = sizeof(effect_backdrop_spv) / sizeof(uint32_t);
    pdesc.entry_point = "main";
    pdesc.push_constant_bytes = sizeof(effect_backdrop_push);
    flux_result r = flux_compute_pipeline_create(d, &pdesc, &st->backdrop_pipeline);
    if (r != FLUX_OK)
        return r;
    flux_compute_pipeline_make_device_weak(st->backdrop_pipeline);
    return FLUX_OK;
}

static flux_result ensure_liquid_glass_pipeline(flux_device *d, effect_state *st) {
    if (st->liquid_glass_pipeline)
        return FLUX_OK;
    flux_compute_pipeline_desc pdesc = FLUX_COMPUTE_PIPELINE_DESC_INIT;
    pdesc.spirv = (const uint32_t *)effect_liquid_glass_spv;
    pdesc.spirv_word_count = sizeof(effect_liquid_glass_spv) / sizeof(uint32_t);
    pdesc.entry_point = "main";
    pdesc.push_constant_bytes = sizeof(effect_liquid_glass_push);
    flux_result r = flux_compute_pipeline_create(d, &pdesc, &st->liquid_glass_pipeline);
    if (r != FLUX_OK)
        return r;
    flux_compute_pipeline_make_device_weak(st->liquid_glass_pipeline);
    return FLUX_OK;
}

/* ------------------------------------------------------------------ */
/*  Barrier helpers                                                   */
/* ------------------------------------------------------------------ */

/* Emit a compute-write → compute-read barrier on `image`, no layout
 * transition (image stays in GENERAL). Used between the two passes
 * and after the final pass. */
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

/* A reusable frame-slot image may have been sampled by the slot's previous
 * submission. begin_frame waited that slot's fence; this barrier expresses
 * the device-side read/write dependency before the next compute overwrite. */
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

static void clear_liquid_glass_output(VkCommandBuffer cmd, VkImage image) {
    VkImageSubresourceRange range = {
        .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
        .levelCount = 1,
        .layerCount = 1,
    };
    VkImageMemoryBarrier2 before = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .srcStageMask =
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
        .srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_2_CLEAR_BIT,
        .dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
        .newLayout = VK_IMAGE_LAYOUT_GENERAL,
        .image = image,
        .subresourceRange = range,
    };
    VkDependencyInfo before_info = {
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .imageMemoryBarrierCount = 1,
        .pImageMemoryBarriers = &before,
    };
    vkCmdPipelineBarrier2(cmd, &before_info);
    VkClearColorValue transparent = {.float32 = {0.0f, 0.0f, 0.0f, 0.0f}};
    vkCmdClearColorImage(cmd, image, VK_IMAGE_LAYOUT_GENERAL, &transparent, 1, &range);

    VkImageMemoryBarrier2 after = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .srcStageMask = VK_PIPELINE_STAGE_2_CLEAR_BIT,
        .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        .dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
        .newLayout = VK_IMAGE_LAYOUT_GENERAL,
        .image = image,
        .subresourceRange = range,
    };
    VkDependencyInfo after_info = {
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .imageMemoryBarrierCount = 1,
        .pImageMemoryBarriers = &after,
    };
    vkCmdPipelineBarrier2(cmd, &after_info);
}

static void barrier_compute_write_to_write(VkCommandBuffer cmd, VkImage image) {
    VkImageMemoryBarrier2 barrier = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        .srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
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
    VkDependencyInfo info = {
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .imageMemoryBarrierCount = 1,
        .pImageMemoryBarriers = &barrier,
    };
    vkCmdPipelineBarrier2(cmd, &info);
}

static void barrier_clear_to_fragment_read(VkCommandBuffer cmd, VkImage image) {
    VkImageMemoryBarrier2 barrier = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .srcStageMask = VK_PIPELINE_STAGE_2_CLEAR_BIT,
        .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
        .dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
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

static flux_result record_blur_dispatch(effect_state *st, flux_device *d, VkCommandBuffer cmd,
                                        flux_image *in, flux_image *intermediate,
                                        flux_image *output, float requested_sigma) {
    float sigma = requested_sigma;
    if (!(sigma == sigma) || sigma < 0.0f)
        sigma = 0.0f;
    if (sigma > FLUX_EFFECT_BLUR_SIGMA_MAX)
        sigma = FLUX_EFFECT_BLUR_SIGMA_MAX;
    int32_t radius = (int32_t)ceilf(3.0f * sigma);
    if (radius > EFFECT_BLUR_RADIUS_MAX)
        radius = EFFECT_BLUR_RADIUS_MAX;

    barrier_reuse_to_compute_write(cmd, intermediate->image);
    barrier_reuse_to_compute_write(cmd, output->image);

    flux_bindless_handle sampler_h = flux_device_default_sampler_handle(d);
    effect_blur_push pc = {
        .in_handle = in->bindless,
        .sampler_handle = sampler_h,
        .out_handle = intermediate->bindless_storage,
        .width = in->width,
        .height = in->height,
        .radius = radius,
        .sigma = (sigma <= 0.0f) ? 1.0f : sigma,
        .axis = 0u,
    };
    uint32_t gx = (in->width + EFFECT_BLUR_WG - 1) / EFFECT_BLUR_WG;
    uint32_t gy = (in->height + EFFECT_BLUR_WG - 1) / EFFECT_BLUR_WG;
    flux_compute_dispatch(cmd, st->blur_pipeline, &pc, sizeof(pc), gx, gy, 1);

    barrier_compute_write_to_read(cmd, intermediate->image);

    pc.in_handle = intermediate->bindless;
    pc.out_handle = output->bindless_storage;
    pc.axis = 1u;
    flux_compute_dispatch(cmd, st->blur_pipeline, &pc, sizeof(pc), gx, gy, 1);

    barrier_compute_write_to_read(cmd, output->image);
    return FLUX_OK;
}

/* ------------------------------------------------------------------ */
/*  Public: flux_effect_blur                                          */
/* ------------------------------------------------------------------ */

flux_result flux_effect_blur(VkCommandBuffer cmd, const flux_effect_blur_desc *desc,
                             flux_image **out) {
    if (!cmd || !desc || !out)
        return FLUX_ERROR_INVALID_ARGUMENT;
    if (desc->type != FLUX_TYPE_EFFECT_BLUR_DESC) {
        FLUX_FAIL(FLUX_ERROR_INVALID_ARGUMENT, "desc->type != FLUX_TYPE_EFFECT_BLUR_DESC");
        return FLUX_ERROR_INVALID_ARGUMENT;
    }
    if (!desc->input) {
        FLUX_FAIL(FLUX_ERROR_INVALID_ARGUMENT, "blur input is null");
        return FLUX_ERROR_INVALID_ARGUMENT;
    }
    flux_image *in = desc->input;
    flux_device *d = in->device;
    /* Only RGBA8_UNORM / BGRA8_UNORM are storage-image-compatible
     * by spec without optional features. Other formats need a real
     * storage-format query before we promise support; reject for
     * now and revisit when a real consumer needs it. */
    if (in->format != FLUX_FORMAT_RGBA8_UNORM && in->format != FLUX_FORMAT_BGRA8_UNORM) {
        FLUX_FAIL(FLUX_ERROR_UNSUPPORTED,
                  "blur input format not supported (use RGBA8_UNORM or BGRA8_UNORM)");
        return FLUX_ERROR_UNSUPPORTED;
    }
    if (in->bindless == FLUX_BINDLESS_INVALID) {
        FLUX_FAIL(FLUX_ERROR_INVALID_ARGUMENT, "blur input lacks sampled bindless handle");
        return FLUX_ERROR_INVALID_ARGUMENT;
    }
    *out = nullptr;

    effect_state *st = effect_state_get_or_init(d);
    if (!st)
        return FLUX_ERROR_OUT_OF_MEMORY;

    pthread_mutex_lock(&st->lock);

    flux_result r = ensure_blur_pipeline(d, st);
    if (r != FLUX_OK) {
        pthread_mutex_unlock(&st->lock);
        return r;
    }

    flux_image *intermediate = nullptr;
    intermediate_entry *intermediate_lease = nullptr;
    r = acquire_intermediate(d, st, in->width, in->height, EFFECT_STORAGE_FORMAT, &intermediate,
                             &intermediate_lease);
    if (r != FLUX_OK) {
        pthread_mutex_unlock(&st->lock);
        return r;
    }

    flux_image *output = nullptr;
    output_entry *output_lease = nullptr;
    r = acquire_output(d, st, in->width, in->height, EFFECT_STORAGE_FORMAT, &output, &output_lease);
    if (r != FLUX_OK) {
        intermediate_lease->leased = false;
        pthread_mutex_unlock(&st->lock);
        return r;
    }
    (void)output_lease;

    pthread_mutex_unlock(&st->lock);

    r = record_blur_dispatch(st, d, cmd, in, intermediate, output, desc->sigma);
    if (r != FLUX_OK)
        return r;
    *out = output;
    return FLUX_OK;
}

/* ------------------------------------------------------------------ */
/*  Reusable frame-slot realtime blur                                 */
/* ------------------------------------------------------------------ */

flux_result flux_blur_filter_create(flux_device *device, flux_blur_filter **out) {
    if (!device || !out)
        return FLUX_ERROR_INVALID_ARGUMENT;
    *out = nullptr;
    flux_blur_filter *filter = flux_internal_alloc(device, sizeof(*filter));
    if (!filter)
        return FLUX_ERROR_OUT_OF_MEMORY;
    atomic_init(&filter->ref_count, 1u);
    filter->device = flux_device_retain(device);
    *out = filter;
    return FLUX_OK;
}

flux_blur_filter *flux_blur_filter_retain(flux_blur_filter *filter) {
    if (filter)
        atomic_fetch_add_explicit(&filter->ref_count, 1u, memory_order_relaxed);
    return filter;
}

void flux_blur_filter_release(flux_blur_filter *filter) {
    if (!filter)
        return;
    if (atomic_fetch_sub_explicit(&filter->ref_count, 1u, memory_order_acq_rel) != 1u)
        return;
    flux_device *device = filter->device;
    for (uint32_t i = 0; i < FLUX_MAX_FRAMES_IN_FLIGHT; ++i) {
        if (filter->slots[i].output)
            flux_image_release(filter->slots[i].output);
        if (filter->slots[i].quarter)
            flux_image_release(filter->slots[i].quarter);
        if (filter->slots[i].half)
            flux_image_release(filter->slots[i].half);
    }
    flux_internal_free(device, filter);
    flux_device_release(device);
}

static flux_result blur_filter_ensure_slot(flux_blur_filter *filter, uint32_t index,
                                           const flux_image *input) {
    blur_filter_slot *slot = &filter->slots[index];
    if (slot->half && slot->quarter && slot->output && slot->width == input->width &&
        slot->height == input->height && slot->format == input->format)
        return FLUX_OK;

    if (slot->output)
        flux_image_release(slot->output);
    if (slot->quarter)
        flux_image_release(slot->quarter);
    if (slot->half)
        flux_image_release(slot->half);
    *slot = (blur_filter_slot){0};

    uint32_t half_width = (input->width + 1u) / 2u;
    uint32_t half_height = (input->height + 1u) / 2u;
    uint32_t quarter_width = (half_width + 1u) / 2u;
    uint32_t quarter_height = (half_height + 1u) / 2u;
    flux_result r = flux_image_create_compute_writable(filter->device, half_width, half_height,
                                                       EFFECT_STORAGE_FORMAT, &slot->half);
    if (r != FLUX_OK)
        return r;
    r = flux_image_create_compute_writable(filter->device, quarter_width, quarter_height,
                                           EFFECT_STORAGE_FORMAT, &slot->quarter);
    if (r != FLUX_OK)
        goto fail;
    r = flux_image_create_compute_writable(filter->device, input->width, input->height,
                                           EFFECT_STORAGE_FORMAT, &slot->output);
    if (r != FLUX_OK)
        goto fail;
    slot->width = input->width;
    slot->height = input->height;
    slot->format = input->format;
    return FLUX_OK;

fail:
    if (slot->quarter)
        flux_image_release(slot->quarter);
    if (slot->half)
        flux_image_release(slot->half);
    *slot = (blur_filter_slot){0};
    return r;
}

static void record_backdrop_pass(effect_state *st, flux_device *device, VkCommandBuffer cmd,
                                 flux_image *input, flux_image *output, float offset,
                                 uint32_t mode) {
    barrier_reuse_to_compute_write(cmd, output->image);
    effect_backdrop_push pc = {
        .in_handle = input->bindless,
        .sampler_handle = flux_device_default_sampler_handle(device),
        .out_handle = output->bindless_storage,
        .input_width = input->width,
        .input_height = input->height,
        .output_width = output->width,
        .output_height = output->height,
        .offset = offset,
        .mode = mode,
    };
    uint32_t gx = (output->width + EFFECT_BLUR_WG - 1u) / EFFECT_BLUR_WG;
    uint32_t gy = (output->height + EFFECT_BLUR_WG - 1u) / EFFECT_BLUR_WG;
    flux_compute_dispatch(cmd, st->backdrop_pipeline, &pc, sizeof(pc), gx, gy, 1u);
    barrier_compute_write_to_read(cmd, output->image);
}

static void record_backdrop_filter(effect_state *st, flux_device *device, VkCommandBuffer cmd,
                                   flux_image *input, blur_filter_slot *slot,
                                   float requested_sigma) {
    float sigma = requested_sigma;
    if (!(sigma == sigma) || sigma < 0.0f)
        sigma = 0.0f;
    if (sigma > FLUX_EFFECT_BLUR_SIGMA_MAX)
        sigma = FLUX_EFFECT_BLUR_SIGMA_MAX;

    if (sigma == 0.0f) {
        record_backdrop_pass(st, device, cmd, input, slot->output, 0.0f, 2u);
        return;
    }

    /* Two pyramid levels provide a wide UI blur with fixed work. Sigma tunes
     * the sub-texel offsets rather than growing a per-pixel kernel loop. */
    float offset = fminf(fmaxf(sigma * 0.25f, 0.5f), 2.5f);
    record_backdrop_pass(st, device, cmd, input, slot->half, offset, 0u);
    record_backdrop_pass(st, device, cmd, slot->half, slot->quarter, offset, 0u);
    record_backdrop_pass(st, device, cmd, slot->quarter, slot->half, offset, 1u);
    record_backdrop_pass(st, device, cmd, slot->half, slot->output, offset, 1u);
}

flux_result flux_blur_filter_apply(flux_blur_filter *filter, flux_frame *frame,
                                   const flux_effect_blur_desc *desc, flux_image **out) {
    if (!filter || !frame || !desc || !out)
        return FLUX_ERROR_INVALID_ARGUMENT;
    *out = nullptr;
    if (desc->type != FLUX_TYPE_EFFECT_BLUR_DESC || !desc->input) {
        FLUX_FAIL(FLUX_ERROR_INVALID_ARGUMENT, "invalid reusable blur descriptor");
        return FLUX_ERROR_INVALID_ARGUMENT;
    }
    if (!frame->surface || frame->state != FLUX_FRAME_STATE_RECORDING || frame->pass_active) {
        FLUX_FAIL(FLUX_ERROR_INVALID_STATE,
                  "reusable blur requires a recording frame pass boundary");
        return FLUX_ERROR_INVALID_STATE;
    }
    flux_image *input = desc->input;
    if (frame->surface->device != filter->device || input->device != filter->device) {
        FLUX_FAIL(FLUX_ERROR_INVALID_ARGUMENT,
                  "blur filter, frame, and input use different devices");
        return FLUX_ERROR_INVALID_ARGUMENT;
    }
    if ((input->format != FLUX_FORMAT_RGBA8_UNORM && input->format != FLUX_FORMAT_BGRA8_UNORM) ||
        input->bindless == FLUX_BINDLESS_INVALID) {
        FLUX_FAIL(FLUX_ERROR_UNSUPPORTED, "reusable blur input must be sampled RGBA8/BGRA8 UNORM");
        return FLUX_ERROR_UNSUPPORTED;
    }
    uint32_t index = flux_frame_index(frame);
    if (index >= FLUX_MAX_FRAMES_IN_FLIGHT)
        return FLUX_ERROR_OUT_OF_RANGE;

    flux_result r = blur_filter_ensure_slot(filter, index, input);
    if (r != FLUX_OK)
        return r;

    effect_state *st = effect_state_get_or_init(filter->device);
    if (!st)
        return FLUX_ERROR_OUT_OF_MEMORY;
    pthread_mutex_lock(&st->lock);
    r = ensure_backdrop_pipeline(filter->device, st);
    pthread_mutex_unlock(&st->lock);
    if (r != FLUX_OK)
        return r;

    blur_filter_slot *slot = &filter->slots[index];
    record_backdrop_filter(st, filter->device, flux_frame_vk_command_buffer(frame), input, slot,
                           desc->sigma);
    *out = slot->output;
    return FLUX_OK;
}

/* ------------------------------------------------------------------ */
/*  Reusable analytic liquid glass                                    */
/* ------------------------------------------------------------------ */

flux_result flux_liquid_glass_filter_create(flux_device *device, flux_liquid_glass_filter **out) {
    if (!device || !out)
        return FLUX_ERROR_INVALID_ARGUMENT;
    *out = nullptr;
    flux_liquid_glass_filter *filter = flux_internal_alloc(device, sizeof(*filter));
    if (!filter)
        return FLUX_ERROR_OUT_OF_MEMORY;
    atomic_init(&filter->ref_count, 1u);
    filter->device = flux_device_retain(device);
    *out = filter;
    return FLUX_OK;
}

flux_liquid_glass_filter *flux_liquid_glass_filter_retain(flux_liquid_glass_filter *filter) {
    if (filter)
        atomic_fetch_add_explicit(&filter->ref_count, 1u, memory_order_relaxed);
    return filter;
}

void flux_liquid_glass_filter_release(flux_liquid_glass_filter *filter) {
    if (!filter)
        return;
    if (atomic_fetch_sub_explicit(&filter->ref_count, 1u, memory_order_acq_rel) != 1u)
        return;
    flux_device *device = filter->device;
    for (uint32_t i = 0; i < FLUX_MAX_FRAMES_IN_FLIGHT; ++i) {
        if (filter->slots[i].output)
            flux_image_release(filter->slots[i].output);
    }
    flux_internal_free(device, filter);
    flux_device_release(device);
}

static flux_result liquid_glass_ensure_slot(flux_liquid_glass_filter *filter, uint32_t index,
                                            const flux_image *input) {
    liquid_glass_filter_slot *slot = &filter->slots[index];
    if (slot->output && slot->width == input->width && slot->height == input->height)
        return FLUX_OK;
    if (slot->output)
        flux_image_release(slot->output);
    *slot = (liquid_glass_filter_slot){0};
    flux_result r = flux_image_create_compute_writable(filter->device, input->width, input->height,
                                                       EFFECT_STORAGE_FORMAT, &slot->output);
    if (r != FLUX_OK)
        return r;
    slot->width = input->width;
    slot->height = input->height;
    return FLUX_OK;
}

static bool finite_rect(flux_rect rect) {
    return isfinite(rect.x) && isfinite(rect.y) && isfinite(rect.w) && isfinite(rect.h) &&
           rect.w > 0.0f && rect.h > 0.0f;
}

static bool valid_liquid_glass_desc(const flux_liquid_glass_desc *desc) {
    if (desc->type != FLUX_TYPE_LIQUID_GLASS_DESC || !desc->input || !desc->blurred_input ||
        !desc->groups || desc->group_count == 0u || desc->group_count > 64u)
        return false;
    if (!isfinite(desc->refraction) || !isfinite(desc->chromatic_aberration) ||
        !isfinite(desc->saturation) || !isfinite(desc->brightness) || !isfinite(desc->edge_width) ||
        !isfinite(desc->glare) || !isfinite(desc->light_direction.x) ||
        !isfinite(desc->light_direction.y) || !isfinite(desc->opacity) ||
        !isfinite(desc->size_reference) || !isfinite(desc->size_scale_min) ||
        !isfinite(desc->tint_strength) || !isfinite(desc->frost_strength))
        return false;
    for (uint32_t i = 0; i < desc->group_count; ++i) {
        const flux_liquid_glass_group *group = &desc->groups[i];
        if (group->shape_count < 1u || group->shape_count > 2u || !isfinite(group->blend_radius) ||
            !isfinite(group->opacity) || !isfinite(group->shadow_alpha) ||
            !isfinite(group->shadow_blur) || !isfinite(group->shadow_offset_y))
            return false;
        for (uint32_t j = 0; j < group->shape_count; ++j) {
            if (!finite_rect(group->shapes[j].bounds) || !isfinite(group->shapes[j].corner_radius))
                return false;
        }
    }
    return true;
}

static void copy_shape(float out[4], flux_liquid_glass_shape shape) {
    out[0] = shape.bounds.x;
    out[1] = shape.bounds.y;
    out[2] = shape.bounds.w;
    out[3] = shape.bounds.h;
}

static bool liquid_glass_group_dispatch_bounds(const flux_liquid_glass_group *group,
                                               float shadow_reach, uint32_t image_width,
                                               uint32_t image_height, uint32_t *out_x,
                                               uint32_t *out_y, uint32_t *out_width,
                                               uint32_t *out_height) {
    float x0 = group->shapes[0].bounds.x;
    float y0 = group->shapes[0].bounds.y;
    float x1 = x0 + group->shapes[0].bounds.w;
    float y1 = y0 + group->shapes[0].bounds.h;
    if (group->shape_count == 2u) {
        flux_rect second = group->shapes[1].bounds;
        x0 = fminf(x0, second.x);
        y0 = fminf(y0, second.y);
        x1 = fmaxf(x1, second.x + second.w);
        y1 = fmaxf(y1, second.y + second.h);
    }
    /* Smooth union may bow beyond the source SDFs, and the drop shadow
     * reaches shadow_reach pixels further out. Two extra pixels cover
     * analytic antialiasing. */
    float pad = fmaxf(fmaxf(group->blend_radius, 0.0f), fmaxf(shadow_reach, 0.0f)) + 2.0f;
    int64_t ix0 = (int64_t)floorf(x0 - pad);
    int64_t iy0 = (int64_t)floorf(y0 - pad);
    int64_t ix1 = (int64_t)ceilf(x1 + pad);
    int64_t iy1 = (int64_t)ceilf(y1 + pad);
    ix0 = ix0 < 0 ? 0 : ix0;
    iy0 = iy0 < 0 ? 0 : iy0;
    ix1 = ix1 > (int64_t)image_width ? image_width : ix1;
    iy1 = iy1 > (int64_t)image_height ? image_height : iy1;
    if (ix1 <= ix0 || iy1 <= iy0)
        return false;
    *out_x = (uint32_t)ix0;
    *out_y = (uint32_t)iy0;
    *out_width = (uint32_t)(ix1 - ix0);
    *out_height = (uint32_t)(iy1 - iy0);
    return true;
}

flux_result flux_liquid_glass_filter_apply(flux_liquid_glass_filter *filter, flux_frame *frame,
                                           const flux_liquid_glass_desc *desc, flux_image **out) {
    if (!filter || !frame || !desc || !out)
        return FLUX_ERROR_INVALID_ARGUMENT;
    *out = nullptr;
    if (!valid_liquid_glass_desc(desc)) {
        FLUX_FAIL(FLUX_ERROR_INVALID_ARGUMENT, "invalid liquid glass descriptor");
        return FLUX_ERROR_INVALID_ARGUMENT;
    }
    if (!frame->surface || frame->state != FLUX_FRAME_STATE_RECORDING || frame->pass_active) {
        FLUX_FAIL(FLUX_ERROR_INVALID_STATE,
                  "liquid glass requires a recording frame pass boundary");
        return FLUX_ERROR_INVALID_STATE;
    }
    flux_image *input = desc->input;
    flux_image *blurred = desc->blurred_input;
    if (frame->surface->device != filter->device || input->device != filter->device ||
        blurred->device != filter->device) {
        FLUX_FAIL(FLUX_ERROR_INVALID_ARGUMENT,
                  "liquid glass filter, frame, and inputs use different devices");
        return FLUX_ERROR_INVALID_ARGUMENT;
    }
    if (input->width != blurred->width || input->height != blurred->height ||
        (input->format != FLUX_FORMAT_RGBA8_UNORM && input->format != FLUX_FORMAT_BGRA8_UNORM) ||
        blurred->format != FLUX_FORMAT_RGBA8_UNORM || input->bindless == FLUX_BINDLESS_INVALID ||
        blurred->bindless == FLUX_BINDLESS_INVALID) {
        FLUX_FAIL(FLUX_ERROR_UNSUPPORTED,
                  "liquid glass inputs must be same-size sampled RGBA8/BGRA8 images");
        return FLUX_ERROR_UNSUPPORTED;
    }
    uint32_t index = flux_frame_index(frame);
    if (index >= FLUX_MAX_FRAMES_IN_FLIGHT)
        return FLUX_ERROR_OUT_OF_RANGE;
    flux_result result = liquid_glass_ensure_slot(filter, index, input);
    if (result != FLUX_OK)
        return result;

    effect_state *state = effect_state_get_or_init(filter->device);
    if (!state)
        return FLUX_ERROR_OUT_OF_MEMORY;
    pthread_mutex_lock(&state->lock);
    result = ensure_liquid_glass_pipeline(filter->device, state);
    pthread_mutex_unlock(&state->lock);
    if (result != FLUX_OK)
        return result;

    liquid_glass_filter_slot *slot = &filter->slots[index];
    VkCommandBuffer command = flux_frame_vk_command_buffer(frame);
    clear_liquid_glass_output(command, slot->output->image);

    bool dispatched = false;
    for (uint32_t i = 0; i < desc->group_count; ++i) {
        const flux_liquid_glass_group *group = &desc->groups[i];
        /* The body's shadow reaches its downward offset plus the falloff. */
        float shadow_reach =
            group->shadow_alpha > 0.0f ? fmaxf(group->shadow_offset_y, 0.0f) +
                                             2.0f * fmaxf(group->shadow_blur, 0.0f)
                                       : 0.0f;
        uint32_t origin_x = 0, origin_y = 0, group_width = 0, group_height = 0;
        if (!liquid_glass_group_dispatch_bounds(group, shadow_reach, input->width, input->height,
                                                &origin_x, &origin_y, &group_width, &group_height))
            continue;
        if (dispatched)
            barrier_compute_write_to_write(command, slot->output->image);
        effect_liquid_glass_push push = {
            .input_handle = input->bindless,
            .blurred_handle = blurred->bindless,
            .sampler_handle = flux_device_default_sampler_handle(filter->device),
            .output_handle = slot->output->bindless_storage,
            .width = input->width,
            .height = input->height,
            .origin_x = origin_x,
            .origin_y = origin_y,
            .radius0 = fmaxf(group->shapes[0].corner_radius, 0.0f),
            .radius1 =
                group->shape_count == 2u ? fmaxf(group->shapes[1].corner_radius, 0.0f) : 0.0f,
            .blend_radius = fmaxf(group->blend_radius, 0.0f),
            .opacity = fminf(fmaxf(group->opacity * desc->opacity, 0.0f), 1.0f),
            .refraction = fmaxf(desc->refraction, 0.0f),
            .chromatic_aberration = fmaxf(desc->chromatic_aberration, 0.0f),
            .saturation = fmaxf(desc->saturation, 0.0f),
            .brightness = fmaxf(desc->brightness, 0.0f),
            .edge_width = fmaxf(desc->edge_width, 1.0f),
            .glare = fmaxf(desc->glare, 0.0f),
            .light_x = desc->light_direction.x,
            .light_y = desc->light_direction.y,
            .group_width = group_width,
            .group_height = group_height,
            .shape_count = group->shape_count,
            .shadow_alpha = fminf(fmaxf(group->shadow_alpha, 0.0f), 1.0f),
            .shadow_blur = fmaxf(group->shadow_blur, 0.0f),
            .shadow_offset_y = group->shadow_offset_y,
            .size_reference = fmaxf(desc->size_reference, 0.0f),
            .size_scale_min = fminf(fmaxf(desc->size_scale_min, 0.0f), 1.0f),
            .tint_strength = fmaxf(desc->tint_strength, 0.0f),
            .frost_strength = fmaxf(desc->frost_strength, 0.0f),
            .tint_color = group->tint_color,
        };
        copy_shape(push.shape0, group->shapes[0]);
        if (group->shape_count == 2u)
            copy_shape(push.shape1, group->shapes[1]);
        uint32_t gx = (group_width + EFFECT_BLUR_WG - 1u) / EFFECT_BLUR_WG;
        uint32_t gy = (group_height + EFFECT_BLUR_WG - 1u) / EFFECT_BLUR_WG;
        flux_compute_dispatch(command, state->liquid_glass_pipeline, &push, sizeof(push), gx, gy,
                              1u);
        dispatched = true;
    }
    /* Clear-only output is still a transfer write; normally at least one
     * clipped group dispatched and this covers compute -> fragment sampling. */
    if (dispatched) {
        barrier_compute_write_to_read(command, slot->output->image);
    } else {
        barrier_clear_to_fragment_read(command, slot->output->image);
    }
    *out = slot->output;
    return FLUX_OK;
}

/* ------------------------------------------------------------------ */
/*  Public: flux_effect_promote                                       */
/* ------------------------------------------------------------------ */

static flux_result promote_copy_submit(flux_device *d, VkImage src_image, VkImage dst_image,
                                       uint32_t width, uint32_t height) {
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkResult vr = flux_vk_new_transient_cmd(d, d->graphics_family, &pool, &cmd);
    if (vr != VK_SUCCESS) {
        FLUX_FAIL_VK(FLUX_ERROR_BACKEND_FAILURE, "promote command buffer allocation failed", vr);
        return FLUX_ERROR_BACKEND_FAILURE;
    }

    VkImageSubresourceRange subres = {
        .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
        .levelCount = 1,
        .layerCount = 1,
    };

    VkImageMemoryBarrier2 pre[2] = {
        /* src: GENERAL → TRANSFER_SRC */
        {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
            .srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            .srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
            .dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT,
            .dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
            .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            .image = src_image,
            .subresourceRange = subres,
        },
        /* dst: SHADER_READ_ONLY (flux_image_create's terminal layout) → TRANSFER_DST */
        {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
            .srcStageMask = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT,
            .srcAccessMask = 0,
            .dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT,
            .dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            .image = dst_image,
            .subresourceRange = subres,
        },
    };
    VkDependencyInfo pre_di = {
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .imageMemoryBarrierCount = 2,
        .pImageMemoryBarriers = pre,
    };
    vkCmdPipelineBarrier2(cmd, &pre_di);

    VkImageCopy region = {
        .srcSubresource = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .layerCount = 1},
        .dstSubresource = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .layerCount = 1},
        .extent = {width, height, 1},
    };
    vkCmdCopyImage(cmd, src_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst_image,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    VkImageMemoryBarrier2 post[2] = {
        /* src: TRANSFER_SRC → GENERAL (restore for future effect re-use) */
        {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
            .srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT,
            .srcAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT,
            .dstStageMask = VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT,
            .dstAccessMask = 0,
            .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            .newLayout = VK_IMAGE_LAYOUT_GENERAL,
            .image = src_image,
            .subresourceRange = subres,
        },
        /* dst: TRANSFER_DST → SHADER_READ_ONLY (caller will sample it) */
        {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
            .srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT,
            .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
            .dstStageMask =
                VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            .dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            .image = dst_image,
            .subresourceRange = subres,
        },
    };
    VkDependencyInfo post_di = {
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .imageMemoryBarrierCount = 2,
        .pImageMemoryBarriers = post,
    };
    vkCmdPipelineBarrier2(cmd, &post_di);

    vr = vkEndCommandBuffer(cmd);
    if (vr == VK_SUCCESS)
        vr = flux_vk_submit_one_shot_and_wait(d, cmd);
    vkDestroyCommandPool(d->device, pool, nullptr);

    if (vr != VK_SUCCESS) {
        flux_result r = vr == VK_TIMEOUT             ? FLUX_ERROR_TIMEOUT
                        : vr == VK_ERROR_DEVICE_LOST ? FLUX_ERROR_DEVICE_LOST
                                                     : FLUX_ERROR_BACKEND_FAILURE;
        FLUX_FAIL_VK(r, "promote copy submit failed", vr);
        return r;
    }
    return FLUX_OK;
}

flux_result flux_effect_promote(flux_image *transient, flux_image **out) {
    if (!transient || !out)
        return FLUX_ERROR_INVALID_ARGUMENT;
    if (transient->bindless_storage == FLUX_BINDLESS_INVALID) {
        FLUX_FAIL(FLUX_ERROR_INVALID_ARGUMENT,
                  "flux_effect_promote source is not an effect output (no storage handle)");
        return FLUX_ERROR_INVALID_ARGUMENT;
    }
    *out = nullptr;

    flux_device *d = transient->device;
    flux_image_desc ddesc = FLUX_IMAGE_DESC_INIT;
    ddesc.width = transient->width;
    ddesc.height = transient->height;
    ddesc.format = transient->format;
    flux_image *dst = nullptr;
    flux_result r = flux_image_create(d, &ddesc, &dst);
    if (r != FLUX_OK)
        return r;

    r = promote_copy_submit(d, transient->image, dst->image, transient->width, transient->height);
    if (r != FLUX_OK) {
        flux_image_release(dst);
        return r;
    }
    *out = dst;
    return FLUX_OK;
}

/* ------------------------------------------------------------------ */
/*  Public: flux_effect_reset                                         */
/* ------------------------------------------------------------------ */

void flux_effect_reset(flux_device *d) {
    if (!d)
        return;
    pthread_mutex_lock(&d->module_state_lock);
    effect_state *st = d->effect_state;
    pthread_mutex_unlock(&d->module_state_lock);
    if (!st)
        return;
    pthread_mutex_lock(&st->lock);
    for (output_entry *o = st->outputs; o; o = o->next)
        o->leased = false;
    for (intermediate_entry *e = st->intermediates; e; e = e->next)
        e->leased = false;
    pthread_mutex_unlock(&st->lock);
}
