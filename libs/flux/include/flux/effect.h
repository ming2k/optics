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

/* Separable two-pass Gaussian blur. The output image has RGBA8_UNORM format
 * and the same dimensions as the input. Normalizing BGRA inputs before the
 * storage write keeps the shader's declared rgba8 format valid on strict
 * Vulkan drivers. Kernel radius is derived
 * from sigma; sigma is clamped to [0, FLUX_EFFECT_BLUR_SIGMA_MAX]
 * — values outside that range are clamped silently.
 *
 * A sigma of 0 is a no-op (the output aliases the input's contents
 * via a direct copy; no kernel work runs). */
#define FLUX_EFFECT_BLUR_SIGMA_MAX 64.0f

/* One input-pixel dispatch region for the reusable realtime blur. The filter
 * maps it outward through every pyramid level and leaves pixels outside all
 * regions untouched. Because later pyramid passes sample neighbouring
 * intermediate pixels, callers must expand each region by the blur sampling
 * footprint and consume only the unexpanded interior. This lets compositors
 * blur disjoint chrome bands (for example a top HUD and bottom Dock) without
 * dispatching over the empty full-screen bounding box between them. */
typedef struct flux_effect_region {
    uint32_t x;
    uint32_t y;
    uint32_t width;
    uint32_t height;
} flux_effect_region;

typedef struct flux_effect_blur_desc {
    flux_struct_type type; /* FLUX_TYPE_EFFECT_BLUR_DESC */
    const void *next;
    flux_image *input; /* retained for the dispatch */
    float sigma;       /* Gaussian sigma in pixels */
} flux_effect_blur_desc;

#define FLUX_EFFECT_BLUR_DESC_INIT {.type = FLUX_TYPE_EFFECT_BLUR_DESC}

/* Optional `flux_effect_blur_desc.next` payload for region-aware reusable
 * blur. Keeping this in the extension chain preserves the binary size and
 * layout of flux_effect_blur_desc. A NULL extension means the full input.
 * This extension is accepted by flux_blur_filter_apply only; the exact
 * flux_effect_blur operator rejects it. */
typedef struct flux_effect_blur_regions_desc {
    flux_struct_type type; /* FLUX_TYPE_EFFECT_BLUR_REGIONS_DESC */
    const void *next;
    const flux_effect_region *regions;
    uint32_t region_count;
} flux_effect_blur_regions_desc;

#define FLUX_EFFECT_BLUR_REGIONS_DESC_INIT {.type = FLUX_TYPE_EFFECT_BLUR_REGIONS_DESC}

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

/* Reusable, frame-slot-safe realtime blur. Unlike flux_effect_blur's exact
 * Gaussian kernel, this path uses a two-level Dual-Kawase pyramid: two
 * downsample and two upsample passes with a fixed sample count. Requested
 * sigma controls sample offsets instead of loop length, so an animated
 * compositor cannot accidentally create an unbounded full-screen dispatch.
 * A filter owns its pyramid/output images per frame-in-flight slot and needs
 * no transient-pool growth or whole-device wait. Use one filter with one
 * surface/frame stream; begin_frame has already waited for the selected slot
 * before that slot is reused. Output is RGBA8_UNORM with the input dimensions.
 *
 * The image returned by apply is borrowed from the filter and remains valid
 * until the same slot is applied again, the filter is released, or its input
 * extent/format changes. Do not release the returned image. */
typedef struct flux_blur_filter flux_blur_filter;

FLUX_NODISCARD FLUX_API flux_result flux_blur_filter_create(flux_device *device,
                                                            flux_blur_filter **out);
FLUX_NODISCARD FLUX_API flux_blur_filter *flux_blur_filter_retain(flux_blur_filter *filter);
FLUX_API void flux_blur_filter_release(flux_blur_filter *filter);
FLUX_NODISCARD FLUX_API flux_result flux_blur_filter_apply(flux_blur_filter *filter,
                                                           flux_frame *frame,
                                                           const flux_effect_blur_desc *desc,
                                                           flux_image **out);

/* ------------------------------------------------------------------ */
/*  SDF liquid glass                                                  */
/* ------------------------------------------------------------------ */

/* One rounded-rectangle volume in capture-image pixel coordinates. */
typedef struct flux_liquid_glass_shape {
    flux_rect bounds;
    float corner_radius;
} flux_liquid_glass_shape;

/* One glass body. A second shape may be smoothly fused into the first,
 * allowing spring-driven droplets or controls to merge without a seam.
 * shape_count must be 1 or 2.
 *
 * Per-body optical character is caller policy, used verbatim:
 * - shadow_alpha / shadow_blur / shadow_offset_y: the drop shadow cast by
 *   the body's own SDF (alpha 0 disables it);
 * - tint_color: 0xRRGGBB multiplier on the adaptive body tint, for
 *   accent-tinted glass (0xFFFFFF keeps the neutral smoke/pearl pair). */
typedef struct flux_liquid_glass_group {
    flux_liquid_glass_shape shapes[2];
    uint32_t shape_count;
    float blend_radius;
    float opacity;
    float shadow_alpha;
    float shadow_blur;
    float shadow_offset_y;
    uint32_t tint_color;
} flux_liquid_glass_group;

/* Neutral baseline for designated-initializer use: a single visible body
 * with no shadow and the neutral tint, so omitted fields can never turn
 * the glass black. Override the fields a body actually needs. */
#define FLUX_LIQUID_GLASS_GROUP_INIT                                                             \
    {.shape_count = 1, .opacity = 1.0f, .tint_color = 0xFFFFFFu}

/* Reusable frame-slot-safe liquid-glass compositor. `input` is the sharp
 * backdrop capture and `blurred_input` is normally the output of
 * flux_blur_filter_apply for the same capture. Both inputs and the returned
 * image have the same extent. The output is transparent outside the exact
 * analytic SDF plus the drop-shadow falloff, so drawing it over the sharp
 * desktop performs the complete glass composite without a separate
 * rectangular clip.
 *
 * Distances are capture-image pixels. refraction controls the lens offset,
 * chromatic_aberration separates the RGB samples along the surface normal,
 * edge_width controls the curved rim thickness, and glare is the directional
 * highlight strength. saturation and brightness are multipliers; opacity is
 * additionally multiplied by each group's opacity. Drop shadows are
 * configured per group — see flux_liquid_glass_group.
 *
 * Every policy knob is caller-owned; only the curve shapes (the lens
 * profile, the falloff curves) are the material's identity and stay in the
 * shader. Rim band and lensing scale down for small bodies: size_reference
 * is the body size (small side, px) at which effects render at full
 * strength — 0 disables the scaling so every body uses the raw
 * parameters — and size_scale_min floors the factor. tint_strength and
 * frost_strength multiply the adaptive body tint and the scattering layer
 * (1.0 = the reference recipe), letting callers dial a body between
 * clearer and frostier without forking the material. group_count may be zero
 * (and groups NULL) to clear any footprints retained by this frame slot after
 * all bodies disappear; otherwise it is capped at 64. */
typedef struct flux_liquid_glass_desc {
    flux_struct_type type; /* FLUX_TYPE_LIQUID_GLASS_DESC */
    const void *next;
    flux_image *input;
    flux_image *blurred_input;
    const flux_liquid_glass_group *groups;
    uint32_t group_count;
    float refraction;
    float chromatic_aberration;
    float saturation;
    float brightness;
    float edge_width;
    float glare;
    flux_point light_direction;
    float opacity;
    float size_reference;
    float size_scale_min;
    float tint_strength;
    float frost_strength;
} flux_liquid_glass_desc;

#define FLUX_LIQUID_GLASS_DESC_INIT                                                                \
    {.type = FLUX_TYPE_LIQUID_GLASS_DESC,                                                          \
     .refraction = 8.0f,                                                                           \
     .chromatic_aberration = 1.25f,                                                                \
     .saturation = 1.08f,                                                                          \
     .brightness = 1.02f,                                                                          \
     .edge_width = 18.0f,                                                                          \
     .glare = 0.55f,                                                                               \
     .light_direction = {-0.45f, -0.89f},                                                          \
     .opacity = 1.0f,                                                                              \
     .size_reference = 72.0f,                                                                      \
     .size_scale_min = 0.15f,                                                                      \
     .tint_strength = 1.0f,                                                                        \
     .frost_strength = 1.0f}

typedef struct flux_liquid_glass_filter flux_liquid_glass_filter;

FLUX_NODISCARD FLUX_API flux_result flux_liquid_glass_filter_create(flux_device *device,
                                                                    flux_liquid_glass_filter **out);
FLUX_NODISCARD FLUX_API flux_liquid_glass_filter *
flux_liquid_glass_filter_retain(flux_liquid_glass_filter *filter);
FLUX_API void flux_liquid_glass_filter_release(flux_liquid_glass_filter *filter);
FLUX_NODISCARD FLUX_API flux_result
flux_liquid_glass_filter_apply(flux_liquid_glass_filter *filter, flux_frame *frame,
                               const flux_liquid_glass_desc *desc, flux_image **out);

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
