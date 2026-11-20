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

#ifndef OPTICS_MATERIAL_ACRYLIC_H
#define OPTICS_MATERIAL_ACRYLIC_H

#include <optics/material/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/* One rounded-rectangle volume in capture-image pixel coordinates. */
typedef struct optics_material_acrylic_shape {
    optics_gfx_rect bounds;
    float corner_radius;
} optics_material_acrylic_shape;

/* One acrylic body. */
typedef struct optics_material_acrylic_group {
    optics_material_acrylic_shape shape;
    float opacity;
    uint32_t tint_color;   /* 0xRRGGBB tint multiplier */
    float tint_strength;   /* <0 = inherit desc value */
    float luminance_plate; /* [0, 1] plate polarity: 0 = smoke, 1 = pearl; <0 = inherit */
    float noise_intensity; /* <0 = inherit desc value */
    float border_width;    /* <0 = inherit desc value */
    float border_alpha;    /* <0 = inherit desc value */
    float shadow_alpha;    /* Drop shadow opacity [0, 1] */
    float shadow_blur;     /* Drop shadow blur radius in px */
    float shadow_offset_y; /* Drop shadow vertical offset */
} optics_material_acrylic_group;

#define OPTICS_MATERIAL_ACRYLIC_GROUP_INIT                                                         \
    {.opacity = 1.0f,                                                                              \
     .tint_color = 0xFFFFFFu,                                                                      \
     .tint_strength = -1.0f,                                                                       \
     .luminance_plate = -1.0f,                                                                     \
     .noise_intensity = -1.0f,                                                                     \
     .border_width = -1.0f,                                                                        \
     .border_alpha = -1.0f}

/* Dispatch-wide caller policy. */
typedef struct optics_material_acrylic_desc {
    optics_material_struct_type type; /* PRISM_TYPE_ACRYLIC_DESC */
    const void *next;
    optics_gfx_image *input;
    optics_gfx_image *blurred_input;
    const optics_material_acrylic_group *groups;
    uint32_t group_count;
    float opacity;         /* Overall opacity [0, 1] */
    float tint_strength;   /* Default tint strength (default 0.25) */
    float luminance_plate; /* Base luminance plate (0 = smoke, 1 = pearl, default 0.5) */
    float noise_intensity; /* Procedural grain intensity (default 0.02) */
    float border_width;    /* Outline thickness in px (default 1.0) */
    float border_alpha;    /* Outline highlight opacity (default 0.12) */
    optics_material_quality quality;
} optics_material_acrylic_desc;

#define OPTICS_MATERIAL_ACRYLIC_DESC_INIT                                                          \
    {.type = OPTICS_MATERIAL_TYPE_ACRYLIC_DESC,                                                    \
     .opacity = 1.0f,                                                                              \
     .tint_strength = 0.25f,                                                                       \
     .luminance_plate = 0.5f,                                                                      \
     .noise_intensity = 0.02f,                                                                     \
     .border_width = 1.0f,                                                                         \
     .border_alpha = 0.12f,                                                                        \
     .quality = OPTICS_MATERIAL_QUALITY_FULL}

typedef struct optics_material_acrylic_filter optics_material_acrylic_filter;

/* Options descriptor for prism_acrylic_filter_warmup (ADR-0141). */
typedef struct optics_material_acrylic_warmup_opts {
    optics_gfx_format format; /* Target image format (default 0 -> FLUX_FORMAT_RGBA8_UNORM) */
} optics_material_acrylic_warmup_opts;

OPTICS_MATERIAL_NODISCARD OPTICS_MATERIAL_API optics_gfx_result
optics_material_acrylic_filter_create(optics_gfx_device *device,
                                      optics_material_acrylic_filter **out);

/* Pre-warm compute pipelines ahead of time to eliminate cold-start JIT hitches (ADR-0141). */
OPTICS_MATERIAL_NODISCARD OPTICS_MATERIAL_API optics_gfx_result
optics_material_acrylic_filter_warmup(optics_material_acrylic_filter *filter,
                                      const optics_material_acrylic_warmup_opts *opts);

OPTICS_MATERIAL_NODISCARD OPTICS_MATERIAL_API optics_material_acrylic_filter *
optics_material_acrylic_filter_retain(optics_material_acrylic_filter *filter);

OPTICS_MATERIAL_API void
optics_material_acrylic_filter_release(optics_material_acrylic_filter *filter);

typedef struct optics_material_acrylic_filter_apply_opts {
    optics_gfx_frame *frame;
    const optics_material_acrylic_desc *desc;
    optics_material_output *out;
} optics_material_acrylic_filter_apply_opts;

OPTICS_MATERIAL_NODISCARD OPTICS_MATERIAL_API optics_gfx_result
optics_material_acrylic_filter_apply(optics_material_acrylic_filter *filter,
                                     const optics_material_acrylic_filter_apply_opts *opts);

#ifdef __cplusplus
}
#endif

#endif /* PRISM_ACRYLIC_H */
