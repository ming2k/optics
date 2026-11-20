/*
 * prism/mica.h — mica foundation material library component.
 *
 * Mica (Fluent Design inspired) is an opaque/translucent foundation material
 * that incorporates the desktop wallpaper and theme color to paint long-lived
 * application windows (window chrome, commanding layers, tab strips):
 *   1. Screen-anchored desktop wallpaper sampling (anchored to desktop coordinates
 *      so the material dynamically reveals the wallpaper behind the window);
 *   2. Soft Gaussian / Dual-Kawase blur to smooth high frequencies;
 *   3. Luminosity plate balancing (Pearl for light theme, Smoke for dark theme);
 *   4. Color tinting layer with support for Mica Base and Mica Alt (deeper contrast);
 *   5. Procedural blue-noise grain / dithering (eliminates 8-bit banding);
 *   6. Subtle 1px SDF border highlight rim & analytic drop shadow;
 *   7. Inactive / low-power solid fallback state blending.
 */

#ifndef OPTICS_MATERIAL_MICA_H
#define OPTICS_MATERIAL_MICA_H

#include <optics/material/types.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* One rounded-rectangle volume in capture-image pixel coordinates. */
typedef struct optics_material_mica_shape {
    optics_gfx_rect bounds;
    float corner_radius;
} optics_material_mica_shape;

/* Mica variant: Base (standard foundation) or Alt (commanding layer / tab strip). */
typedef enum optics_material_mica_kind : uint32_t {
    OPTICS_MATERIAL_MICA_BASE = 0,
    OPTICS_MATERIAL_MICA_ALT = 1,
} optics_material_mica_kind;

/* One mica body group. */
typedef struct optics_material_mica_group {
    optics_material_mica_shape shape;
    float opacity;
    uint32_t tint_color;      /* 0xRRGGBB tint multiplier */
    float tint_opacity;       /* <0 = inherit desc value */
    float luminosity_plate;   /* [0, 1] plate polarity: 0 = smoke, 1 = pearl; <0 = inherit */
    float luminosity_opacity; /* <0 = inherit desc value */
    float noise_intensity;    /* <0 = inherit desc value */
    float border_width;       /* <0 = inherit desc value */
    float border_alpha;       /* <0 = inherit desc value */
    float shadow_alpha;       /* Drop shadow opacity [0, 1] */
    float shadow_blur;        /* Drop shadow blur radius in px */
    float shadow_offset_y;    /* Drop shadow vertical offset */
} optics_material_mica_group;

#define OPTICS_MATERIAL_MICA_GROUP_INIT                                                            \
    {.opacity = 1.0f,                                                                              \
     .tint_color = 0xFFFFFFu,                                                                      \
     .tint_opacity = -1.0f,                                                                        \
     .luminosity_plate = -1.0f,                                                                    \
     .luminosity_opacity = -1.0f,                                                                  \
     .noise_intensity = -1.0f,                                                                     \
     .border_width = -1.0f,                                                                        \
     .border_alpha = -1.0f}

/* Dispatch-wide caller policy. */
typedef struct optics_material_mica_desc {
    optics_material_struct_type type; /* PRISM_TYPE_MICA_DESC */
    const void *next;
    optics_gfx_image *wallpaper; /* Clean wallpaper image (optional if blurred_wallpaper set) */
    optics_gfx_image
        *blurred_wallpaper; /* Blurred wallpaper image (falls back to wallpaper if null) */
    const optics_material_mica_group *groups;
    uint32_t group_count;
    optics_material_mica_kind kind; /* Base or Alt variant */
    float screen_origin_x;          /* Window top-left position on desktop screen in px */
    float screen_origin_y;
    float screen_width;       /* Total desktop screen width in px (0 for local UV) */
    float screen_height;      /* Total desktop screen height in px */
    float opacity;            /* Overall opacity [0, 1] */
    uint32_t tint_color;      /* Theme tint color 0xRRGGBB (default 0xFFFFFF) */
    float tint_opacity;       /* Tint strength [0, 1] (default: 0.20 Base, 0.40 Alt) */
    float luminosity_plate;   /* Base luminance plate (0 = smoke, 1 = pearl, default 0.5) */
    float luminosity_opacity; /* Plate blend weight (default: 0.65 Base, 0.78 Alt) */
    float noise_intensity;    /* Procedural grain intensity (default 0.02) */
    float border_width;       /* Outline thickness in px (default 1.0) */
    float border_alpha;       /* Outline highlight opacity (default 0.10) */
    uint32_t fallback_color;  /* Solid fallback color 0xRRGGBB (inactive state) */
    float fallback_weight;    /* [0, 1] weight toward fallback_color (1.0 = solid inactive) */
    optics_material_quality quality;
} optics_material_mica_desc;

#define OPTICS_MATERIAL_MICA_DESC_INIT                                                             \
    {.type = OPTICS_MATERIAL_TYPE_MICA_DESC,                                                       \
     .opacity = 1.0f,                                                                              \
     .tint_color = 0xFFFFFFu,                                                                      \
     .tint_opacity = 0.20f,                                                                        \
     .luminosity_plate = 0.5f,                                                                     \
     .luminosity_opacity = 0.65f,                                                                  \
     .noise_intensity = 0.02f,                                                                     \
     .border_width = 1.0f,                                                                         \
     .border_alpha = 0.10f,                                                                        \
     .fallback_color = 0x202020u,                                                                  \
     .fallback_weight = 0.0f,                                                                      \
     .quality = OPTICS_MATERIAL_QUALITY_FULL}

typedef struct optics_material_mica_filter optics_material_mica_filter;

/* Options descriptor for prism_mica_filter_warmup (ADR-0141). */
typedef struct optics_material_mica_warmup_opts {
    optics_gfx_format format; /* Target image format (default 0 -> FLUX_FORMAT_RGBA8_UNORM) */
} optics_material_mica_warmup_opts;

OPTICS_MATERIAL_NODISCARD OPTICS_MATERIAL_API optics_gfx_result
optics_material_mica_filter_create(optics_gfx_device *device, optics_material_mica_filter **out);

/* Pre-warm compute pipelines ahead of time to eliminate cold-start JIT hitches (ADR-0141). */
OPTICS_MATERIAL_NODISCARD OPTICS_MATERIAL_API optics_gfx_result optics_material_mica_filter_warmup(
    optics_material_mica_filter *filter, const optics_material_mica_warmup_opts *opts);

OPTICS_MATERIAL_NODISCARD OPTICS_MATERIAL_API optics_material_mica_filter *
optics_material_mica_filter_retain(optics_material_mica_filter *filter);

OPTICS_MATERIAL_API void optics_material_mica_filter_release(optics_material_mica_filter *filter);

typedef struct optics_material_mica_filter_apply_opts {
    optics_gfx_frame *frame;
    const optics_material_mica_desc *desc;
    optics_material_output *out;
} optics_material_mica_filter_apply_opts;

OPTICS_MATERIAL_NODISCARD OPTICS_MATERIAL_API optics_gfx_result optics_material_mica_filter_apply(
    optics_material_mica_filter *filter, const optics_material_mica_filter_apply_opts *opts);

#ifdef __cplusplus
}
#endif

#endif /* PRISM_MICA_H */
