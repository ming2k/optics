/*
 * prism/acrylic.h — acrylic material library component.
 *
 * Acrylic (Fluent Design inspired) is a layered translucent surface:
 *   1. Dual-Kawase backdrop blur with saturation boost;
 *   2. Luminance plate balancing (smoke / pearl plate) for light/dark contrast;
 *   3. Tint color layer;
 *   4. Procedural blue-noise / triangular grain (eliminates 8-bit banding and provides texture);
 *   5. Subtle 1px SDF border highlight / rim outline.
 */

#ifndef PRISM_ACRYLIC_H
#define PRISM_ACRYLIC_H

#include <prism/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/* One rounded-rectangle volume in capture-image pixel coordinates. */
typedef struct prism_acrylic_shape {
    flux_rect bounds;
    float corner_radius;
} prism_acrylic_shape;

/* One acrylic body. */
typedef struct prism_acrylic_group {
    prism_acrylic_shape shape;
    float opacity;
    uint32_t tint_color;      /* 0xRRGGBB tint multiplier */
    float tint_strength;      /* <0 = inherit desc value */
    float luminance_plate;    /* [0, 1] plate polarity: 0 = smoke, 1 = pearl; <0 = inherit */
    float noise_intensity;    /* <0 = inherit desc value */
    float border_width;       /* <0 = inherit desc value */
    float border_alpha;       /* <0 = inherit desc value */
    float shadow_alpha;       /* Drop shadow opacity [0, 1] */
    float shadow_blur;        /* Drop shadow blur radius in px */
    float shadow_offset_y;    /* Drop shadow vertical offset */
} prism_acrylic_group;

#define PRISM_ACRYLIC_GROUP_INIT                                                                   \
    {.opacity = 1.0f,                                                                              \
     .tint_color = 0xFFFFFFu,                                                                      \
     .tint_strength = -1.0f,                                                                       \
     .luminance_plate = -1.0f,                                                                     \
     .noise_intensity = -1.0f,                                                                     \
     .border_width = -1.0f,                                                                        \
     .border_alpha = -1.0f}

/* Dispatch-wide caller policy. */
typedef struct prism_acrylic_desc {
    prism_struct_type type; /* PRISM_TYPE_ACRYLIC_DESC */
    const void *next;
    flux_image *input;
    flux_image *blurred_input;
    const prism_acrylic_group *groups;
    uint32_t group_count;
    float opacity;            /* Overall opacity [0, 1] */
    float tint_strength;      /* Default tint strength (default 0.25) */
    float luminance_plate;    /* Base luminance plate (0 = smoke, 1 = pearl, default 0.5) */
    float noise_intensity;    /* Procedural grain intensity (default 0.02) */
    float border_width;       /* Outline thickness in px (default 1.0) */
    float border_alpha;       /* Outline highlight opacity (default 0.12) */
    prism_material_quality quality;
} prism_acrylic_desc;

#define PRISM_ACRYLIC_DESC_INIT                                                                    \
    {.type = PRISM_TYPE_ACRYLIC_DESC,                                                              \
     .opacity = 1.0f,                                                                              \
     .tint_strength = 0.25f,                                                                       \
     .luminance_plate = 0.5f,                                                                      \
     .noise_intensity = 0.02f,                                                                     \
     .border_width = 1.0f,                                                                         \
     .border_alpha = 0.12f,                                                                        \
     .quality = PRISM_QUALITY_FULL}

typedef struct prism_acrylic_filter prism_acrylic_filter;

PRISM_NODISCARD PRISM_API flux_result
prism_acrylic_filter_create(flux_device *device, prism_acrylic_filter **out);

PRISM_NODISCARD PRISM_API prism_acrylic_filter *
prism_acrylic_filter_retain(prism_acrylic_filter *filter);

PRISM_API void prism_acrylic_filter_release(prism_acrylic_filter *filter);

PRISM_NODISCARD PRISM_API flux_result
prism_acrylic_filter_apply(prism_acrylic_filter *filter, flux_frame *frame,
                           const prism_acrylic_desc *desc, flux_image **out);

#ifdef __cplusplus
}
#endif

#endif /* PRISM_ACRYLIC_H */
