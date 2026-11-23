/*
 * gfx/effect.h — image-domain effects (blur, shadow, tone-map). *
 * Effects read optics_gfx_image inputs, write a optics_gfx_image output, and
 * are recordable into any VkCommandBuffer. Inside a frame, pull
 * the cmd buffer from optics_gfx_frame_vk_command_buffer(); outside a
 * frame, the caller supplies a one-shot command buffer.
 *
 * Intended consumers: material (the material library) and end-user
 * showcases/effects. This header is the seam where gfx (rendering
 * mechanism) meets named materials (ADR-0063): gfx owns the effect
 * runtime, material owns material identity. It is public by design —
 * applications may compose custom image-domain effects — but there is
 * no stable-API commitment beyond the stack's own deprecation policy
 * (docs/reference/api.md).
 *
 * Output ownership and lifetime
 *   The effect owns the output image. It is leased exclusively from a
 *   per-device pool keyed by (format, width, height). A writable intermediate
 *   or output is never reused by another command buffer in the same reset
 *   epoch. The returned optics_gfx_image * remains valid until
 *   optics_gfx_effect_reset(device) or device destruction.
 *
 *   To take a long-lived copy, run the effect and then
 *   optics_gfx_effect_promote() the leased image into a caller-owned,
 *   refcounted optics_gfx_image (see below).
 *
 * Threading
 *   Effects are not thread-safe per device. Serialize calls per
 *   device, as you would for any other recording API.
 *
 * See ADR-0008 for the design rationale.
 */

#ifndef OPTICS_GFX_EFFECT_H
#define OPTICS_GFX_EFFECT_H

#include <optics/gfx/canvas.h> /* optics_gfx_image */
#include <optics/gfx/core.h>
#include <optics/gfx/vulkan.h> /* VkCommandBuffer */

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/*  Gaussian blur                                                     */
/* ------------------------------------------------------------------ */

/* Separable two-pass Gaussian blur. The output image has the same
 * dimensions as the input; its format follows the input: RGBA8_UNORM
 * for 8-bit SDR content, RGBA16_SFLOAT for 16F working-space content
 * (ADR-0069 — linear-light blurring, requires rgba16f storage support;
 * a 16F input on a device without it fails with OPTICS_GFX_ERROR_UNSUPPORTED).
 * Kernel radius is derived
 * from sigma; sigma is clamped to [0, OPTICS_GFX_EFFECT_BLUR_SIGMA_MAX]
 * — values outside that range are clamped silently.
 *
 * A sigma of 0 is a no-op (the output aliases the input's contents
 * via a direct copy; no kernel work runs). */
#define OPTICS_GFX_EFFECT_BLUR_SIGMA_MAX 64.0f

/* One input-pixel dispatch region for the reusable realtime blur. The filter
 * maps it outward through every pyramid level and leaves pixels outside all
 * regions untouched. Because later pyramid passes sample neighbouring
 * intermediate pixels, callers must expand each region by the blur sampling
 * footprint and consume only the unexpanded interior. This lets compositors
 * blur disjoint chrome bands (for example a top HUD and bottom Dock) without
 * dispatching over the empty full-screen bounding box between them. */
typedef struct optics_gfx_effect_region {
    uint32_t x;
    uint32_t y;
    uint32_t width;
    uint32_t height;
} optics_gfx_effect_region;

typedef struct optics_gfx_effect_blur_desc {
    optics_gfx_struct_type type; /* OPTICS_GFX_TYPE_EFFECT_BLUR_DESC */
    const void *next;
    optics_gfx_image *input; /* borrowed, not retained; safe to release right
                              * after the call — destruction defers through the
                              * device retire queue past the recorded dispatch */
    float sigma;             /* Gaussian sigma in pixels */
} optics_gfx_effect_blur_desc;

#define OPTICS_GFX_EFFECT_BLUR_DESC_INIT {.type = OPTICS_GFX_TYPE_EFFECT_BLUR_DESC}

/* Optional `optics_gfx_effect_blur_desc.next` payload for region-aware reusable
 * blur. Keeping this in the extension chain preserves the binary size and
 * layout of optics_gfx_effect_blur_desc. A NULL extension means the full input.
 * This extension is accepted by optics_gfx_blur_filter_apply only; the exact
 * optics_gfx_effect_blur operator rejects it. */
typedef struct optics_gfx_effect_blur_regions_desc {
    optics_gfx_struct_type type; /* OPTICS_GFX_TYPE_EFFECT_BLUR_REGIONS_DESC */
    const void *next;
    const optics_gfx_effect_region *regions;
    uint32_t region_count;
} optics_gfx_effect_blur_regions_desc;

#define OPTICS_GFX_EFFECT_BLUR_REGIONS_DESC_INIT {.type = OPTICS_GFX_TYPE_EFFECT_BLUR_REGIONS_DESC}

/* Record the blur into `cmd`. The pipeline is created lazily on
 * first use per device and cached for the device's lifetime.
 *
 * Errors:
 *   OPTICS_GFX_ERROR_INVALID_ARGUMENT — null cmd/desc/out, wrong type,
 *     null input, or input format unsupported by the operator.
 *   OPTICS_GFX_ERROR_OUT_OF_MEMORY    — transient pool exhausted.
 *   OPTICS_GFX_ERROR_BACKEND_FAILURE  — Vulkan pipeline or image
 *     creation failed. */
typedef struct optics_gfx_effect_blur_opts {
    VkCommandBuffer cmd;
    const optics_gfx_effect_blur_desc *desc;
    optics_gfx_image **out;
} optics_gfx_effect_blur_opts;

OPTICS_GFX_NODISCARD OPTICS_GFX_API optics_gfx_result
optics_gfx_effect_blur(const optics_gfx_effect_blur_opts *opts);

/* Reusable, frame-slot-safe realtime blur. Unlike optics_gfx_effect_blur's exact
 * Gaussian kernel, this path uses a two-level Dual-Kawase pyramid: two
 * downsample and two upsample passes with a fixed sample count. Requested
 * sigma controls sample offsets instead of loop length, so an animated
 * compositor cannot accidentally create an unbounded full-screen dispatch.
 * A filter owns its pyramid/output images per frame-in-flight slot and needs
 * no transient-pool growth or whole-device wait. Use one filter with one
 * surface/frame stream; begin_frame has already waited for the selected slot
 * before that slot is reused. Output follows the input: RGBA8_UNORM for
 * 8-bit SDR content, RGBA16_SFLOAT for 16F working-space content (with the
 * input dimensions).
 *
 * The image returned by apply is borrowed from the filter and remains valid
 * until the same slot is applied again, the filter is released, or its input
 * extent/format changes. Do not release the returned image.
 *
 * The work is DEFERRED (ADR-0147): apply registers a pass and returns the
 * handle immediately, but the four dispatches are only built when something
 * requires the pixels. A pass nothing samples is pruned at submit and costs
 * no GPU time, so callers do not need to guard the call by hand. Consumers
 * that record their own compute (materials, custom passes) call
 * optics_gfx_frame_materialize_image at their boundary; a canvas draw that samples
 * a still-pending output is rejected with OPTICS_GFX_ERROR_INVALID_STATE. */
typedef struct optics_gfx_blur_filter optics_gfx_blur_filter;

typedef struct optics_gfx_blur_filter_apply_opts {
    optics_gfx_frame *frame;
    const optics_gfx_effect_blur_desc *desc;
    optics_gfx_image **out;
} optics_gfx_blur_filter_apply_opts;

typedef struct optics_gfx_blur_filter_current_opts {
    optics_gfx_frame *frame;
    optics_gfx_image **out;
} optics_gfx_blur_filter_current_opts;

OPTICS_GFX_NODISCARD OPTICS_GFX_API optics_gfx_result
optics_gfx_blur_filter_create(optics_gfx_device *device, optics_gfx_blur_filter **out);
OPTICS_GFX_NODISCARD OPTICS_GFX_API optics_gfx_blur_filter *
optics_gfx_blur_filter_retain(optics_gfx_blur_filter *filter);
OPTICS_GFX_API void optics_gfx_blur_filter_release(optics_gfx_blur_filter *filter);
OPTICS_GFX_NODISCARD OPTICS_GFX_API optics_gfx_result optics_gfx_blur_filter_apply(
    optics_gfx_blur_filter *filter, const optics_gfx_blur_filter_apply_opts *opts);

/* Borrow the slot's current valid blurred output without re-registering the
 * Dual-Kawase pyramid dispatch.
 *
 * ADR-0147: the returned image may belong to a deferred pass that has not
 * been executed yet. Materialize it (`optics_gfx_frame_materialize_image`) before
 * anything samples it; a draw inside a canvas pass that reaches a pending
 * output is rejected.
 *
 * Errors:
 *   OPTICS_GFX_ERROR_INVALID_ARGUMENT — null filter/opts/frame/out.
 *   OPTICS_GFX_ERROR_INVALID_STATE    — frame not recording, or slot has no
 *     valid recorded blur output.
 *   OPTICS_GFX_ERROR_OUT_OF_RANGE     — frame slot index >= OPTICS_GFX_MAX_FRAMES_IN_FLIGHT. */
OPTICS_GFX_NODISCARD OPTICS_GFX_API optics_gfx_result optics_gfx_blur_filter_current(
    optics_gfx_blur_filter *filter, const optics_gfx_blur_filter_current_opts *opts);

/* ------------------------------------------------------------------ */
/*  Drop shadow                                                       */
/* ------------------------------------------------------------------ */

/* Drop shadow from a shape mask. The input is a mask image: an RGBA8/
 * BGRA8 image whose alpha channel is the shape coverage (a colour
 * image works — only alpha is read; an opaque-on-transparent canvas
 * capture is the typical source), or an RGBA16_SFLOAT image (ADR-0069
 * path, requires rgba16f storage support). The output is a
 * premultiplied tinted shadow of the same extent: rgb = tint * a.
 *
 * The operator is geometry-neutral (ADR-0074): shape geometry arrives
 * only through the mask. Building the mask is the caller's job — the
 * canvas already draws rounded rects, and material owns its own SDF path.
 *
 * The blur is the same separable two-pass Gaussian as optics_gfx_effect_blur;
 * OPTICS_GFX_EFFECT_SHADOW_BLUR_MAX bounds it. A sigma of 0 produces a hard
 * (unblurred) offset copy of the mask. `offset_x`/`offset_y` are in
 * input pixels, +x right, +y down; the shadow is sampled at
 * (p - offset), so positive y moves the shadow down. `tint_red`/
 * `tint_green`/`tint_blue` are straight (unpremultiplied) colour in the
 * input's space; `alpha` scales coverage and is clamped to [0,1].
 *
 * Animation safety (ADR-0074): dispatch shape is fixed per extent;
 * sigma/offset/tint/alpha vary per frame without pipeline or
 * allocation churn beyond the transient pool's normal lease path.
 *
 * Errors:
 *   OPTICS_GFX_ERROR_INVALID_ARGUMENT — null cmd/desc/out, wrong type, null
 *     or handle-less input, or non-finite offset/tint/alpha/blur.
 *   OPTICS_GFX_ERROR_OUT_OF_MEMORY    — transient pool exhausted.
 *   OPTICS_GFX_ERROR_BACKEND_FAILURE  — pipeline or image creation failed.
 *   OPTICS_GFX_ERROR_UNSUPPORTED      — 16F input on a device without
 *     rgba16f storage, or an unsupported input format. */
#define OPTICS_GFX_EFFECT_SHADOW_BLUR_MAX 64.0f

typedef struct optics_gfx_effect_shadow_desc {
    optics_gfx_struct_type type; /* OPTICS_GFX_TYPE_EFFECT_SHADOW_DESC */
    const void *next;            /* must be NULL (no extensions yet) */
    optics_gfx_image *input;     /* borrowed, not retained; safe to release right
                                  * after the call — destruction defers through the
                                  * device retire queue past the recorded dispatch */
    float blur;                  /* Gaussian sigma in pixels, [0,
                                  * OPTICS_GFX_EFFECT_SHADOW_BLUR_MAX] */
    float offset_x;              /* shadow offset, pixels, +x right */
    float offset_y;              /* shadow offset, pixels, +y down */
    float tint_red;              /* straight colour, input colour space */
    float tint_green;
    float tint_blue;
    float alpha; /* shadow opacity, clamped to [0,1] */
} optics_gfx_effect_shadow_desc;

#define OPTICS_GFX_EFFECT_SHADOW_DESC_INIT {.type = OPTICS_GFX_TYPE_EFFECT_SHADOW_DESC}

/* Record the shadow into `cmd`. The pipeline is created lazily on first
 * use per device and cached for the device's lifetime. Output follows
 * the input's storage class (RGBA8_UNORM or RGBA16_SFLOAT) at the
 * input's extent, leased from the per-device transient pool like any
 * other effect output (see "Output ownership and lifetime" above). */
typedef struct optics_gfx_effect_shadow_opts {
    VkCommandBuffer cmd;
    const optics_gfx_effect_shadow_desc *desc;
    optics_gfx_image **out;
} optics_gfx_effect_shadow_opts;

OPTICS_GFX_NODISCARD OPTICS_GFX_API optics_gfx_result
optics_gfx_effect_shadow(const optics_gfx_effect_shadow_opts *opts);

/* Reusable, frame-slot-safe realtime shadow. The exact optics_gfx_effect_shadow
 * leases its intermediates from the per-device transient pool, so a caller
 * driving it every frame would hold one lease per dispatch until
 * optics_gfx_effect_reset — wrong for a live compositor that never resets. This
 * filter instead owns its two intermediates and output per frame-in-flight
 * slot (the same ownership model as optics_gfx_blur_filter): begin_frame has
 * already waited for the selected slot before that slot is reused, so no
 * transient-pool growth, lease accumulation, or device-wide wait occurs.
 * Dispatch shape is fixed per extent; blur/offset/tint/alpha vary per frame
 * without pipeline churn (ADR-0074).
 *
 * Accepts the same optics_gfx_effect_shadow_desc as optics_gfx_effect_shadow (its `next`
 * must be NULL). The image returned by apply is borrowed from the filter and
 * remains valid until the same slot is applied again, the filter is
 * released, or its input extent/format changes. Do not release it.
 *
 * Deferred like the blur (ADR-0147): nothing samples the shadow, nothing
 * runs. Materialize it at a pass boundary before a canvas draw consumes it. */
typedef struct optics_gfx_shadow_filter optics_gfx_shadow_filter;

typedef struct optics_gfx_shadow_filter_apply_opts {
    optics_gfx_frame *frame;
    const optics_gfx_effect_shadow_desc *desc;
    optics_gfx_image **out;
} optics_gfx_shadow_filter_apply_opts;

OPTICS_GFX_NODISCARD OPTICS_GFX_API optics_gfx_result
optics_gfx_shadow_filter_create(optics_gfx_device *device, optics_gfx_shadow_filter **out);
OPTICS_GFX_NODISCARD OPTICS_GFX_API optics_gfx_shadow_filter *
optics_gfx_shadow_filter_retain(optics_gfx_shadow_filter *filter);
OPTICS_GFX_API void optics_gfx_shadow_filter_release(optics_gfx_shadow_filter *filter);
OPTICS_GFX_NODISCARD OPTICS_GFX_API optics_gfx_result optics_gfx_shadow_filter_apply(
    optics_gfx_shadow_filter *filter, const optics_gfx_shadow_filter_apply_opts *opts);

/* ------------------------------------------------------------------ */
/*  Promote a transient output to a caller-owned image                */
/* ------------------------------------------------------------------ */

/* Copy `transient` (an image returned by optics_gfx_effect_blur or any
 * later effect operator) into a fresh caller-owned optics_gfx_image with
 * the same width, height, and format. The returned image has the
 * regular optics_gfx_image lifecycle: refcount 1, bindless-sampled,
 * released with optics_gfx_image_release, unaffected by optics_gfx_effect_reset.
 *
 * Synchronous: records and submits a one-shot graphics-queue
 * command buffer and waits for completion. Caller must have
 * synchronised any prior submission that wrote `transient` (a
 * vkQueueWaitIdle, vkDeviceWaitIdle, or fence wait) before this
 * call — promote does not re-synchronise on the caller's behalf.
 *
 * Errors:
 *   OPTICS_GFX_ERROR_INVALID_ARGUMENT — null transient/out, or `transient`
 *     is not an effect output (no storage bindless handle).
 *   OPTICS_GFX_ERROR_OUT_OF_MEMORY    — destination image allocation failed.
 *   OPTICS_GFX_ERROR_BACKEND_FAILURE  — Vulkan command-buffer or queue
 *     submission failed. */
OPTICS_GFX_NODISCARD OPTICS_GFX_API optics_gfx_result
optics_gfx_effect_promote(optics_gfx_image *transient, optics_gfx_image **out);

/* ------------------------------------------------------------------ */
/*  Lifecycle                                                         */
/* ------------------------------------------------------------------ */

/* End the current effect lease epoch and return its intermediate/output slots
 * to the per-device pool. Every command buffer that references an effect image
 * must already have completed, and no recorded command buffer that references
 * one may be submitted later. With multiple frames in flight, a CPU frame
 * boundary is not sufficient; wait all relevant fences or call
 * optics_gfx_device_wait_idle first. Old output pointers are invalid after reset. */
OPTICS_GFX_API void optics_gfx_effect_reset(optics_gfx_device *device);

#ifdef __cplusplus
}
#endif

#endif /* OPTICS_GFX_EFFECT_H */
