/*
 * prism/backdrop_layer.h — layered backdrop material compositor.
 *
 * A chrome surface is usually not one material: a frosted sheet (the
 * blurred backdrop, drawn as one or more rectangles) carries analytic
 * liquid-glass bodies on top of it. Both materials share the same two
 * source textures — the sharp backdrop capture and its blur — but until
 * now each had to be evaluated against that shared source and stacked by
 * the caller as two separate offscreen images. A glass body refracted the
 * sharp desktop, so it pierced straight through the frost beneath it: the
 * two layers disagreed about what "behind" means and the stack came apart
 * at every glass silhouette.
 *
 * This filter composes the stack in one ordered dispatch sequence against
 * one persistent transparent output (the same contract as the other prism
 * materials):
 *
 *   1. frost: every `prism_backdrop_frost` rectangle writes the blurred
 *      backdrop, source-over, inside its rect (rounded-rect SDF coverage,
 *      per-rect opacity);
 *   2. glass: every `prism_liquid_glass_group` is evaluated exactly as
 *      prism_liquid_glass_filter_apply evaluates it — but the lens samples
 *      the *frosted* layer, so a glass body bends and frosts the frost
 *      beneath it instead of looking past it.
 *
 * The result is one image: transparent outside every frost rect and glass
 * silhouette, the materials correctly nested inside it. Drawing it over
 * the sharp desktop completes the composite.
 *
 * `input` is the sharp backdrop capture and `blurred_input` is normally
 * the output of flux_blur_filter_apply for the same capture. All three
 * images share one extent. Glass statistics are submitted for the glass
 * groups exactly as the liquid-glass filter does, and are read back with
 * prism_liquid_glass_filter_stats on the same slot cadence.
 *
 * Threading: filters are not thread-safe per device. Serialize calls per
 * device, as you would for any other flux recording API.
 */

#ifndef PRISM_BACKDROP_LAYER_H
#define PRISM_BACKDROP_LAYER_H

#include <prism/liquid_glass.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Base Material Layer (底衬材质层):
 * Represents a non-distorting foundation plate (e.g. frosted blur sheet,
 * tinted acrylic, or protective scrim) in capture-image pixel coordinates.
 *
 * The plate writes an OPAQUE pixel: the (optionally tinted) blurred backdrop
 * resolved over the sharp capture underneath, gated by analytic rounded-rect
 * coverage and the plate's opacity. Opacity therefore blends base-vs-sharp,
 * never base-vs-transparent — a partial-coverage base plate still reads as a
 * true background colour to the optical glass lens above it.
 *
 * `tint_color`/`tint_strength` blend a wash INTO the base material so veils
 * and scheme-adaptive scrims live beneath the glass instead of being
 * painted over it by chrome. A radius of 0 is a plain rectangle. Base
 * material plates paint before every glass group regardless of array order —
 * the layer order is part of this composite material's identity, not caller policy.
 */
typedef struct prism_base_material {
    flux_rect bounds;
    float corner_radius;
    float opacity;       /* [0, 1] */
    uint32_t tint_color; /* 0xRRGGBB wash blended into the base material */
    float tint_strength; /* [0, 1]; 0 keeps the blurred backdrop */
} prism_base_material;

#define PRISM_BASE_MATERIAL_INIT                                                                   \
    {.corner_radius = 0.0f, .opacity = 1.0f, .tint_color = 0xFFFFFFu, .tint_strength = 0.0f}

/* Backward-compatible alias for existing call sites. */
typedef prism_base_material prism_backdrop_frost;
#define PRISM_BACKDROP_FROST_INIT PRISM_BASE_MATERIAL_INIT

#define PRISM_BACKDROP_MAX_BASE_MATERIALS 16u
#define PRISM_BACKDROP_MAX_FROST_RECTS PRISM_BACKDROP_MAX_BASE_MATERIALS
#define PRISM_BACKDROP_MAX_GLASS_GROUPS 64u

/*
 * Optical Glass Layer (高光与折射透镜层 policy):
 * Defines dispatch-wide convex lens simulation, edge dispersion, and
 * sculpted directional lighting (primary key line, diagonal secondary catchlight,
 * fresnel sheen, and transmitted light trough).
 */
typedef struct prism_optical_glass_params {
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
    float curvature; /* [0, 1] continuous curvature (squircle) blend factor: 0 = rounded rect, 1 =
                        G2 superellipse */
} prism_optical_glass_params;

#define PRISM_OPTICAL_GLASS_PARAMS_INIT                                                            \
    {.refraction = 8.0f,                                                                           \
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
     .frost_strength = 1.0f,                                                                       \
     .curvature = 0.0f}

/*
 * Multi-layering Compositor (多层级层叠复合描述符):
 * Combines Base Material Layer (plates) beneath Optical Glass Layer (groups).
 */
typedef struct prism_backdrop_layer_desc {
    prism_struct_type type; /* PRISM_TYPE_BACKDROP_LAYER_DESC */
    const void *next;
    flux_image *input;         /* sharp backdrop capture (required) */
    flux_image *blurred_input; /* blurred backdrop (required, same extent) */

    /* 1. Base Material Layer (底衬材质层) */
    const prism_base_material *frost; /* may be NULL/0; base material plates */
    uint32_t frost_count;

    /* 2. Optical Glass Layer (高光与折射透镜层) */
    const prism_liquid_glass_group *groups; /* may be NULL/0 */
    uint32_t group_count;

    /* Optical Glass policy parameters (mirrors prism_optical_glass_params) */
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
    float curvature; /* [0, 1] continuous curvature (squircle) blend factor: 0 = rounded rect, 1 =
                        G2 superellipse */
} prism_backdrop_layer_desc;

#define PRISM_BACKDROP_LAYER_DESC_INIT                                                             \
    {.type = PRISM_TYPE_BACKDROP_LAYER_DESC,                                                       \
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
     .frost_strength = 1.0f,                                                                       \
     .curvature = 0.0f}

typedef struct prism_backdrop_layer_filter prism_backdrop_layer_filter;

PRISM_NODISCARD PRISM_API flux_result
prism_backdrop_layer_filter_create(flux_device *device, prism_backdrop_layer_filter **out);
PRISM_NODISCARD PRISM_API prism_backdrop_layer_filter *
prism_backdrop_layer_filter_retain(prism_backdrop_layer_filter *filter);
/* Release only after every submission that references the filter's outputs
 * has completed (its frame-slot fence has signalled, or after
 * flux_device_wait_idle): release destroys the filter's compute pipelines
 * inline. */
PRISM_API void prism_backdrop_layer_filter_release(prism_backdrop_layer_filter *filter);
/* Requires a recording frame with no active pass. The returned image is
 * borrowed from the filter and remains valid until the same slot is
 * applied again, the filter is released, or its input extent changes. Do
 * not release it. The output is transparent outside the union of every
 * frost rect and glass silhouette (including shadow falloff). */
PRISM_NODISCARD PRISM_API flux_result
prism_backdrop_layer_filter_apply(prism_backdrop_layer_filter *filter, flux_frame *frame,
                                  const prism_backdrop_layer_desc *desc, flux_image **out);

/* Reads the glass statistics this frame slot last submitted (see
 * prism_liquid_glass_filter_stats; group i of the stats aligns with group
 * i of that submission's glass array). */
PRISM_NODISCARD PRISM_API flux_result prism_backdrop_layer_filter_stats(
    prism_backdrop_layer_filter *filter, flux_frame *frame, prism_backdrop_stat *out,
    uint32_t max_groups, uint32_t *out_count);

#ifdef __cplusplus
}
#endif

#endif /* PRISM_BACKDROP_LAYER_H */
