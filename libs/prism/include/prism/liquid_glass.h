/*
 * prism/liquid_glass.h — analytic liquid-glass material.
 *
 * prism is the material library of the optics stack. A named material —
 * its shader, its curve shapes, and its parameter contract — lives here,
 * built on flux's public effect runtime (compute-writable images, frame
 * slots, the bindless heap). flux owns rendering mechanism, prism owns
 * material identity, and the caller owns policy (ADR-0063; the material
 * model is ADR-0046, the focus field ADR-0050).
 *
 * The filter is a reusable frame-slot-safe compositor. `input` is the
 * sharp backdrop capture and `blurred_input` is normally the output of
 * flux_blur_filter_apply for the same capture. Both inputs and the
 * returned image have the same extent. The output is transparent outside
 * the exact analytic SDF plus the drop-shadow falloff, so drawing it over
 * the sharp desktop performs the complete glass composite without a
 * separate rectangular clip.
 *
 * Threading: filters are not thread-safe per device. Serialize calls per
 * device, as you would for any other flux recording API.
 */

#ifndef PRISM_LIQUID_GLASS_H
#define PRISM_LIQUID_GLASS_H

#include <flux/core.h>
#include <flux/math.h> /* flux_rect, flux_point */

#if defined(_WIN32) && !defined(PRISM_STATIC)
#ifdef PRISM_BUILDING
#define PRISM_API __declspec(dllexport)
#else
#define PRISM_API __declspec(dllimport)
#endif
#elif defined(__GNUC__) || defined(__clang__)
#define PRISM_API __attribute__((visibility("default")))
#else
#define PRISM_API
#endif

#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 202311L
#define PRISM_NODISCARD [[nodiscard]]
#elif defined(__GNUC__) || defined(__clang__)
#define PRISM_NODISCARD __attribute__((warn_unused_result))
#elif defined(_MSC_VER)
#define PRISM_NODISCARD _Check_return_
#else
#define PRISM_NODISCARD
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* prism descriptors open with `prism_struct_type type; const void *next;`
 * following the same extension-chain pattern as flux. The registry is
 * prism-local: values are not shared with flux_struct_type. */
typedef enum prism_struct_type {
    PRISM_TYPE_UNKNOWN = 0,
    PRISM_TYPE_LIQUID_GLASS_DESC = 1,
    /* Append only. Never repurpose. */
} prism_struct_type;

/* One rounded-rectangle volume in capture-image pixel coordinates. */
typedef struct prism_liquid_glass_shape {
    flux_rect bounds;
    float corner_radius;
} prism_liquid_glass_shape;

/* One glass body. A second shape may be smoothly fused into the first,
 * allowing spring-driven droplets or controls to merge without a seam.
 * shape_count must be 1 or 2.
 *
 * Per-body optical character is caller policy, used verbatim:
 * - shadow_alpha / shadow_blur / shadow_offset_y: the drop shadow cast by
 *   the body's own SDF (alpha 0 disables it);
 * - tint_color: 0xRRGGBB multiplier on the adaptive body tint, for
 *   accent-tinted glass (0xFFFFFF keeps the neutral smoke/pearl pair);
 * - focus / focus_strength: one soft optical emphasis field inside a
 *   single-shape body. It changes clarity and directional light without
 *   creating or outlining another glass body. Focus bounds must remain
 *   inside shapes[0]. Focus and smooth union are mutually exclusive because
 *   both reuse the secondary-shape shader slot. */
typedef struct prism_liquid_glass_group {
    prism_liquid_glass_shape shapes[2];
    uint32_t shape_count;
    float blend_radius;
    float opacity;
    float shadow_alpha;
    float shadow_blur;
    float shadow_offset_y;
    uint32_t tint_color;
    prism_liquid_glass_shape focus;
    float focus_strength;
} prism_liquid_glass_group;

/* Neutral baseline for designated-initializer use: a single visible body
 * with no shadow and the neutral tint, so omitted fields can never turn
 * the glass black. Override the fields a body actually needs. */
#define PRISM_LIQUID_GLASS_GROUP_INIT {.shape_count = 1, .opacity = 1.0f, .tint_color = 0xFFFFFFu}

/* Dispatch-wide caller policy. Distances are capture-image pixels.
 * refraction controls the lens offset, chromatic_aberration separates the
 * RGB samples along the surface normal, edge_width controls the curved rim
 * thickness, and rim_light scales the whole rim lighting set (key line,
 * sheen, fresnel, shadow side, trough) — one knob for overall rim energy.
 * saturation and brightness are multipliers; opacity is additionally
 * multiplied by each group's opacity. Drop shadows are configured per
 * group — see prism_liquid_glass_group.
 *
 * Rim band and lensing scale down for small bodies: size_reference is the
 * body size (small side, px) at which effects render at full strength —
 * 0 disables the scaling so every body uses the raw parameters — and
 * size_scale_min floors the factor. tint_strength and frost_strength
 * multiply the adaptive body tint and the scattering layer (1.0 = the
 * reference recipe), letting callers dial a body between clearer and
 * frostier without forking the material. group_count may be zero (and
 * groups NULL) to clear any footprints retained by this frame slot after
 * all bodies disappear; otherwise it is capped at 64. */
typedef struct prism_liquid_glass_desc {
    prism_struct_type type; /* PRISM_TYPE_LIQUID_GLASS_DESC */
    const void *next;
    flux_image *input;
    flux_image *blurred_input;
    const prism_liquid_glass_group *groups;
    uint32_t group_count;
    float refraction;
    float chromatic_aberration;
    float saturation;
    float brightness;
    float edge_width;
    float rim_light;
    flux_point light_direction;
    float opacity;
    float size_reference;
    float size_scale_min;
    float tint_strength;
    float frost_strength;
} prism_liquid_glass_desc;

#define PRISM_LIQUID_GLASS_DESC_INIT                                                               \
    {.type = PRISM_TYPE_LIQUID_GLASS_DESC,                                                         \
     .refraction = 8.0f,                                                                           \
     .chromatic_aberration = 1.25f,                                                                \
     .saturation = 1.08f,                                                                          \
     .brightness = 1.02f,                                                                          \
     .edge_width = 18.0f,                                                                          \
     .rim_light = 0.55f,                                                                           \
     .light_direction = {-0.45f, -0.89f},                                                          \
     .opacity = 1.0f,                                                                              \
     .size_reference = 72.0f,                                                                      \
     .size_scale_min = 0.15f,                                                                      \
     .tint_strength = 1.0f,                                                                        \
     .frost_strength = 1.0f}

typedef struct prism_liquid_glass_filter prism_liquid_glass_filter;

PRISM_NODISCARD PRISM_API flux_result
prism_liquid_glass_filter_create(flux_device *device, prism_liquid_glass_filter **out);
PRISM_NODISCARD PRISM_API prism_liquid_glass_filter *
prism_liquid_glass_filter_retain(prism_liquid_glass_filter *filter);
/* Release only after every submission that references the filter's outputs
 * has completed (its frame-slot fence has signalled, or after
 * flux_device_wait_idle): release destroys the filter's compute pipelines
 * inline. */
PRISM_API void prism_liquid_glass_filter_release(prism_liquid_glass_filter *filter);
/* Requires a recording frame with no active pass
 * (flux_frame_get_state(frame) == FLUX_FRAME_STATE_RECORDING and
 * !flux_frame_has_active_pass(frame)). The returned image is borrowed from
 * the filter and remains valid until the same slot is applied again, the
 * filter is released, or its input extent changes. Do not release it. */
PRISM_NODISCARD PRISM_API flux_result
prism_liquid_glass_filter_apply(prism_liquid_glass_filter *filter, flux_frame *frame,
                                const prism_liquid_glass_desc *desc, flux_image **out);

#ifdef __cplusplus
}
#endif

#endif /* PRISM_LIQUID_GLASS_H */
