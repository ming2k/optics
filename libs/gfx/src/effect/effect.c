/*
 * Image-domain effects. v1 ships the Gaussian blur operator (see
 * ADR-0008). Module state hangs off optics_gfx_device via the same
 * pattern the canvas module uses (ADR-0002): a typed pointer + a
 * destroy callback set lazily on first use.
 *
 * Layout in this file:
 *   - module state struct + per-device init/destroy
 *   - transient-image pool (intermediate cache + output list)
 *   - compute pipeline lazy-build (single shader, axis push-const)
 *   - optics_gfx_effect_blur exact Gaussian entry point
 *   - fixed-cost multi-resolution filter for animated backdrops
 */

#include "../compute/internal.h"    /* optics_gfx_compute_pipeline_make_device_weak */
#include "../core/image_internal.h" /* struct optics_gfx_image layout */
#include "../core/internal.h"
#include <optics/gfx/compute.h>
#include <optics/gfx/effect.h>
#include <optics/gfx/vulkan.h>

#include <math.h>
#include <stdalign.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include <float.h>

/* Shader bytecode embedded via native C23 #embed. */
alignas(uint32_t) static const unsigned char effect_blur_spv[] = {
#embed "effect_blur.comp.spv"
};

alignas(uint32_t) static const unsigned char effect_backdrop_spv[] = {
#embed "effect_backdrop.comp.spv"
};

/* ADR-0069: rgba16f storage variants for 16F working-space content. */
alignas(uint32_t) static const unsigned char effect_blur_16f_spv[] = {
#embed "effect_blur_16f.comp.spv"
};

alignas(uint32_t) static const unsigned char effect_backdrop_16f_spv[] = {
#embed "effect_backdrop_16f.comp.spv"
};

/* ADR-0074: drop-shadow operator, same dual-storage compile. */
alignas(uint32_t) static const unsigned char effect_shadow_spv[] = {
#embed "effect_shadow.comp.spv"
};

alignas(uint32_t) static const unsigned char effect_shadow_16f_spv[] = {
#embed "effect_shadow_16f.comp.spv"
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
    uint32_t origin_x;
    uint32_t origin_y;
    uint32_t region_width;
    uint32_t region_height;
} effect_backdrop_push;

static_assert(sizeof(effect_blur_push) <= OPTICS_GFX_DEVICE_REQUIRED_PUSH_BYTES,
              "effect_blur_push exceeds device-wide push budget");
static_assert(sizeof(optics_gfx_effect_blur_desc) == 32,
              "optics_gfx_effect_blur_desc ABI size changed; use its next chain instead");
static_assert(sizeof(optics_gfx_effect_blur_regions_desc) == 32,
              "optics_gfx_effect_blur_regions_desc ABI layout changed unexpectedly");
static_assert(sizeof(effect_backdrop_push) <= OPTICS_GFX_DEVICE_REQUIRED_PUSH_BYTES,
              "effect_backdrop_push exceeds device-wide push budget");
static_assert(sizeof(effect_backdrop_push) == 52,
              "effect_backdrop_push no longer matches its shader block");
static_assert(offsetof(effect_backdrop_push, origin_x) == 36,
              "effect_backdrop_push origin no longer matches its shader block");
static_assert(offsetof(effect_backdrop_push, region_height) == 48,
              "effect_backdrop_push region no longer matches its shader block");

/* Push-constant block — must match the layout in effect_shadow.comp. */
typedef struct effect_shadow_push {
    uint32_t in_handle;
    uint32_t sampler_handle;
    uint32_t out_handle;
    uint32_t width;
    uint32_t height;
    int32_t radius;
    float sigma;
    uint32_t mode; /* 0 horizontal, 1 vertical, 2 combine */
    float offset_x;
    float offset_y;
    float tint_r;
    float tint_g;
    float tint_b;
    float alpha;
} effect_shadow_push;

static_assert(sizeof(effect_shadow_push) <= OPTICS_GFX_DEVICE_REQUIRED_PUSH_BYTES,
              "effect_shadow_push exceeds device-wide push budget");
static_assert(sizeof(effect_shadow_push) == 56,
              "effect_shadow_push no longer matches its shader block");
static_assert(offsetof(effect_shadow_push, offset_x) == 32,
              "effect_shadow_push offset no longer matches its shader block");
static_assert(offsetof(effect_shadow_push, alpha) == 52,
              "effect_shadow_push alpha no longer matches its shader block");
static_assert(sizeof(optics_gfx_effect_shadow_desc) == 56,
              "optics_gfx_effect_shadow_desc ABI size changed; use its next chain instead");

#define EFFECT_BLUR_WG 16u
#define EFFECT_BLUR_RADIUS_MAX 64

/* ------------------------------------------------------------------ */
/*  Module state                                                      */
/* ------------------------------------------------------------------ */

/* Cached intermediate target keyed by (format, width, height). One
 * intermediate per key is enough — the horizontal pass writes it,
 * the vertical pass reads it, and the two passes are serialised
 * within a single optics_gfx_effect_blur call. Callers issuing concurrent
 * blurs on the same device are already required to serialise per
 * the header doc. */
typedef struct intermediate_entry {
    uint32_t width;
    uint32_t height;
    optics_gfx_format format;
    optics_gfx_image *image;
    bool leased;
    uint64_t last_used_serial;
    struct intermediate_entry *next;
} intermediate_entry;

/* Intermediate and output slots are exclusively leased within one reset epoch.
 * They are returned to the pool only at a caller-proven GPU quiescent point. */
typedef struct output_entry {
    uint32_t width;
    uint32_t height;
    optics_gfx_format format;
    optics_gfx_image *image;
    bool leased;
    uint64_t last_used_serial;
    struct output_entry *next;
} output_entry;

/* Pool growth bound.
 *
 * The pools historically grew unbounded — every distinct (format,w,h) key
 * observed lived until device teardown — which was fine for fixed-size blurs
 * but turns a size-sweeping animation (resizes, reveals) into one pooled image
 * per visited size.
 *
 * Bounding the pool by *entry count* is the wrong dimension: at 3072x1920 an
 * RGBA16F transient is 45 MiB, so 16 intermediates plus 16 outputs is 1.4 GiB
 * of resident image memory for a desktop with two glass bodies on it. On a
 * small display the same entry count is a few MiB. The budget must be the
 * thing that actually hurts — bytes.
 *
 * EFFECT_POOL_BUDGET_BYTES is the per-pool ceiling on the summed byte size of
 * every resident transient. EFFECT_POOL_MIN_ENTRIES guarantees progress: an
 * entry larger than a quarter of the budget must still be admissible when the
 * pool is otherwise empty, or a single full-resolution blur could never run.
 * Eviction is LRU over unleased entries and parks the resources on the device
 * retire queue, so eviction at lease time is safe with batches still in
 * flight. */
/* Bounding the pool by bytes with a tight budget: 32 MiB per pool
 * bounds steady-state memory while EFFECT_POOL_MIN_ENTRIES (2) guarantees
 * progress for large dispatches without hoarding stale textures. */
#define EFFECT_POOL_BUDGET_BYTES (32ull * 1024ull * 1024ull)
#define EFFECT_POOL_MIN_ENTRIES 2u

/* Bytes one pooled transient of this key occupies, assuming the 4-byte-per-
 * channel floor. 16F keys double it via the format's own component size. */
static uint64_t transient_bytes(uint32_t w, uint32_t h, optics_gfx_format fmt) {
    uint64_t bpp = (fmt == OPTICS_GFX_FORMAT_RGBA16_SFLOAT) ? 8ull : 4ull;
    return (uint64_t)w * (uint64_t)h * bpp;
}

typedef struct effect_state {
    optics_gfx_platform_mutex lock;
    /* Pipeline slots, indexed by storage class: [0] rgba8, [1] rgba16f
     * (ADR-0069: 16F for working-space content when the device
     * advertises rgba16f storage). Lazily built; shared across calls. */
    optics_gfx_compute_pipeline *blur_pipelines[2];
    optics_gfx_compute_pipeline *backdrop_pipelines[2]; /* fixed-cost live-compositor filter */
    optics_gfx_compute_pipeline *shadow_pipelines[2];   /* ADR-0074 drop-shadow operator */
    bool storage_16f_supported; /* VK_FORMAT_R16G16B16A16_SFLOAT STORAGE_IMAGE */
    intermediate_entry *intermediates;
    output_entry *outputs;
    uint64_t pool_serial;
    uint32_t intermediate_count;
    uint32_t output_count;
    /* Summed byte size of every resident entry in each pool. */
    uint64_t intermediate_bytes;
    uint64_t output_bytes;
} effect_state;

/* Whether a pool may take one more entry of `bytes`: under the byte budget, or
 * below the minimum entry count. */
static bool pool_has_room(uint64_t resident_bytes, uint32_t resident_count, uint64_t bytes) {
    if (resident_count < EFFECT_POOL_MIN_ENTRIES)
        return true;
    return resident_bytes <= EFFECT_POOL_BUDGET_BYTES &&
           bytes <= EFFECT_POOL_BUDGET_BYTES - resident_bytes;
}

/* Evict the least-recently-used *unleased* entry from the intermediate pool.
 * Returns false when every entry is in flight. */
static bool evict_lru_intermediate(optics_gfx_device *d, effect_state *st) {
    intermediate_entry **prev_target = nullptr;
    intermediate_entry *lru = nullptr;
    for (intermediate_entry *e = st->intermediates, **prev = &st->intermediates; e;
         prev = &e->next, e = e->next) {
        if (!e->leased && (!lru || e->last_used_serial < lru->last_used_serial)) {
            lru = e;
            prev_target = prev;
        }
    }
    if (!lru)
        return false;
    *prev_target = lru->next;
    st->intermediate_bytes -= transient_bytes(lru->width, lru->height, lru->format);
    st->intermediate_count -= 1;
    /* Parks the resources on the device retire queue, so an in-flight batch
     * that still samples this image stays valid. */
    optics_gfx_image_release(lru->image);
    optics_gfx_internal_free(d, lru);
    return true;
}

/* Output-pool counterpart of `evict_lru_intermediate`. */
static bool evict_lru_output(optics_gfx_device *d, effect_state *st) {
    output_entry **prev_target = nullptr;
    output_entry *lru = nullptr;
    for (output_entry *o = st->outputs, **prev = &st->outputs; o; prev = &o->next, o = o->next) {
        if (!o->leased && (!lru || o->last_used_serial < lru->last_used_serial)) {
            lru = o;
            prev_target = prev;
        }
    }
    if (!lru)
        return false;
    *prev_target = lru->next;
    st->output_bytes -= transient_bytes(lru->width, lru->height, lru->format);
    st->output_count -= 1;
    optics_gfx_image_release(lru->image);
    optics_gfx_internal_free(d, lru);
    return true;
}

/* Evict LRU unleased entries until an entry of `incoming` bytes fits inside the
 * pool's byte budget. Returns false when it still cannot fit — every candidate
 * is leased — in which case the caller refuses the operation rather than grow
 * past the budget. */
static bool pool_make_room(optics_gfx_device *d, effect_state *st, bool intermediates,
                           uint64_t incoming) {
    while (!pool_has_room(intermediates ? st->intermediate_bytes : st->output_bytes,
                          intermediates ? st->intermediate_count : st->output_count, incoming)) {
        bool evicted = intermediates ? evict_lru_intermediate(d, st) : evict_lru_output(d, st);
        if (!evicted)
            return false;
    }
    return true;
}

typedef struct blur_filter_slot {
    uint32_t width;
    uint32_t height;
    optics_gfx_format format;
    optics_gfx_image *output;
    bool valid;
} blur_filter_slot;

struct optics_gfx_blur_filter {
    atomic_uint ref_count;
    optics_gfx_device *device; /* retained; slot images hold weak device refs */
    optics_gfx_image *half;
    optics_gfx_image *quarter;
    uint32_t scratch_width;
    uint32_t scratch_height;
    optics_gfx_format scratch_format;
    blur_filter_slot slots[OPTICS_GFX_MAX_FRAMES_IN_FLIGHT];
};

/* Parameters a deferred blur pass replays (ADR-0147). Copied into the
 * frame's plan arena, so the descriptor and region list may live on the
 * caller's stack. */
typedef struct blur_plan_params {
    effect_state *st;
    optics_gfx_device *device;
    optics_gfx_image *input;
    optics_gfx_image *half;
    optics_gfx_image *quarter;
    optics_gfx_image *output;
    float sigma;
    uint32_t root_width;
    uint32_t root_height;
    bool is16f;
    uint32_t region_count;
    optics_gfx_effect_region regions[];
} blur_plan_params;

static void record_backdrop_filter(effect_state *st, optics_gfx_device *device, VkCommandBuffer cmd,
                                   optics_gfx_image *input, optics_gfx_image *half,
                                   optics_gfx_image *quarter, optics_gfx_image *output,
                                   float requested_sigma, const optics_gfx_effect_region *regions,
                                   uint32_t region_count, bool is16f);

static void blur_pass_record(void *raw, VkCommandBuffer cmd) {
    blur_plan_params *p = raw;
    if (!p || cmd == VK_NULL_HANDLE)
        return;
    record_backdrop_filter(p->st, p->device, cmd, p->input, p->half, p->quarter, p->output,
                           p->sigma, p->region_count ? p->regions : nullptr, p->region_count,
                           p->is16f);
}

typedef struct shadow_filter_slot {
    uint32_t width;
    uint32_t height;
    optics_gfx_format format;
    optics_gfx_image *output;
} shadow_filter_slot;

/* Parameters a deferred shadow pass replays (ADR-0147). The descriptor's
 * scalar policy is copied by value, so the caller's stack copy may die. */
typedef struct shadow_plan_params {
    effect_state *st;
    optics_gfx_device *device;
    optics_gfx_image *mask;
    optics_gfx_image *ping;
    optics_gfx_image *pong;
    optics_gfx_image *output;
    optics_gfx_effect_shadow_desc desc;
} shadow_plan_params;

static optics_gfx_result record_shadow_dispatch(effect_state *st, optics_gfx_device *d,
                                                VkCommandBuffer cmd, optics_gfx_image *mask,
                                                optics_gfx_image *ping, optics_gfx_image *pong,
                                                optics_gfx_image *output,
                                                const optics_gfx_effect_shadow_desc *desc,
                                                bool is16f);

static void shadow_pass_record(void *raw, VkCommandBuffer cmd) {
    shadow_plan_params *p = raw;
    if (!p || cmd == VK_NULL_HANDLE)
        return;
    (void)record_shadow_dispatch(
        p->st, p->device, cmd, p->mask, p->ping, p->pong, p->output, &p->desc,
        optics_gfx_image_format(p->mask) == OPTICS_GFX_FORMAT_RGBA16_SFLOAT);
}

struct optics_gfx_shadow_filter {
    atomic_uint ref_count;
    optics_gfx_device *device; /* retained; slot images hold weak device refs */
    optics_gfx_image *ping;
    optics_gfx_image *pong;
    uint32_t scratch_width;
    uint32_t scratch_height;
    optics_gfx_format scratch_format;
    shadow_filter_slot slots[OPTICS_GFX_MAX_FRAMES_IN_FLIGHT];
};

static void effect_state_destroy(optics_gfx_device *d) {
    effect_state *st = d->effect_state;
    if (!st)
        return;

    for (output_entry *o = st->outputs; o;) {
        output_entry *next = o->next;
        if (o->image)
            optics_gfx_image_release(o->image);
        optics_gfx_internal_free(d, o);
        o = next;
    }
    for (intermediate_entry *e = st->intermediates; e;) {
        intermediate_entry *next = e->next;
        if (e->image)
            optics_gfx_image_release(e->image);
        optics_gfx_internal_free(d, e);
        e = next;
    }
    if (st->blur_pipelines[0])
        optics_gfx_compute_pipeline_release(st->blur_pipelines[0]);
    if (st->blur_pipelines[1])
        optics_gfx_compute_pipeline_release(st->blur_pipelines[1]);
    if (st->backdrop_pipelines[0])
        optics_gfx_compute_pipeline_release(st->backdrop_pipelines[0]);
    if (st->backdrop_pipelines[1])
        optics_gfx_compute_pipeline_release(st->backdrop_pipelines[1]);
    if (st->shadow_pipelines[0])
        optics_gfx_compute_pipeline_release(st->shadow_pipelines[0]);
    if (st->shadow_pipelines[1])
        optics_gfx_compute_pipeline_release(st->shadow_pipelines[1]);
    optics_gfx_platform_mutex_destroy(&st->lock);
    optics_gfx_internal_free(d, st);
    d->effect_state = nullptr;
    d->effect_state_destroy = nullptr;
}

static effect_state *effect_state_get_or_init(optics_gfx_device *d) {
    optics_gfx_platform_mutex_lock(&d->module_state_lock);
    effect_state *published = d->effect_state;
    optics_gfx_platform_mutex_unlock(&d->module_state_lock);
    if (published)
        return published;

    effect_state *candidate = optics_gfx_internal_alloc(d, sizeof(*candidate));
    if (!candidate)
        return nullptr;
    if (!optics_gfx_platform_mutex_init(&candidate->lock)) {
        optics_gfx_internal_free(d, candidate);
        return nullptr;
    }
    /* rgba16f storage is an optional format feature; resolve once per
     * device (ADR-0069's "effects accept 16F when the device advertises
     * the format features"). */
    {
        VkFormatProperties props;
        vkGetPhysicalDeviceFormatProperties(d->physical_device, VK_FORMAT_R16G16B16A16_SFLOAT,
                                            &props);
        candidate->storage_16f_supported =
            (props.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) != 0;
    }

    optics_gfx_platform_mutex_lock(&d->module_state_lock);
    if (!d->effect_state) {
        d->effect_state = candidate;
        d->effect_state_destroy = effect_state_destroy;
        published = candidate;
        candidate = nullptr;
    } else {
        published = d->effect_state;
    }
    optics_gfx_platform_mutex_unlock(&d->module_state_lock);

    if (candidate) {
        optics_gfx_platform_mutex_destroy(&candidate->lock);
        optics_gfx_internal_free(d, candidate);
    }
    return published;
}

/* ------------------------------------------------------------------ */
/*  Transient acquisition                                             */
/* ------------------------------------------------------------------ */

/* Look up or allocate the intermediate for this key. Returned image is owned
 * by the pool and lives until reset. The pool admits a new key only while its
 * summed byte size stays inside EFFECT_POOL_BUDGET_BYTES; above that the
 * least-recently-used unleased entry is evicted — released through the device
 * retire queue, so in-flight batches that still sample it stay safe. */
static optics_gfx_result acquire_intermediate(optics_gfx_device *d, effect_state *st, uint32_t w,
                                              uint32_t h, optics_gfx_format fmt,
                                              optics_gfx_image **out,
                                              intermediate_entry **out_lease) {
    st->pool_serial++;
    uint64_t now = st->pool_serial;
    for (intermediate_entry *e = st->intermediates; e; e = e->next) {
        if (!e->leased && e->width == w && e->height == h && e->format == fmt) {
            e->leased = true;
            e->last_used_serial = now;
            *out = e->image;
            *out_lease = e;
            return OPTICS_GFX_OK;
        }
    }
    uint64_t bytes = transient_bytes(w, h, fmt);
    if (!pool_make_room(d, st, true, bytes)) {
        OPTICS_GFX_FAILF(
            OPTICS_GFX_ERROR_OUT_OF_MEMORY,
            "intermediate pool at its %llu MiB budget: %u entries / %llu MiB resident, "
            "requesting %ux%u (%llu MiB), every evictable entry in flight",
            (unsigned long long)(EFFECT_POOL_BUDGET_BYTES >> 20), st->intermediate_count,
            (unsigned long long)(st->intermediate_bytes >> 20), w, h,
            (unsigned long long)(bytes >> 20));
        return OPTICS_GFX_ERROR_OUT_OF_MEMORY;
    }
    optics_gfx_image *img = nullptr;
    optics_gfx_result r = optics_gfx_image_create(
        d,
        &(optics_gfx_image_desc){.type = OPTICS_GFX_TYPE_IMAGE_DESC,
                                 .width = w,
                                 .height = h,
                                 .format = fmt,
                                 .usage = OPTICS_GFX_IMAGE_USAGE_COMPUTE_WRITE},
        &img);
    if (r != OPTICS_GFX_OK)
        return r;

    intermediate_entry *e = optics_gfx_internal_alloc(d, sizeof(*e));
    if (!e) {
        optics_gfx_image_release(img);
        return OPTICS_GFX_ERROR_OUT_OF_MEMORY;
    }
    e->width = w;
    e->height = h;
    e->format = fmt;
    e->image = img;
    e->leased = true;
    e->last_used_serial = now;
    e->next = st->intermediates;
    st->intermediates = e;
    st->intermediate_count++;
    st->intermediate_bytes += bytes;
    *out = img;
    *out_lease = e;
    return OPTICS_GFX_OK;
}

/* Lease a same-key output, growing the pool while it fits the byte budget;
 * evicted LRU-first like the intermediates. */
static optics_gfx_result acquire_output(optics_gfx_device *d, effect_state *st, uint32_t w,
                                        uint32_t h, optics_gfx_format fmt, optics_gfx_image **out,
                                        output_entry **out_lease) {
    st->pool_serial++;
    uint64_t now = st->pool_serial;
    for (output_entry *o = st->outputs; o; o = o->next) {
        if (!o->leased && o->width == w && o->height == h && o->format == fmt) {
            o->leased = true;
            o->last_used_serial = now;
            *out = o->image;
            *out_lease = o;
            return OPTICS_GFX_OK;
        }
    }
    uint64_t bytes = transient_bytes(w, h, fmt);
    if (!pool_make_room(d, st, false, bytes)) {
        OPTICS_GFX_FAILF(OPTICS_GFX_ERROR_OUT_OF_MEMORY,
                         "output pool at its %llu MiB budget: %u entries / %llu MiB resident, "
                         "requesting %ux%u (%llu MiB), every evictable entry in flight",
                         (unsigned long long)(EFFECT_POOL_BUDGET_BYTES >> 20), st->output_count,
                         (unsigned long long)(st->output_bytes >> 20), w, h,
                         (unsigned long long)(bytes >> 20));
        return OPTICS_GFX_ERROR_OUT_OF_MEMORY;
    }
    optics_gfx_image *img = nullptr;
    optics_gfx_result r = optics_gfx_image_create(
        d,
        &(optics_gfx_image_desc){.type = OPTICS_GFX_TYPE_IMAGE_DESC,
                                 .width = w,
                                 .height = h,
                                 .format = fmt,
                                 .usage = OPTICS_GFX_IMAGE_USAGE_COMPUTE_WRITE},
        &img);
    if (r != OPTICS_GFX_OK)
        return r;

    output_entry *o = optics_gfx_internal_alloc(d, sizeof(*o));
    if (!o) {
        optics_gfx_image_release(img);
        return OPTICS_GFX_ERROR_OUT_OF_MEMORY;
    }
    o->width = w;
    o->height = h;
    o->format = fmt;
    o->image = img;
    o->leased = true;
    o->last_used_serial = now;
    o->next = st->outputs;
    st->outputs = o;
    st->output_count++;
    st->output_bytes += bytes;
    *out = img;
    *out_lease = o;
    return OPTICS_GFX_OK;
}

/* ------------------------------------------------------------------ */
/*  Pipeline build                                                    */
/* ------------------------------------------------------------------ */

static optics_gfx_result ensure_pipeline(optics_gfx_device *d, optics_gfx_compute_pipeline **slot,
                                         const unsigned char *spv, size_t spv_bytes,
                                         uint32_t push_bytes) {
    if (*slot)
        return OPTICS_GFX_OK;
    optics_gfx_compute_pipeline_desc pdesc = OPTICS_GFX_COMPUTE_PIPELINE_DESC_INIT;
    pdesc.spirv = (const uint32_t *)spv;
    pdesc.spirv_word_count = spv_bytes / sizeof(uint32_t);
    pdesc.entry_point = "main";
    pdesc.push_constant_bytes = push_bytes;
    optics_gfx_result r = optics_gfx_compute_pipeline_create(d, &pdesc, slot);
    if (r != OPTICS_GFX_OK)
        return r;
    /* The pipeline lives in per-device module state and is released
     * by effect_state_destroy inside optics_gfx_device_release; a strong
     * device ref would cycle and leak the whole device. */
    optics_gfx_compute_pipeline_make_device_weak(*slot);
    return OPTICS_GFX_OK;
}

static optics_gfx_result ensure_blur_pipeline(optics_gfx_device *d, effect_state *st, bool is16f) {
    return ensure_pipeline(
        d, &st->blur_pipelines[is16f ? 1 : 0], is16f ? effect_blur_16f_spv : effect_blur_spv,
        is16f ? sizeof(effect_blur_16f_spv) : sizeof(effect_blur_spv), sizeof(effect_blur_push));
}

static optics_gfx_result ensure_backdrop_pipeline(optics_gfx_device *d, effect_state *st,
                                                  bool is16f) {
    return ensure_pipeline(d, &st->backdrop_pipelines[is16f ? 1 : 0],
                           is16f ? effect_backdrop_16f_spv : effect_backdrop_spv,
                           is16f ? sizeof(effect_backdrop_16f_spv) : sizeof(effect_backdrop_spv),
                           sizeof(effect_backdrop_push));
}

static optics_gfx_result ensure_shadow_pipeline(optics_gfx_device *d, effect_state *st,
                                                bool is16f) {
    return ensure_pipeline(d, &st->shadow_pipelines[is16f ? 1 : 0],
                           is16f ? effect_shadow_16f_spv : effect_shadow_spv,
                           is16f ? sizeof(effect_shadow_16f_spv) : sizeof(effect_shadow_spv),
                           sizeof(effect_shadow_push));
}

/* Storage/intermediate format for an effect input (ADR-0069): 8-bit
 * SDR content keeps the historic RGBA8 path; 16F working-space content
 * stays 16F so blurring runs in linear light — when the device
 * advertises rgba16f storage. */
static optics_gfx_result effect_resolve_storage(effect_state *st, const optics_gfx_image *in,
                                                optics_gfx_format *out, bool *out_16f) {
    if (in->format == OPTICS_GFX_FORMAT_RGBA8_UNORM ||
        in->format == OPTICS_GFX_FORMAT_BGRA8_UNORM) {
        *out = OPTICS_GFX_FORMAT_RGBA8_UNORM;
        *out_16f = false;
        return OPTICS_GFX_OK;
    }
    if (in->format == OPTICS_GFX_FORMAT_RGBA16_SFLOAT) {
        if (!st->storage_16f_supported) {
            OPTICS_GFX_FAIL(OPTICS_GFX_ERROR_UNSUPPORTED,
                            "16F effect input needs rgba16f storage support on this device");
            return OPTICS_GFX_ERROR_UNSUPPORTED;
        }
        *out = OPTICS_GFX_FORMAT_RGBA16_SFLOAT;
        *out_16f = true;
        return OPTICS_GFX_OK;
    }
    OPTICS_GFX_FAIL(OPTICS_GFX_ERROR_UNSUPPORTED,
                    "effect input format not supported (use RGBA8/BGRA8_UNORM or RGBA16_SFLOAT)");
    return OPTICS_GFX_ERROR_UNSUPPORTED;
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

static optics_gfx_result record_blur_dispatch(effect_state *st, optics_gfx_device *d,
                                              VkCommandBuffer cmd, optics_gfx_image *in,
                                              optics_gfx_image *intermediate,
                                              optics_gfx_image *output, float requested_sigma,
                                              bool is16f) {
    float sigma = requested_sigma;
    if (!(sigma == sigma) || sigma < 0.0f)
        sigma = 0.0f;
    if (sigma > OPTICS_GFX_EFFECT_BLUR_SIGMA_MAX)
        sigma = OPTICS_GFX_EFFECT_BLUR_SIGMA_MAX;
    int32_t radius = (int32_t)ceilf(3.0f * sigma);
    if (radius > EFFECT_BLUR_RADIUS_MAX)
        radius = EFFECT_BLUR_RADIUS_MAX;

    barrier_reuse_to_compute_write(cmd, intermediate->image);
    barrier_reuse_to_compute_write(cmd, output->image);

    optics_gfx_bindless_handle sampler_h = optics_gfx_device_default_sampler_handle(d);
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
    optics_gfx_compute_dispatch(
        &(optics_gfx_compute_dispatch_opts){.cmd = cmd,
                                            .pipeline = st->blur_pipelines[is16f ? 1 : 0],
                                            .push_constants = &pc,
                                            .push_bytes = sizeof(pc),
                                            .group_x = gx,
                                            .group_y = gy,
                                            .group_z = 1});

    barrier_compute_write_to_read(cmd, intermediate->image);

    pc.in_handle = intermediate->bindless;
    pc.out_handle = output->bindless_storage;
    pc.axis = 1u;
    optics_gfx_compute_dispatch(
        &(optics_gfx_compute_dispatch_opts){.cmd = cmd,
                                            .pipeline = st->blur_pipelines[is16f ? 1 : 0],
                                            .push_constants = &pc,
                                            .push_bytes = sizeof(pc),
                                            .group_x = gx,
                                            .group_y = gy,
                                            .group_z = 1});

    barrier_compute_write_to_read(cmd, output->image);
    return OPTICS_GFX_OK;
}

/* ------------------------------------------------------------------ */
/*  Public: optics_gfx_effect_blur                                          */
/* ------------------------------------------------------------------ */

optics_gfx_result optics_gfx_effect_blur(const optics_gfx_effect_blur_opts *opts) {
    if (!opts || !opts->cmd || !opts->desc || !opts->out)
        return OPTICS_GFX_ERROR_INVALID_ARGUMENT;
    VkCommandBuffer cmd = opts->cmd;
    const optics_gfx_effect_blur_desc *desc = opts->desc;
    optics_gfx_image **out = opts->out;
    if (desc->type != OPTICS_GFX_TYPE_EFFECT_BLUR_DESC) {
        OPTICS_GFX_FAIL(OPTICS_GFX_ERROR_INVALID_ARGUMENT,
                        "desc->type != OPTICS_GFX_TYPE_EFFECT_BLUR_DESC");
        return OPTICS_GFX_ERROR_INVALID_ARGUMENT;
    }
    if (desc->next) {
        OPTICS_GFX_FAIL(OPTICS_GFX_ERROR_INVALID_ARGUMENT,
                        "exact blur does not accept reusable-blur extensions");
        return OPTICS_GFX_ERROR_INVALID_ARGUMENT;
    }
    if (!desc->input) {
        OPTICS_GFX_FAIL(OPTICS_GFX_ERROR_INVALID_ARGUMENT, "blur input is null");
        return OPTICS_GFX_ERROR_INVALID_ARGUMENT;
    }
    optics_gfx_image *in = desc->input;
    /* This primitive records straight into `cmd` and does not participate in the
     * plan, so it can neither require producers nor be pruned. What it MUST not do
     * is sample an output whose pass has not run: that reads last frame's pixels
     * under this frame's handle (ADR-0147). Checked here — after every descriptor
     * field has been validated and before the image is dereferenced, so a garbage
     * pointer in a validation test still gets the argument error it expects. */
    if (optics_gfx_image_is_deferred_pending(in)) {
        OPTICS_GFX_FAIL(OPTICS_GFX_ERROR_INVALID_STATE,
                        "exact blur input is a deferred pass output that has not executed; "
                        "materialize it or use optics_gfx_blur_filter_apply instead");
        return OPTICS_GFX_ERROR_INVALID_STATE;
    }
    optics_gfx_device *d = in->device;
    if (in->bindless == OPTICS_GFX_BINDLESS_INVALID) {
        OPTICS_GFX_FAIL(OPTICS_GFX_ERROR_INVALID_ARGUMENT,
                        "blur input lacks sampled bindless handle");
        return OPTICS_GFX_ERROR_INVALID_ARGUMENT;
    }
    *out = nullptr;

    effect_state *st = effect_state_get_or_init(d);
    if (!st)
        return OPTICS_GFX_ERROR_OUT_OF_MEMORY;

    /* 8-bit SDR content takes the historic RGBA8 path; 16F working-space
     * content stays 16F so the blur runs in linear light (ADR-0069). */
    optics_gfx_format storage_format;
    bool is16f;
    optics_gfx_result rr = effect_resolve_storage(st, in, &storage_format, &is16f);
    if (rr != OPTICS_GFX_OK)
        return rr;

    optics_gfx_platform_mutex_lock(&st->lock);

    optics_gfx_result r = ensure_blur_pipeline(d, st, is16f);
    if (r != OPTICS_GFX_OK) {
        optics_gfx_platform_mutex_unlock(&st->lock);
        return r;
    }

    optics_gfx_image *intermediate = nullptr;
    intermediate_entry *intermediate_lease = nullptr;
    r = acquire_intermediate(d, st, in->width, in->height, storage_format, &intermediate,
                             &intermediate_lease);
    if (r != OPTICS_GFX_OK) {
        optics_gfx_platform_mutex_unlock(&st->lock);
        return r;
    }

    optics_gfx_image *output = nullptr;
    output_entry *output_lease = nullptr;
    r = acquire_output(d, st, in->width, in->height, storage_format, &output, &output_lease);
    if (r != OPTICS_GFX_OK) {
        intermediate_lease->leased = false;
        optics_gfx_platform_mutex_unlock(&st->lock);
        return r;
    }
    (void)output_lease;

    optics_gfx_platform_mutex_unlock(&st->lock);

    r = record_blur_dispatch(st, d, cmd, in, intermediate, output, desc->sigma, is16f);
    if (r != OPTICS_GFX_OK)
        return r;
    *out = output;
    return OPTICS_GFX_OK;
}

/* ------------------------------------------------------------------ */
/*  Public: optics_gfx_effect_shadow                                        */
/* ------------------------------------------------------------------ */

#define EFFECT_SHADOW_WG 16u
#define EFFECT_SHADOW_RADIUS_MAX 64

static bool effect_finite(float v) {
    return v == v && v > -FLT_MAX && v < FLT_MAX;
}

static optics_gfx_result record_shadow_dispatch(effect_state *st, optics_gfx_device *d,
                                                VkCommandBuffer cmd, optics_gfx_image *mask,
                                                optics_gfx_image *ping, optics_gfx_image *pong,
                                                optics_gfx_image *output,
                                                const optics_gfx_effect_shadow_desc *desc,
                                                bool is16f) {
    float sigma = desc->blur;
    if (!(sigma == sigma) || sigma < 0.0f)
        sigma = 0.0f;
    if (sigma > OPTICS_GFX_EFFECT_SHADOW_BLUR_MAX)
        sigma = OPTICS_GFX_EFFECT_SHADOW_BLUR_MAX;
    int32_t radius = (int32_t)ceilf(3.0f * sigma);
    if (radius > EFFECT_SHADOW_RADIUS_MAX)
        radius = EFFECT_SHADOW_RADIUS_MAX;

    barrier_reuse_to_compute_write(cmd, ping->image);
    barrier_reuse_to_compute_write(cmd, pong->image);
    barrier_reuse_to_compute_write(cmd, output->image);

    optics_gfx_bindless_handle sampler_h = optics_gfx_device_default_sampler_handle(d);
    uint32_t gx = (mask->width + EFFECT_SHADOW_WG - 1) / EFFECT_SHADOW_WG;
    uint32_t gy = (mask->height + EFFECT_SHADOW_WG - 1) / EFFECT_SHADOW_WG;

    float alpha = desc->alpha;
    if (!(alpha == alpha) || alpha < 0.0f)
        alpha = 0.0f;
    if (alpha > 1.0f)
        alpha = 1.0f;

    effect_shadow_push pc = {
        .in_handle = mask->bindless,
        .sampler_handle = sampler_h,
        .out_handle = ping->bindless_storage,
        .width = mask->width,
        .height = mask->height,
        .radius = radius,
        .sigma = (sigma <= 0.0f) ? 1.0f : sigma,
        .mode = 0u,
        .offset_x = desc->offset_x,
        .offset_y = desc->offset_y,
        .tint_r = desc->tint_red,
        .tint_g = desc->tint_green,
        .tint_b = desc->tint_blue,
        .alpha = alpha,
    };
    optics_gfx_compute_pipeline *pipeline = st->shadow_pipelines[is16f ? 1 : 0];

    /* Pass 0: horizontal Gaussian over the mask alpha → ping. */
    optics_gfx_compute_dispatch(&(optics_gfx_compute_dispatch_opts){.cmd = cmd,
                                                                    .pipeline = pipeline,
                                                                    .push_constants = &pc,
                                                                    .push_bytes = sizeof(pc),
                                                                    .group_x = gx,
                                                                    .group_y = gy,
                                                                    .group_z = 1});
    barrier_compute_write_to_read(cmd, ping->image);

    /* Pass 1: vertical Gaussian over ping → pong. */
    pc.in_handle = ping->bindless;
    pc.out_handle = pong->bindless_storage;
    pc.mode = 1u;
    optics_gfx_compute_dispatch(&(optics_gfx_compute_dispatch_opts){.cmd = cmd,
                                                                    .pipeline = pipeline,
                                                                    .push_constants = &pc,
                                                                    .push_bytes = sizeof(pc),
                                                                    .group_x = gx,
                                                                    .group_y = gy,
                                                                    .group_z = 1});
    barrier_compute_write_to_read(cmd, pong->image);

    /* Pass 2: combine — sample pong at (p - offset), tint, premultiply. */
    pc.in_handle = pong->bindless;
    pc.out_handle = output->bindless_storage;
    pc.mode = 2u;
    optics_gfx_compute_dispatch(&(optics_gfx_compute_dispatch_opts){.cmd = cmd,
                                                                    .pipeline = pipeline,
                                                                    .push_constants = &pc,
                                                                    .push_bytes = sizeof(pc),
                                                                    .group_x = gx,
                                                                    .group_y = gy,
                                                                    .group_z = 1});
    barrier_compute_write_to_read(cmd, output->image);
    return OPTICS_GFX_OK;
}

optics_gfx_result optics_gfx_effect_shadow(const optics_gfx_effect_shadow_opts *opts) {
    if (!opts || !opts->cmd || !opts->desc || !opts->out)
        return OPTICS_GFX_ERROR_INVALID_ARGUMENT;
    VkCommandBuffer cmd = opts->cmd;
    const optics_gfx_effect_shadow_desc *desc = opts->desc;
    optics_gfx_image **out = opts->out;
    if (desc->type != OPTICS_GFX_TYPE_EFFECT_SHADOW_DESC) {
        OPTICS_GFX_FAIL(OPTICS_GFX_ERROR_INVALID_ARGUMENT,
                        "desc->type != OPTICS_GFX_TYPE_EFFECT_SHADOW_DESC");
        return OPTICS_GFX_ERROR_INVALID_ARGUMENT;
    }
    if (desc->next) {
        OPTICS_GFX_FAIL(OPTICS_GFX_ERROR_INVALID_ARGUMENT, "shadow does not accept extensions yet");
        return OPTICS_GFX_ERROR_INVALID_ARGUMENT;
    }
    if (!desc->input) {
        OPTICS_GFX_FAIL(OPTICS_GFX_ERROR_INVALID_ARGUMENT, "shadow input is null");
        return OPTICS_GFX_ERROR_INVALID_ARGUMENT;
    }
    if (!effect_finite(desc->offset_x) || !effect_finite(desc->offset_y) ||
        !effect_finite(desc->tint_red) || !effect_finite(desc->tint_green) ||
        !effect_finite(desc->tint_blue) || !effect_finite(desc->alpha) ||
        !effect_finite(desc->blur)) {
        OPTICS_GFX_FAIL(OPTICS_GFX_ERROR_INVALID_ARGUMENT, "shadow parameters must be finite");
        return OPTICS_GFX_ERROR_INVALID_ARGUMENT;
    }
    optics_gfx_image *in = desc->input;
    /* Same rule as the exact blur, and the same placement: after descriptor
     * validation, before the image is dereferenced. */
    if (optics_gfx_image_is_deferred_pending(in)) {
        OPTICS_GFX_FAIL(OPTICS_GFX_ERROR_INVALID_STATE,
                        "exact shadow input is a deferred pass output that has not executed; "
                        "materialize it or use optics_gfx_shadow_filter_apply instead");
        return OPTICS_GFX_ERROR_INVALID_STATE;
    }
    optics_gfx_device *d = in->device;
    if (in->bindless == OPTICS_GFX_BINDLESS_INVALID) {
        OPTICS_GFX_FAIL(OPTICS_GFX_ERROR_INVALID_ARGUMENT,
                        "shadow input lacks sampled bindless handle");
        return OPTICS_GFX_ERROR_INVALID_ARGUMENT;
    }
    *out = nullptr;

    effect_state *st = effect_state_get_or_init(d);
    if (!st)
        return OPTICS_GFX_ERROR_OUT_OF_MEMORY;

    optics_gfx_format storage_format;
    bool is16f;
    optics_gfx_result rr = effect_resolve_storage(st, in, &storage_format, &is16f);
    if (rr != OPTICS_GFX_OK)
        return rr;

    optics_gfx_platform_mutex_lock(&st->lock);

    optics_gfx_result r = ensure_shadow_pipeline(d, st, is16f);
    if (r != OPTICS_GFX_OK) {
        optics_gfx_platform_mutex_unlock(&st->lock);
        return r;
    }

    /* Ping and pong intermediates: same key as the blur's single
     * intermediate, so a mixed blur+shadow workload reuses pool entries
     * once leases return at reset. */
    optics_gfx_image *ping = nullptr;
    intermediate_entry *ping_lease = nullptr;
    r = acquire_intermediate(d, st, in->width, in->height, storage_format, &ping, &ping_lease);
    if (r != OPTICS_GFX_OK) {
        optics_gfx_platform_mutex_unlock(&st->lock);
        return r;
    }

    optics_gfx_image *pong = nullptr;
    intermediate_entry *pong_lease = nullptr;
    r = acquire_intermediate(d, st, in->width, in->height, storage_format, &pong, &pong_lease);
    if (r != OPTICS_GFX_OK) {
        ping_lease->leased = false;
        optics_gfx_platform_mutex_unlock(&st->lock);
        return r;
    }

    optics_gfx_image *output = nullptr;
    output_entry *output_lease = nullptr;
    r = acquire_output(d, st, in->width, in->height, storage_format, &output, &output_lease);
    if (r != OPTICS_GFX_OK) {
        ping_lease->leased = false;
        pong_lease->leased = false;
        optics_gfx_platform_mutex_unlock(&st->lock);
        return r;
    }
    (void)output_lease;

    optics_gfx_platform_mutex_unlock(&st->lock);

    r = record_shadow_dispatch(st, d, cmd, in, ping, pong, output, desc, is16f);
    if (r != OPTICS_GFX_OK)
        return r;
    *out = output;
    return OPTICS_GFX_OK;
}

/* ------------------------------------------------------------------ */
/*  Reusable frame-slot realtime blur                                 */
/* ------------------------------------------------------------------ */

optics_gfx_result optics_gfx_blur_filter_create(optics_gfx_device *device,
                                                optics_gfx_blur_filter **out) {
    if (!device || !out)
        return OPTICS_GFX_ERROR_INVALID_ARGUMENT;
    *out = nullptr;
    optics_gfx_blur_filter *filter = optics_gfx_internal_alloc(device, sizeof(*filter));
    if (!filter)
        return OPTICS_GFX_ERROR_OUT_OF_MEMORY;
    atomic_init(&filter->ref_count, 1u);
    filter->device = optics_gfx_device_retain(device);
    *out = filter;
    return OPTICS_GFX_OK;
}

optics_gfx_blur_filter *optics_gfx_blur_filter_retain(optics_gfx_blur_filter *filter) {
    if (filter)
        atomic_fetch_add_explicit(&filter->ref_count, 1u, memory_order_relaxed);
    return filter;
}

void optics_gfx_blur_filter_release(optics_gfx_blur_filter *filter) {
    if (!filter)
        return;
    if (atomic_fetch_sub_explicit(&filter->ref_count, 1u, memory_order_acq_rel) != 1u)
        return;
    optics_gfx_device *device = filter->device;
    if (filter->quarter)
        optics_gfx_image_release(filter->quarter);
    if (filter->half)
        optics_gfx_image_release(filter->half);
    for (uint32_t i = 0; i < OPTICS_GFX_MAX_FRAMES_IN_FLIGHT; ++i) {
        if (filter->slots[i].output)
            optics_gfx_image_release(filter->slots[i].output);
    }
    optics_gfx_internal_free(device, filter);
    optics_gfx_device_release(device);
}

static optics_gfx_result blur_filter_ensure_scratch(optics_gfx_blur_filter *filter, uint32_t in_w,
                                                    uint32_t in_h,
                                                    optics_gfx_format storage_format) {
    if (filter->half && filter->quarter && filter->scratch_width == in_w &&
        filter->scratch_height == in_h && filter->scratch_format == storage_format)
        return OPTICS_GFX_OK;

    if (filter->quarter)
        optics_gfx_image_release(filter->quarter);
    if (filter->half)
        optics_gfx_image_release(filter->half);
    filter->half = nullptr;
    filter->quarter = nullptr;

    uint32_t half_width = (in_w + 1u) / 2u;
    uint32_t half_height = (in_h + 1u) / 2u;
    uint32_t quarter_width = (half_width + 1u) / 2u;
    uint32_t quarter_height = (half_height + 1u) / 2u;
    optics_gfx_result r = optics_gfx_image_create(
        filter->device,
        &(optics_gfx_image_desc){.type = OPTICS_GFX_TYPE_IMAGE_DESC,
                                 .width = half_width,
                                 .height = half_height,
                                 .format = storage_format,
                                 .usage = OPTICS_GFX_IMAGE_USAGE_COMPUTE_WRITE},
        &filter->half);
    if (r != OPTICS_GFX_OK)
        return r;
    r = optics_gfx_image_create(
        filter->device,
        &(optics_gfx_image_desc){.type = OPTICS_GFX_TYPE_IMAGE_DESC,
                                 .width = quarter_width,
                                 .height = quarter_height,
                                 .format = storage_format,
                                 .usage = OPTICS_GFX_IMAGE_USAGE_COMPUTE_WRITE},
        &filter->quarter);
    if (r != OPTICS_GFX_OK) {
        optics_gfx_image_release(filter->half);
        filter->half = nullptr;
        return r;
    }
    filter->scratch_width = in_w;
    filter->scratch_height = in_h;
    filter->scratch_format = storage_format;
    return OPTICS_GFX_OK;
}

static optics_gfx_result blur_filter_ensure_slot(optics_gfx_blur_filter *filter, uint32_t index,
                                                 const optics_gfx_image *input,
                                                 optics_gfx_format storage_format) {
    blur_filter_slot *slot = &filter->slots[index];
    if (slot->output && slot->width == input->width && slot->height == input->height &&
        slot->format == storage_format)
        return OPTICS_GFX_OK;

    if (slot->output)
        optics_gfx_image_release(slot->output);
    *slot = (blur_filter_slot){};

    optics_gfx_result r = optics_gfx_image_create(
        filter->device,
        &(optics_gfx_image_desc){.type = OPTICS_GFX_TYPE_IMAGE_DESC,
                                 .width = input->width,
                                 .height = input->height,
                                 .format = storage_format,
                                 .usage = OPTICS_GFX_IMAGE_USAGE_COMPUTE_WRITE},
        &slot->output);
    if (r != OPTICS_GFX_OK)
        return r;
    slot->width = input->width;
    slot->height = input->height;
    slot->format = storage_format;
    return OPTICS_GFX_OK;
}

static bool map_effect_region(const optics_gfx_effect_region *region, uint32_t root_width,
                              uint32_t root_height, uint32_t output_width, uint32_t output_height,
                              uint32_t *out_x, uint32_t *out_y, uint32_t *out_width,
                              uint32_t *out_height) {
    uint64_t x0 = region->x < root_width ? region->x : root_width;
    uint64_t y0 = region->y < root_height ? region->y : root_height;
    uint64_t x1 = (uint64_t)region->x + region->width;
    uint64_t y1 = (uint64_t)region->y + region->height;
    if (x1 > root_width)
        x1 = root_width;
    if (y1 > root_height)
        y1 = root_height;
    if (x1 <= x0 || y1 <= y0)
        return false;

    /* Map outward so a fractional pyramid edge can never discard a source
     * pixel requested by the caller. */
    uint64_t ox0 = x0 * output_width / root_width;
    uint64_t oy0 = y0 * output_height / root_height;
    uint64_t ox1 = (x1 * output_width + root_width - 1u) / root_width;
    uint64_t oy1 = (y1 * output_height + root_height - 1u) / root_height;
    if (ox1 > output_width)
        ox1 = output_width;
    if (oy1 > output_height)
        oy1 = output_height;
    if (ox1 <= ox0 || oy1 <= oy0)
        return false;
    *out_x = (uint32_t)ox0;
    *out_y = (uint32_t)oy0;
    *out_width = (uint32_t)(ox1 - ox0);
    *out_height = (uint32_t)(oy1 - oy0);
    return true;
}

static void record_backdrop_pass(effect_state *st, optics_gfx_device *device, VkCommandBuffer cmd,
                                 optics_gfx_image *input, optics_gfx_image *output, float offset,
                                 uint32_t mode, uint32_t root_width, uint32_t root_height,
                                 const optics_gfx_effect_region *regions, uint32_t region_count,
                                 bool is16f) {
    barrier_reuse_to_compute_write(cmd, output->image);
    optics_gfx_compute_pipeline *pipeline = st->backdrop_pipelines[is16f ? 1 : 0];
    effect_backdrop_push pc = {
        .in_handle = input->bindless,
        .sampler_handle = optics_gfx_device_default_sampler_handle(device),
        .out_handle = output->bindless_storage,
        .input_width = input->width,
        .input_height = input->height,
        .output_width = output->width,
        .output_height = output->height,
        .offset = offset,
        .mode = mode,
    };
    if (!regions || region_count == 0) {
        pc.region_width = output->width;
        pc.region_height = output->height;
        uint32_t gx = (output->width + EFFECT_BLUR_WG - 1u) / EFFECT_BLUR_WG;
        uint32_t gy = (output->height + EFFECT_BLUR_WG - 1u) / EFFECT_BLUR_WG;
        optics_gfx_compute_dispatch(&(optics_gfx_compute_dispatch_opts){.cmd = cmd,
                                                                        .pipeline = pipeline,
                                                                        .push_constants = &pc,
                                                                        .push_bytes = sizeof(pc),
                                                                        .group_x = gx,
                                                                        .group_y = gy,
                                                                        .group_z = 1u});
    } else {
        bool dispatched = false;
        for (uint32_t i = 0; i < region_count; ++i) {
            if (!map_effect_region(&regions[i], root_width, root_height, output->width,
                                   output->height, &pc.origin_x, &pc.origin_y, &pc.region_width,
                                   &pc.region_height))
                continue;
            if (dispatched)
                barrier_compute_write_to_write(cmd, output->image);
            uint32_t gx = (pc.region_width + EFFECT_BLUR_WG - 1u) / EFFECT_BLUR_WG;
            uint32_t gy = (pc.region_height + EFFECT_BLUR_WG - 1u) / EFFECT_BLUR_WG;
            optics_gfx_compute_dispatch(
                &(optics_gfx_compute_dispatch_opts){.cmd = cmd,
                                                    .pipeline = pipeline,
                                                    .push_constants = &pc,
                                                    .push_bytes = sizeof(pc),
                                                    .group_x = gx,
                                                    .group_y = gy,
                                                    .group_z = 1u});
            dispatched = true;
        }
    }
    barrier_compute_write_to_read(cmd, output->image);
}

static void record_backdrop_filter(effect_state *st, optics_gfx_device *device, VkCommandBuffer cmd,
                                   optics_gfx_image *input, optics_gfx_image *half,
                                   optics_gfx_image *quarter, optics_gfx_image *output,
                                   float requested_sigma, const optics_gfx_effect_region *regions,
                                   uint32_t region_count, bool is16f) {
    float sigma = requested_sigma;
    if (!(sigma == sigma) || sigma < 0.0f)
        sigma = 0.0f;
    if (sigma > OPTICS_GFX_EFFECT_BLUR_SIGMA_MAX)
        sigma = OPTICS_GFX_EFFECT_BLUR_SIGMA_MAX;

    if (sigma == 0.0f) {
        record_backdrop_pass(st, device, cmd, input, output, 0.0f, 2u, input->width, input->height,
                             regions, region_count, is16f);
        return;
    }

    /* Two pyramid levels provide a wide UI blur with fixed work. Sigma tunes
     * the sub-texel offsets rather than growing a per-pixel kernel loop. */
    float offset = fminf(fmaxf(sigma * 0.25f, 0.5f), 2.5f);
    record_backdrop_pass(st, device, cmd, input, half, offset, 0u, input->width, input->height,
                         regions, region_count, is16f);
    record_backdrop_pass(st, device, cmd, half, quarter, offset, 0u, input->width, input->height,
                         regions, region_count, is16f);
    record_backdrop_pass(st, device, cmd, quarter, half, offset, 1u, input->width, input->height,
                         regions, region_count, is16f);
    record_backdrop_pass(st, device, cmd, half, output, offset, 1u, input->width, input->height,
                         regions, region_count, is16f);
}

optics_gfx_result optics_gfx_blur_filter_apply(optics_gfx_blur_filter *filter,
                                               const optics_gfx_blur_filter_apply_opts *opts) {
    if (!filter || !opts || !opts->frame || !opts->desc || !opts->out)
        return OPTICS_GFX_ERROR_INVALID_ARGUMENT;
    optics_gfx_frame *frame = opts->frame;
    const optics_gfx_effect_blur_desc *desc = opts->desc;
    optics_gfx_image **out = opts->out;
    *out = nullptr;
    if (desc->type != OPTICS_GFX_TYPE_EFFECT_BLUR_DESC || !desc->input) {
        OPTICS_GFX_FAIL(OPTICS_GFX_ERROR_INVALID_ARGUMENT, "invalid reusable blur descriptor");
        return OPTICS_GFX_ERROR_INVALID_ARGUMENT;
    }
    const optics_gfx_effect_blur_regions_desc *region_desc = desc->next;
    if (region_desc &&
        (region_desc->type != OPTICS_GFX_TYPE_EFFECT_BLUR_REGIONS_DESC || region_desc->next ||
         (region_desc->region_count > 0 && !region_desc->regions))) {
        OPTICS_GFX_FAIL(OPTICS_GFX_ERROR_INVALID_ARGUMENT,
                        "invalid reusable blur regions extension");
        return OPTICS_GFX_ERROR_INVALID_ARGUMENT;
    }
    if (!frame->surface || frame->state != OPTICS_GFX_FRAME_STATE_RECORDING ||
        optics_gfx_frame_has_active_pass(frame)) {
        OPTICS_GFX_FAIL(OPTICS_GFX_ERROR_INVALID_STATE,
                        "reusable blur requires a recording frame pass boundary");
        return OPTICS_GFX_ERROR_INVALID_STATE;
    }
    optics_gfx_image *input = desc->input;
    if (frame->surface->device != filter->device || input->device != filter->device) {
        OPTICS_GFX_FAIL(OPTICS_GFX_ERROR_INVALID_ARGUMENT,
                        "blur filter, frame, and input use different devices");
        return OPTICS_GFX_ERROR_INVALID_ARGUMENT;
    }
    if (input->bindless == OPTICS_GFX_BINDLESS_INVALID) {
        OPTICS_GFX_FAIL(OPTICS_GFX_ERROR_INVALID_ARGUMENT,
                        "reusable blur input lacks a sampled handle");
        return OPTICS_GFX_ERROR_INVALID_ARGUMENT;
    }
    effect_state *st = effect_state_get_or_init(filter->device);
    if (!st)
        return OPTICS_GFX_ERROR_OUT_OF_MEMORY;
    /* 8-bit SDR content takes the historic RGBA8 path; 16F working-space
     * content stays 16F so the pyramid runs in linear light (ADR-0069). */
    optics_gfx_format storage_format;
    bool is16f;
    optics_gfx_result rr = effect_resolve_storage(st, input, &storage_format, &is16f);
    if (rr != OPTICS_GFX_OK)
        return rr;
    const optics_gfx_effect_region *regions = region_desc ? region_desc->regions : nullptr;
    uint32_t region_count = region_desc ? region_desc->region_count : 0u;
    if (region_count > 0) {
        bool any_valid = false;
        for (uint32_t i = 0; i < region_count; ++i) {
            uint32_t x, y, width, height;
            if (map_effect_region(&regions[i], input->width, input->height, input->width,
                                  input->height, &x, &y, &width, &height)) {
                any_valid = true;
                break;
            }
        }
        if (!any_valid) {
            OPTICS_GFX_FAIL(OPTICS_GFX_ERROR_INVALID_ARGUMENT,
                            "reusable blur region list has no pixels inside the input");
            return OPTICS_GFX_ERROR_INVALID_ARGUMENT;
        }
    }
    uint32_t index = optics_gfx_frame_index(frame);
    if (index >= OPTICS_GFX_MAX_FRAMES_IN_FLIGHT)
        return OPTICS_GFX_ERROR_OUT_OF_RANGE;

    optics_gfx_result r = blur_filter_ensure_slot(filter, index, input, storage_format);
    if (r != OPTICS_GFX_OK)
        return r;
    r = blur_filter_ensure_scratch(filter, input->width, input->height, storage_format);
    if (r != OPTICS_GFX_OK)
        return r;

    optics_gfx_platform_mutex_lock(&st->lock);
    r = ensure_backdrop_pipeline(filter->device, st, is16f);
    optics_gfx_platform_mutex_unlock(&st->lock);
    if (r != OPTICS_GFX_OK)
        return r;

    blur_filter_slot *slot = &filter->slots[index];

    /* Register the blur as a deferred pass instead of recording it
     * (ADR-0147). The output handle is valid immediately; the four
     * dispatches are built the first time something requires this image and
     * never built if nothing does. A plan that cannot take the pass (full,
     * arena exhausted, or no command pool) falls back to recording here,
     * which is always correct — pruning is an optimisation, never a
     * correctness dependency. */
    size_t params_bytes =
        sizeof(blur_plan_params) + (size_t)region_count * sizeof(optics_gfx_effect_region);
    blur_plan_params *params =
        optics_gfx_plan_alloc_params(&frame->plan, params_bytes, alignof(blur_plan_params));
    uint32_t pass_index = UINT32_MAX;
    if (params) {
        params->st = st;
        params->device = filter->device;
        params->input = input;
        params->half = filter->half;
        params->quarter = filter->quarter;
        params->output = slot->output;
        params->sigma = desc->sigma;
        params->root_width = input->width;
        params->root_height = input->height;
        params->is16f = is16f;
        params->region_count = region_count;
        if (region_count > 0)
            memcpy(params->regions, regions,
                   (size_t)region_count * sizeof(optics_gfx_effect_region));

        optics_gfx_plan_pass_desc pass = {
            .record = blur_pass_record,
            .params = params,
            .label = "gfx.effect.blur",
            .inputs = {input},
            .input_count = 1,
            .outputs = {slot->output},
            .output_count = 1,
        };
        pass_index = optics_gfx_plan_register(&frame->plan, &pass);
    }
    if (pass_index == UINT32_MAX) {
        frame->plan.fallback_total++;
        record_backdrop_filter(st, filter->device, optics_gfx_frame_vk_command_buffer(frame), input,
                               filter->half, filter->quarter, slot->output, desc->sigma, regions,
                               region_count, is16f);
    }
    slot->valid = true;
    *out = slot->output;
    return OPTICS_GFX_OK;
}

optics_gfx_result optics_gfx_blur_filter_current(optics_gfx_blur_filter *filter,
                                                 const optics_gfx_blur_filter_current_opts *opts) {
    if (!filter || !opts || !opts->frame || !opts->out)
        return OPTICS_GFX_ERROR_INVALID_ARGUMENT;
    optics_gfx_frame *frame = opts->frame;
    optics_gfx_image **out = opts->out;
    *out = nullptr;
    if (!frame->surface || frame->state != OPTICS_GFX_FRAME_STATE_RECORDING ||
        optics_gfx_frame_has_active_pass(frame)) {
        OPTICS_GFX_FAIL(OPTICS_GFX_ERROR_INVALID_STATE,
                        "reusable blur query requires a recording frame pass boundary");
        return OPTICS_GFX_ERROR_INVALID_STATE;
    }
    uint32_t index = optics_gfx_frame_index(frame);
    if (index >= OPTICS_GFX_MAX_FRAMES_IN_FLIGHT)
        return OPTICS_GFX_ERROR_OUT_OF_RANGE;
    blur_filter_slot *slot = &filter->slots[index];
    if (!slot->valid || !slot->output)
        return OPTICS_GFX_ERROR_INVALID_STATE;
    /* ADR-0147: `valid` only says this slot's output belongs to the current
     * recording; the pass that writes it may still be deferred. Handing back
     * a pending image would let the caller sample last frame's pixels, so the
     * query refuses until the work has actually run. */
    if (optics_gfx_plan_image_pending(&frame->plan, slot->output)) {
        OPTICS_GFX_FAIL(
            OPTICS_GFX_ERROR_INVALID_STATE,
            "reusable blur output is deferred and not executed yet; materialize it first");
        return OPTICS_GFX_ERROR_INVALID_STATE;
    }
    *out = slot->output;
    return OPTICS_GFX_OK;
}

/* ------------------------------------------------------------------ */
/*  Reusable frame-slot realtime shadow                               */
/* ------------------------------------------------------------------ */

optics_gfx_result optics_gfx_shadow_filter_create(optics_gfx_device *device,
                                                  optics_gfx_shadow_filter **out) {
    if (!device || !out)
        return OPTICS_GFX_ERROR_INVALID_ARGUMENT;
    *out = nullptr;
    optics_gfx_shadow_filter *filter = optics_gfx_internal_alloc(device, sizeof(*filter));
    if (!filter)
        return OPTICS_GFX_ERROR_OUT_OF_MEMORY;
    atomic_init(&filter->ref_count, 1u);
    filter->device = optics_gfx_device_retain(device);
    *out = filter;
    return OPTICS_GFX_OK;
}

optics_gfx_shadow_filter *optics_gfx_shadow_filter_retain(optics_gfx_shadow_filter *filter) {
    if (filter)
        atomic_fetch_add_explicit(&filter->ref_count, 1u, memory_order_relaxed);
    return filter;
}

void optics_gfx_shadow_filter_release(optics_gfx_shadow_filter *filter) {
    if (!filter)
        return;
    if (atomic_fetch_sub_explicit(&filter->ref_count, 1u, memory_order_acq_rel) != 1u)
        return;
    optics_gfx_device *device = filter->device;
    if (filter->pong)
        optics_gfx_image_release(filter->pong);
    if (filter->ping)
        optics_gfx_image_release(filter->ping);
    for (uint32_t i = 0; i < OPTICS_GFX_MAX_FRAMES_IN_FLIGHT; ++i) {
        if (filter->slots[i].output)
            optics_gfx_image_release(filter->slots[i].output);
    }
    optics_gfx_internal_free(device, filter);
    optics_gfx_device_release(device);
}

static optics_gfx_result shadow_filter_ensure_scratch(optics_gfx_shadow_filter *filter,
                                                      uint32_t in_w, uint32_t in_h,
                                                      optics_gfx_format storage_format) {
    if (filter->ping && filter->pong && filter->scratch_width == in_w &&
        filter->scratch_height == in_h && filter->scratch_format == storage_format)
        return OPTICS_GFX_OK;

    if (filter->pong)
        optics_gfx_image_release(filter->pong);
    if (filter->ping)
        optics_gfx_image_release(filter->ping);
    filter->ping = nullptr;
    filter->pong = nullptr;

    optics_gfx_result r = optics_gfx_image_create(
        filter->device,
        &(optics_gfx_image_desc){.type = OPTICS_GFX_TYPE_IMAGE_DESC,
                                 .width = in_w,
                                 .height = in_h,
                                 .format = storage_format,
                                 .usage = OPTICS_GFX_IMAGE_USAGE_COMPUTE_WRITE},
        &filter->ping);
    if (r != OPTICS_GFX_OK)
        return r;
    r = optics_gfx_image_create(
        filter->device,
        &(optics_gfx_image_desc){.type = OPTICS_GFX_TYPE_IMAGE_DESC,
                                 .width = in_w,
                                 .height = in_h,
                                 .format = storage_format,
                                 .usage = OPTICS_GFX_IMAGE_USAGE_COMPUTE_WRITE},
        &filter->pong);
    if (r != OPTICS_GFX_OK) {
        optics_gfx_image_release(filter->ping);
        filter->ping = nullptr;
        return r;
    }
    filter->scratch_width = in_w;
    filter->scratch_height = in_h;
    filter->scratch_format = storage_format;
    return OPTICS_GFX_OK;
}

static optics_gfx_result shadow_filter_ensure_slot(optics_gfx_shadow_filter *filter, uint32_t index,
                                                   const optics_gfx_image *input,
                                                   optics_gfx_format storage_format) {
    shadow_filter_slot *slot = &filter->slots[index];
    if (slot->output && slot->width == input->width && slot->height == input->height &&
        slot->format == storage_format)
        return OPTICS_GFX_OK;

    if (slot->output)
        optics_gfx_image_release(slot->output);
    *slot = (shadow_filter_slot){};

    optics_gfx_result r = optics_gfx_image_create(
        filter->device,
        &(optics_gfx_image_desc){.type = OPTICS_GFX_TYPE_IMAGE_DESC,
                                 .width = input->width,
                                 .height = input->height,
                                 .format = storage_format,
                                 .usage = OPTICS_GFX_IMAGE_USAGE_COMPUTE_WRITE},
        &slot->output);
    if (r != OPTICS_GFX_OK)
        return r;
    slot->width = input->width;
    slot->height = input->height;
    slot->format = storage_format;
    return OPTICS_GFX_OK;
}

optics_gfx_result optics_gfx_shadow_filter_apply(optics_gfx_shadow_filter *filter,
                                                 const optics_gfx_shadow_filter_apply_opts *opts) {
    if (!filter || !opts || !opts->frame || !opts->desc || !opts->out)
        return OPTICS_GFX_ERROR_INVALID_ARGUMENT;
    optics_gfx_frame *frame = opts->frame;
    const optics_gfx_effect_shadow_desc *desc = opts->desc;
    optics_gfx_image **out = opts->out;
    *out = nullptr;
    if (desc->type != OPTICS_GFX_TYPE_EFFECT_SHADOW_DESC || !desc->input) {
        OPTICS_GFX_FAIL(OPTICS_GFX_ERROR_INVALID_ARGUMENT, "invalid reusable shadow descriptor");
        return OPTICS_GFX_ERROR_INVALID_ARGUMENT;
    }
    if (desc->next) {
        OPTICS_GFX_FAIL(OPTICS_GFX_ERROR_INVALID_ARGUMENT,
                        "reusable shadow does not accept extensions");
        return OPTICS_GFX_ERROR_INVALID_ARGUMENT;
    }
    /* Finite check before the frame dereference: callers validating against
     * a not-yet-started frame (null or placeholder) must fail cleanly, not
     * crash. Mirrors the exact operator's ordering. */
    if (!effect_finite(desc->offset_x) || !effect_finite(desc->offset_y) ||
        !effect_finite(desc->tint_red) || !effect_finite(desc->tint_green) ||
        !effect_finite(desc->tint_blue) || !effect_finite(desc->alpha) ||
        !effect_finite(desc->blur)) {
        OPTICS_GFX_FAIL(OPTICS_GFX_ERROR_INVALID_ARGUMENT, "shadow parameters must be finite");
        return OPTICS_GFX_ERROR_INVALID_ARGUMENT;
    }
    if (!frame->surface || frame->state != OPTICS_GFX_FRAME_STATE_RECORDING ||
        optics_gfx_frame_has_active_pass(frame)) {
        OPTICS_GFX_FAIL(OPTICS_GFX_ERROR_INVALID_STATE,
                        "reusable shadow requires a recording frame pass boundary");
        return OPTICS_GFX_ERROR_INVALID_STATE;
    }
    optics_gfx_image *input = desc->input;
    if (frame->surface->device != filter->device || input->device != filter->device) {
        OPTICS_GFX_FAIL(OPTICS_GFX_ERROR_INVALID_ARGUMENT,
                        "shadow filter, frame, and input use different devices");
        return OPTICS_GFX_ERROR_INVALID_ARGUMENT;
    }
    if (input->bindless == OPTICS_GFX_BINDLESS_INVALID) {
        OPTICS_GFX_FAIL(OPTICS_GFX_ERROR_INVALID_ARGUMENT,
                        "reusable shadow input lacks a sampled handle");
        return OPTICS_GFX_ERROR_INVALID_ARGUMENT;
    }
    effect_state *st = effect_state_get_or_init(filter->device);
    if (!st)
        return OPTICS_GFX_ERROR_OUT_OF_MEMORY;
    optics_gfx_format storage_format;
    bool is16f;
    optics_gfx_result rr = effect_resolve_storage(st, input, &storage_format, &is16f);
    if (rr != OPTICS_GFX_OK)
        return rr;
    uint32_t index = optics_gfx_frame_index(frame);
    if (index >= OPTICS_GFX_MAX_FRAMES_IN_FLIGHT)
        return OPTICS_GFX_ERROR_OUT_OF_RANGE;

    optics_gfx_result r = shadow_filter_ensure_slot(filter, index, input, storage_format);
    if (r != OPTICS_GFX_OK)
        return r;
    r = shadow_filter_ensure_scratch(filter, input->width, input->height, storage_format);
    if (r != OPTICS_GFX_OK)
        return r;

    optics_gfx_platform_mutex_lock(&st->lock);
    r = ensure_shadow_pipeline(filter->device, st, is16f);
    optics_gfx_platform_mutex_unlock(&st->lock);
    if (r != OPTICS_GFX_OK)
        return r;

    shadow_filter_slot *slot = &filter->slots[index];

    /* Register the shadow as a deferred pass, exactly as the blur does
     * (ADR-0147). A shadow whose output nothing samples costs nothing; the
     * eager dispatch below is the fallback when the plan cannot take it. */
    shadow_plan_params *params =
        optics_gfx_plan_alloc_params(&frame->plan, sizeof(*params), alignof(shadow_plan_params));
    uint32_t pass_index = UINT32_MAX;
    if (params) {
        params->st = st;
        params->device = filter->device;
        params->mask = input;
        params->ping = filter->ping;
        params->pong = filter->pong;
        params->output = slot->output;
        params->desc = *desc;
        optics_gfx_plan_pass_desc pass = {
            .record = shadow_pass_record,
            .params = params,
            .label = "gfx.effect.shadow",
            .inputs = {input},
            .input_count = 1,
            .outputs = {slot->output},
            .output_count = 1,
        };
        pass_index = optics_gfx_plan_register(&frame->plan, &pass);
    }
    if (pass_index == UINT32_MAX) {
        frame->plan.fallback_total++;
        r = record_shadow_dispatch(st, filter->device, optics_gfx_frame_vk_command_buffer(frame),
                                   input, filter->ping, filter->pong, slot->output, desc, is16f);
        if (r != OPTICS_GFX_OK)
            return r;
    }
    *out = slot->output;
    return OPTICS_GFX_OK;
}

/* ------------------------------------------------------------------ */
/*  Public: optics_gfx_effect_promote                                       */
/* ------------------------------------------------------------------ */

static optics_gfx_result promote_copy_submit(optics_gfx_device *d, VkImage src_image,
                                             VkImage dst_image, uint32_t width, uint32_t height) {
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkResult vr = optics_gfx_vk_new_transient_cmd(d, d->graphics_family, &pool, &cmd);
    if (vr != VK_SUCCESS) {
        OPTICS_GFX_FAIL_VK(OPTICS_GFX_ERROR_BACKEND_FAILURE,
                           "promote command buffer allocation failed", vr);
        return OPTICS_GFX_ERROR_BACKEND_FAILURE;
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
        /* dst: SHADER_READ_ONLY (optics_gfx_image_create's terminal layout) → TRANSFER_DST */
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
        vr = optics_gfx_vk_submit_one_shot_and_wait(d, cmd);
    vkDestroyCommandPool(d->device, pool, nullptr);

    if (vr != VK_SUCCESS) {
        optics_gfx_result r = vr == VK_TIMEOUT             ? OPTICS_GFX_ERROR_TIMEOUT
                              : vr == VK_ERROR_DEVICE_LOST ? OPTICS_GFX_ERROR_DEVICE_LOST
                                                           : OPTICS_GFX_ERROR_BACKEND_FAILURE;
        OPTICS_GFX_FAIL_VK(r, "promote copy submit failed", vr);
        return r;
    }
    return OPTICS_GFX_OK;
}

optics_gfx_result optics_gfx_effect_promote(optics_gfx_image *transient, optics_gfx_image **out) {
    if (!transient || !out)
        return OPTICS_GFX_ERROR_INVALID_ARGUMENT;
    if (transient->bindless_storage == OPTICS_GFX_BINDLESS_INVALID) {
        OPTICS_GFX_FAIL(
            OPTICS_GFX_ERROR_INVALID_ARGUMENT,
            "optics_gfx_effect_promote source is not an effect output (no storage handle)");
        return OPTICS_GFX_ERROR_INVALID_ARGUMENT;
    }
    *out = nullptr;

    optics_gfx_device *d = transient->device;
    optics_gfx_image_desc ddesc = OPTICS_GFX_IMAGE_DESC_INIT;
    ddesc.width = transient->width;
    ddesc.height = transient->height;
    ddesc.format = transient->format;
    optics_gfx_image *dst = nullptr;
    optics_gfx_result r = optics_gfx_image_create(d, &ddesc, &dst);
    if (r != OPTICS_GFX_OK)
        return r;

    r = promote_copy_submit(d, transient->image, dst->image, transient->width, transient->height);
    if (r != OPTICS_GFX_OK) {
        optics_gfx_image_release(dst);
        return r;
    }
    *out = dst;
    return OPTICS_GFX_OK;
}

/* ------------------------------------------------------------------ */
/*  Public: optics_gfx_effect_reset                                         */
/* ------------------------------------------------------------------ */

void optics_gfx_effect_reset(optics_gfx_device *d) {
    if (!d)
        return;
    optics_gfx_platform_mutex_lock(&d->module_state_lock);
    effect_state *st = d->effect_state;
    optics_gfx_platform_mutex_unlock(&d->module_state_lock);
    if (!st)
        return;
    optics_gfx_platform_mutex_lock(&st->lock);
    for (output_entry *o = st->outputs; o; o = o->next)
        o->leased = false;
    for (intermediate_entry *e = st->intermediates; e; e = e->next)
        e->leased = false;
    optics_gfx_platform_mutex_unlock(&st->lock);
}
