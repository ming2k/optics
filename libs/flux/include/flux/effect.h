/*
 * flux/effect.h — image-domain effects (blur, shadow, tone-map).
 *
 * Effects read flux_image inputs, write a flux_image output, and
 * are recordable into any VkCommandBuffer. Inside a frame, pull
 * the cmd buffer from flux_frame_vk_command_buffer(); outside a
 * frame, the caller supplies a one-shot command buffer.
 *
 * Output ownership and lifetime
 *   The effect owns the output image. It is leased exclusively from a
 *   per-device pool keyed by (format, width, height). A writable intermediate
 *   or output is never reused by another command buffer in the same reset
 *   epoch. The returned flux_image * remains valid until
 *   flux_effect_reset(device) or device destruction.
 *
 *   To take a long-lived copy, run the effect and then copy the
 *   result into a caller-owned flux_image via the canvas/image
 *   surface (a dedicated "promote to owned" helper may land
 *   later if a real consumer needs it).
 *
 * Threading
 *   Effects are not thread-safe per device. Serialize calls per
 *   device, as you would for any other recording API.
 *
 * See ADR-0008 for the design rationale.
 */

#ifndef FLUX_EFFECT_H
#define FLUX_EFFECT_H

#include <flux/canvas.h> /* flux_image */
#include <flux/core.h>
#include <flux/vulkan.h> /* VkCommandBuffer */

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/*  Gaussian blur                                                     */
/* ------------------------------------------------------------------ */

/* Separable two-pass Gaussian blur. The output image has the same
 * format and dimensions as the input. Kernel radius is derived
 * from sigma; sigma is clamped to [0, FLUX_EFFECT_BLUR_SIGMA_MAX]
 * — values outside that range are clamped silently.
 *
 * A sigma of 0 is a no-op (the output aliases the input's contents
 * via a direct copy; no kernel work runs). */
#define FLUX_EFFECT_BLUR_SIGMA_MAX 64.0f

typedef struct flux_effect_blur_desc {
    flux_struct_type type; /* FLUX_TYPE_EFFECT_BLUR_DESC */
    const void *next;
    flux_image *input; /* retained for the dispatch */
    float sigma;       /* Gaussian sigma in pixels */
} flux_effect_blur_desc;

#define FLUX_EFFECT_BLUR_DESC_INIT {.type = FLUX_TYPE_EFFECT_BLUR_DESC}

/* Record the blur into `cmd`. The pipeline is created lazily on
 * first use per device and cached for the device's lifetime.
 *
 * Errors:
 *   FLUX_ERROR_INVALID_ARGUMENT — null cmd/desc/out, wrong type,
 *     null input, or input format unsupported by the operator.
 *   FLUX_ERROR_OUT_OF_MEMORY    — transient pool exhausted.
 *   FLUX_ERROR_BACKEND_FAILURE  — Vulkan pipeline or image
 *     creation failed. */
FLUX_NODISCARD FLUX_API flux_result flux_effect_blur(VkCommandBuffer cmd,
                                                     const flux_effect_blur_desc *desc,
                                                     flux_image **out);

/* ------------------------------------------------------------------ */
/*  Promote a transient output to a caller-owned image                */
/* ------------------------------------------------------------------ */

/* Copy `transient` (an image returned by flux_effect_blur or any
 * later effect operator) into a fresh caller-owned flux_image with
 * the same width, height, and format. The returned image has the
 * regular flux_image lifecycle: refcount 1, bindless-sampled,
 * released with flux_image_release, unaffected by flux_effect_reset.
 *
 * Synchronous: records and submits a one-shot graphics-queue
 * command buffer and waits for completion. Caller must have
 * synchronised any prior submission that wrote `transient` (a
 * vkQueueWaitIdle, vkDeviceWaitIdle, or fence wait) before this
 * call — promote does not re-synchronise on the caller's behalf.
 *
 * Errors:
 *   FLUX_ERROR_INVALID_ARGUMENT — null transient/out, or `transient`
 *     is not an effect output (no storage bindless handle).
 *   FLUX_ERROR_OUT_OF_MEMORY    — destination image allocation failed.
 *   FLUX_ERROR_BACKEND_FAILURE  — Vulkan command-buffer or queue
 *     submission failed. */
FLUX_NODISCARD FLUX_API flux_result flux_effect_promote(flux_image *transient, flux_image **out);

/* ------------------------------------------------------------------ */
/*  Lifecycle                                                         */
/* ------------------------------------------------------------------ */

/* End the current effect lease epoch and return its intermediate/output slots
 * to the per-device pool. Every command buffer that references an effect image
 * must already have completed, and no recorded command buffer that references
 * one may be submitted later. With multiple frames in flight, a CPU frame
 * boundary is not sufficient; wait all relevant fences or call
 * flux_device_wait_idle first. Old output pointers are invalid after reset. */
FLUX_API void flux_effect_reset(flux_device *device);

#ifdef __cplusplus
}
#endif

#endif /* FLUX_EFFECT_H */
