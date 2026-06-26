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
 *   - flux_effect_blur entry point
 */

#include "../canvas/image_internal.h" /* struct flux_image + create_compute_writable */
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

static_assert(sizeof(effect_blur_push) <= FLUX_DEVICE_REQUIRED_PUSH_BYTES,
              "effect_blur_push exceeds device-wide push budget");

#define EFFECT_BLUR_WG 16u
#define EFFECT_BLUR_RADIUS_MAX 64

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
    struct intermediate_entry *next;
} intermediate_entry;

/* Output transients are appended on every blur call and released
 * en masse on flux_effect_reset. The list grows to the high-water
 * mark of concurrent live outputs; callers reset between frames
 * to recycle. */
typedef struct output_entry {
    flux_image *image;
    struct output_entry *next;
} output_entry;

typedef struct effect_state {
    pthread_mutex_t lock;
    flux_compute_pipeline *blur_pipeline; /* lazily built; shared across calls */
    intermediate_entry *intermediates;
    output_entry *outputs;
} effect_state;

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
    pthread_mutex_destroy(&st->lock);
    flux_internal_free(d, st);
    d->effect_state = nullptr;
    d->effect_state_destroy = nullptr;
}

static effect_state *effect_state_get_or_init(flux_device *d) {
    if (d->effect_state)
        return d->effect_state;
    effect_state *st = flux_internal_alloc(d, sizeof(*st));
    if (!st)
        return nullptr;
    pthread_mutex_init(&st->lock, nullptr);
    d->effect_state = st;
    d->effect_state_destroy = effect_state_destroy;
    return st;
}

/* ------------------------------------------------------------------ */
/*  Transient acquisition                                             */
/* ------------------------------------------------------------------ */

/* Look up or allocate the intermediate for this key. Returned image
 * is owned by the pool and lives until reset. */
static flux_result acquire_intermediate(flux_device *d, effect_state *st, uint32_t w, uint32_t h,
                                        flux_format fmt, flux_image **out) {
    for (intermediate_entry *e = st->intermediates; e; e = e->next) {
        if (e->width == w && e->height == h && e->format == fmt) {
            *out = e->image;
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
    e->next = st->intermediates;
    st->intermediates = e;
    *out = img;
    return FLUX_OK;
}

/* Allocate a fresh output transient and link it into the pool. */
static flux_result acquire_output(flux_device *d, effect_state *st, uint32_t w, uint32_t h,
                                  flux_format fmt, flux_image **out) {
    flux_image *img = nullptr;
    flux_result r = flux_image_create_compute_writable(d, w, h, fmt, &img);
    if (r != FLUX_OK)
        return r;

    output_entry *o = flux_internal_alloc(d, sizeof(*o));
    if (!o) {
        flux_image_release(img);
        return FLUX_ERROR_OUT_OF_MEMORY;
    }
    o->image = img;
    o->next = st->outputs;
    st->outputs = o;
    *out = img;
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

    /* Clamp sigma to the documented range. radius derived as
     * ceil(3 * sigma); the 3-sigma cutoff captures > 99.7% of the
     * Gaussian's mass. Cap at EFFECT_BLUR_RADIUS_MAX so the
     * per-pixel loop has a known upper bound for any caller. */
    float sigma = desc->sigma;
    if (!(sigma == sigma) || sigma < 0.0f)
        sigma = 0.0f; /* NaN or negative → 0 */
    if (sigma > FLUX_EFFECT_BLUR_SIGMA_MAX)
        sigma = FLUX_EFFECT_BLUR_SIGMA_MAX;
    int32_t radius = (int32_t)ceilf(3.0f * sigma);
    if (radius > EFFECT_BLUR_RADIUS_MAX)
        radius = EFFECT_BLUR_RADIUS_MAX;

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
    r = acquire_intermediate(d, st, in->width, in->height, in->format, &intermediate);
    if (r != FLUX_OK) {
        pthread_mutex_unlock(&st->lock);
        return r;
    }

    flux_image *output = nullptr;
    r = acquire_output(d, st, in->width, in->height, in->format, &output);
    if (r != FLUX_OK) {
        pthread_mutex_unlock(&st->lock);
        return r;
    }

    pthread_mutex_unlock(&st->lock);

    flux_bindless_handle sampler_h = flux_device_default_sampler_handle(d);

    /* Pass 1: horizontal. Reads input (sampled), writes intermediate. */
    effect_blur_push pc = {
        .in_handle = in->bindless,
        .sampler_handle = sampler_h,
        .out_handle = intermediate->bindless_storage,
        .width = in->width,
        .height = in->height,
        .radius = radius,
        .sigma = (sigma <= 0.0f) ? 1.0f : sigma, /* harmless when radius==0 */
        .axis = 0u,
    };
    uint32_t gx = (in->width + EFFECT_BLUR_WG - 1) / EFFECT_BLUR_WG;
    uint32_t gy = (in->height + EFFECT_BLUR_WG - 1) / EFFECT_BLUR_WG;
    flux_compute_dispatch(cmd, st->blur_pipeline, &pc, sizeof(pc), gx, gy, 1);

    /* The intermediate was just written. Make those writes visible to
     * the next dispatch's sampled reads of the same image. */
    barrier_compute_write_to_read(cmd, intermediate->image);

    /* Pass 2: vertical. Reads intermediate (sampled), writes output. */
    pc.in_handle = intermediate->bindless;
    pc.out_handle = output->bindless_storage;
    pc.axis = 1u;
    flux_compute_dispatch(cmd, st->blur_pipeline, &pc, sizeof(pc), gx, gy, 1);

    /* Output is now written. Make those writes visible to whoever
     * samples it next (canvas draw, another effect, the host on
     * read-back, ...). */
    barrier_compute_write_to_read(cmd, output->image);

    *out = output;
    return FLUX_OK;
}

/* ------------------------------------------------------------------ */
/*  Public: flux_effect_promote                                       */
/* ------------------------------------------------------------------ */

static flux_result promote_copy_submit(flux_device *d, VkImage src_image, VkImage dst_image,
                                       uint32_t width, uint32_t height) {
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandPoolCreateInfo pci = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .queueFamilyIndex = d->graphics_family,
        .flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT,
    };
    if (vkCreateCommandPool(d->device, &pci, nullptr, &pool) != VK_SUCCESS)
        return FLUX_ERROR_BACKEND_FAILURE;

    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkCommandBufferAllocateInfo cbai = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };
    if (vkAllocateCommandBuffers(d->device, &cbai, &cmd) != VK_SUCCESS) {
        vkDestroyCommandPool(d->device, pool, nullptr);
        return FLUX_ERROR_BACKEND_FAILURE;
    }

    VkCommandBufferBeginInfo cbbi = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    vkBeginCommandBuffer(cmd, &cbbi);

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

    vkEndCommandBuffer(cmd);

    VkCommandBufferSubmitInfo cbsi = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
        .commandBuffer = cmd,
    };
    VkSubmitInfo2 si = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
        .commandBufferInfoCount = 1,
        .pCommandBufferInfos = &cbsi,
    };
    VkFence fence = VK_NULL_HANDLE;
    VkFenceCreateInfo fci = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    vkCreateFence(d->device, &fci, nullptr, &fence);

    /* Queue submits are externally synchronised; flux frame submits
     * acquire d->queue_lock too, so promote contends with them via
     * the same mutex. */
    pthread_mutex_lock(&d->queue_lock);
    VkResult vr = vkQueueSubmit2(d->graphics_queue, 1, &si, fence);
    pthread_mutex_unlock(&d->queue_lock);

    if (vr == VK_SUCCESS) {
        VkResult wr = vkWaitForFences(d->device, 1, &fence, VK_TRUE, FLUX_DEFAULT_FRAME_TIMEOUT_NS);
        if (wr == VK_TIMEOUT) {
            vkDestroyFence(d->device, fence, nullptr);
            vkDestroyCommandPool(d->device, pool, nullptr);
            FLUX_FAIL(FLUX_ERROR_TIMEOUT, "promote queue wait timed out");
            return FLUX_ERROR_TIMEOUT;
        }
    }

    vkDestroyFence(d->device, fence, nullptr);
    vkDestroyCommandPool(d->device, pool, nullptr);

    if (vr != VK_SUCCESS) {
        FLUX_FAIL_VK(FLUX_ERROR_BACKEND_FAILURE, "promote queue submit failed", vr);
        return FLUX_ERROR_BACKEND_FAILURE;
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
    if (!d || !d->effect_state)
        return;
    effect_state *st = d->effect_state;
    pthread_mutex_lock(&st->lock);
    for (output_entry *o = st->outputs; o;) {
        output_entry *next = o->next;
        if (o->image)
            flux_image_release(o->image);
        flux_internal_free(d, o);
        o = next;
    }
    st->outputs = nullptr;
    /* Intermediates are kept across resets — they're a cache, not
     * an exposed transient. They go away with the device. */
    pthread_mutex_unlock(&st->lock);
}
