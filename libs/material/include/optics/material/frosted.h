/*
 * material/frosted.h — classic non-distorting frosted glass material.
 *
 * Frosted glass is the clean, non-refractive scattering material:
 * Dual-Kawase backdrop blur, saturation boost for color vibrancy,
 * subtle adaptive tint, procedural blue-noise dithering (to eliminate
 * 8-bit banding), and analytic rounded-rect SDF coverage with drop shadow.
 *
 * Unlike liquid glass, frosted glass features NO convex lens distortion
 * or chromatic dispersion, making it ideal for text-heavy windows, sidebars,
 * menus, and standard dialogs.
 */

#ifndef OPTICS_MATERIAL_FROSTED_H
#define OPTICS_MATERIAL_FROSTED_H

#include <optics/material/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/* One rounded-rectangle volume in capture-image pixel coordinates. */
typedef struct optics_material_frosted_shape {
    optics_gfx_rect bounds;
    float corner_radius;
} optics_material_frosted_shape;

/* One frosted glass body. */
typedef struct optics_material_frosted_group {
    optics_material_frosted_shape shape;
    float opacity;
    uint32_t tint_color;   /* 0xRRGGBB tint multiplier */
    float tint_strength;   /* <0 = inherit desc value */
    float saturation;      /* <0 = inherit desc value */
    float shadow_alpha;    /* Drop shadow opacity [0, 1] */
    float shadow_blur;     /* Drop shadow blur radius in px */
    float shadow_offset_y; /* Drop shadow vertical offset */
    float noise_intensity; /* <0 = inherit desc value */
} optics_material_frosted_group;

#define OPTICS_MATERIAL_FROSTED_GROUP_INIT                                                         \
    {.opacity = 1.0f,                                                                              \
     .tint_color = 0xFFFFFFu,                                                                      \
     .tint_strength = -1.0f,                                                                       \
     .saturation = -1.0f,                                                                          \
     .noise_intensity = -1.0f}

/* Dispatch-wide caller policy. */
typedef struct optics_material_frosted_desc {
    optics_material_struct_type type; /* OPTICS_MATERIAL_TYPE_FROSTED_DESC */
    const void *next;
    optics_gfx_image *input;
    optics_gfx_image *blurred_input;
    const optics_material_frosted_group *groups;
    uint32_t group_count;
    float opacity;         /* Overall opacity [0, 1] */
    float saturation;      /* Saturation multiplier (default 1.25) */
    float tint_strength;   /* Default tint strength (default 0.15) */
    float noise_intensity; /* Procedural grain intensity (default 0.015) */
    optics_material_quality quality;
} optics_material_frosted_desc;

#define OPTICS_MATERIAL_FROSTED_DESC_INIT                                                          \
    {.type = OPTICS_MATERIAL_TYPE_FROSTED_DESC,                                                    \
     .opacity = 1.0f,                                                                              \
     .saturation = 1.25f,                                                                          \
     .tint_strength = 0.15f,                                                                       \
     .noise_intensity = 0.015f,                                                                    \
     .quality = OPTICS_MATERIAL_QUALITY_FULL}

typedef struct optics_material_frosted_filter optics_material_frosted_filter;

/* Options descriptor for optics_material_frosted_filter_warmup (ADR-0141). */
typedef struct optics_material_frosted_warmup_opts {
    optics_gfx_format format; /* Target image format (default 0 -> OPTICS_GFX_FORMAT_RGBA8_UNORM) */
} optics_material_frosted_warmup_opts;

OPTICS_MATERIAL_NODISCARD OPTICS_MATERIAL_API optics_gfx_result
optics_material_frosted_filter_create(optics_gfx_device *device,
                                      optics_material_frosted_filter **out);

/* Pre-warm compute pipelines ahead of time to eliminate cold-start JIT hitches (ADR-0141). */
OPTICS_MATERIAL_NODISCARD OPTICS_MATERIAL_API optics_gfx_result
optics_material_frosted_filter_warmup(optics_material_frosted_filter *filter,
                                      const optics_material_frosted_warmup_opts *opts);

OPTICS_MATERIAL_NODISCARD OPTICS_MATERIAL_API optics_material_frosted_filter *
optics_material_frosted_filter_retain(optics_material_frosted_filter *filter);

OPTICS_MATERIAL_API void
optics_material_frosted_filter_release(optics_material_frosted_filter *filter);

typedef struct optics_material_frosted_filter_apply_opts {
    optics_gfx_frame *frame;
    const optics_material_frosted_desc *desc;
    optics_material_output *out;
} optics_material_frosted_filter_apply_opts;

OPTICS_MATERIAL_NODISCARD OPTICS_MATERIAL_API optics_gfx_result
optics_material_frosted_filter_apply(optics_material_frosted_filter *filter,
                                     const optics_material_frosted_filter_apply_opts *opts);

#ifdef __cplusplus
}
#endif

#endif /* OPTICS_MATERIAL_FROSTED_H */
