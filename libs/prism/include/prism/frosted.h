/*
 * prism/frosted.h — classic non-distorting frosted glass material.
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

#ifndef PRISM_FROSTED_H
#define PRISM_FROSTED_H

#include <prism/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/* One rounded-rectangle volume in capture-image pixel coordinates. */
typedef struct prism_frosted_shape {
    flux_rect bounds;
    float corner_radius;
} prism_frosted_shape;

/* One frosted glass body. */
typedef struct prism_frosted_group {
    prism_frosted_shape shape;
    float opacity;
    uint32_t tint_color;   /* 0xRRGGBB tint multiplier */
    float tint_strength;   /* <0 = inherit desc value */
    float saturation;      /* <0 = inherit desc value */
    float shadow_alpha;    /* Drop shadow opacity [0, 1] */
    float shadow_blur;     /* Drop shadow blur radius in px */
    float shadow_offset_y; /* Drop shadow vertical offset */
    float noise_intensity; /* <0 = inherit desc value */
} prism_frosted_group;

#define PRISM_FROSTED_GROUP_INIT                                                                   \
    {.opacity = 1.0f,                                                                              \
     .tint_color = 0xFFFFFFu,                                                                      \
     .tint_strength = -1.0f,                                                                       \
     .saturation = -1.0f,                                                                          \
     .noise_intensity = -1.0f}

/* Dispatch-wide caller policy. */
typedef struct prism_frosted_desc {
    prism_struct_type type; /* PRISM_TYPE_FROSTED_DESC */
    const void *next;
    flux_image *input;
    flux_image *blurred_input;
    const prism_frosted_group *groups;
    uint32_t group_count;
    float opacity;         /* Overall opacity [0, 1] */
    float saturation;      /* Saturation multiplier (default 1.25) */
    float tint_strength;   /* Default tint strength (default 0.15) */
    float noise_intensity; /* Procedural grain intensity (default 0.015) */
    prism_material_quality quality;
} prism_frosted_desc;

#define PRISM_FROSTED_DESC_INIT                                                                    \
    {.type = PRISM_TYPE_FROSTED_DESC,                                                              \
     .opacity = 1.0f,                                                                              \
     .saturation = 1.25f,                                                                          \
     .tint_strength = 0.15f,                                                                       \
     .noise_intensity = 0.015f,                                                                    \
     .quality = PRISM_QUALITY_FULL}

typedef struct prism_frosted_filter prism_frosted_filter;

PRISM_NODISCARD PRISM_API flux_result prism_frosted_filter_create(flux_device *device,
                                                                  prism_frosted_filter **out);

PRISM_NODISCARD PRISM_API prism_frosted_filter *
prism_frosted_filter_retain(prism_frosted_filter *filter);

PRISM_API void prism_frosted_filter_release(prism_frosted_filter *filter);

PRISM_NODISCARD PRISM_API flux_result prism_frosted_filter_apply(prism_frosted_filter *filter,
                                                                 flux_frame *frame,
                                                                 const prism_frosted_desc *desc,
                                                                 flux_image **out);

#ifdef __cplusplus
}
#endif

#endif /* PRISM_FROSTED_H */
