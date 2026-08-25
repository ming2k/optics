#ifndef PRISM_GLASS_DISPATCH_H
#define PRISM_GLASS_DISPATCH_H

/*
 * Shared glass-dispatch recording between the standalone liquid-glass
 * material (liquid_glass.c) and the layered backdrop compositor
 * (backdrop_layer.c). Both materials must evaluate a prism_liquid_glass_group
 * with the identical reference recipe — the layer filter exists precisely so
 * a glass body in a layer stack samples the frosted sheet beneath it rather
 * than the sharp capture, and any divergence between the two dispatch
 * recorders would make a layered body render differently from a standalone
 * one.
 *
 * This header is prism-internal (not installed) and holds no state: every
 * entry is a pure function of its arguments.
 */

#include <prism/liquid_glass.h>

#include <flux/compute.h>
#include <flux/vulkan.h>

#include "regions.h"

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/* Push-constant block — must match liquid_glass.comp exactly. The budget is
 * exactly 160 bytes. */
typedef struct liquid_glass_push {
    uint32_t input_handle;
    uint32_t blurred_handle;
    uint32_t sampler_handle;
    uint32_t output_handle;
    uint32_t width;
    uint32_t height;
    uint32_t origin_x;
    uint32_t origin_y;
    float shape0[4];
    float shape1[4];
    float focus_shape[4];
    float radius0;
    float radius1;
    float focus_radius;
    float blend_radius;
    float opacity;
    float refraction;
    float chromatic_aberration;
    float saturation;
    float brightness;
    float edge_width;
    float rim_light;
    float shadow_alpha;
    uint32_t light_dir;
    uint32_t group_extent;
    uint32_t tint_color_shape_count;
    uint32_t shadow_params;
    uint32_t size_params;
    uint32_t strength_params;
    uint32_t focus_curvature;
    uint32_t tone_params;
} liquid_glass_push;

static_assert(sizeof(liquid_glass_push) == 160,
              "liquid_glass_push no longer matches its shader block");

#define PRISM_GLASS_WG 16u

/* IEEE-754 binary32 → binary16, round-to-nearest-even, for packed
 * 16-bit float push slots. Inputs are validated finite and clamped by caller. */
static inline uint16_t prism_glass_f32_to_f16(float value) {
    uint32_t bits;
    memcpy(&bits, &value, sizeof(bits));
    uint32_t sign = (bits >> 16u) & 0x8000u;
    uint32_t exponent = (bits >> 23u) & 0xFFu;
    uint32_t mantissa = bits & 0x7FFFFFu;

    if (exponent == 0xFFu) /* inf/nan (validation rejects nan) → f16 inf */
        return (uint16_t)(sign | 0x7C00u);

    int half_exp = (int)exponent - 127 + 15;
    if (half_exp >= 0x1F) /* overflow → f16 inf */
        return (uint16_t)(sign | 0x7C00u);
    if (half_exp <= 0) {
        /* f16 subnormal or signed zero. */
        if (half_exp < -10)
            return (uint16_t)sign;
        mantissa |= 0x800000u;                      /* restore the hidden bit */
        uint32_t shift = (uint32_t)(14 - half_exp); /* 13..24 */
        uint32_t half = mantissa >> shift;
        uint32_t remainder = mantissa & ((1u << shift) - 1u);
        uint32_t halfway = 1u << (shift - 1u);
        if (remainder > halfway || (remainder == halfway && (half & 1u)))
            ++half; /* may carry into the exponent field — correct */
        return (uint16_t)(sign | half);
    }
    /* Normal f16: round the 13 dropped mantissa bits, nearest-even. */
    uint32_t half = sign | ((uint32_t)half_exp << 10u) | (mantissa >> 13u);
    uint32_t remainder = mantissa & 0x1FFFu;
    if (remainder > 0x1000u || (remainder == 0x1000u && (half & 1u)))
        ++half; /* mantissa overflow carries into the exponent — correct */
    return (uint16_t)half;
}

static inline void prism_glass_copy_shape(float out[4], prism_liquid_glass_shape shape) {
    out[0] = shape.bounds.x;
    out[1] = shape.bounds.y;
    out[2] = shape.bounds.w;
    out[3] = shape.bounds.h;
}

/* Dispatch-wide glass policy shared by both materials: the desc knobs the
 * reference recipe consumes. The standalone desc and the layered desc both
 * flatten into this before any group is recorded, so a body's evaluation
 * cannot depend on which filter drew it. */
typedef struct prism_glass_policy {
    float refraction;
    float chromatic_aberration;
    float saturation;
    float brightness;
    float edge_width;
    float rim_light;
    float light_x;
    float light_y;
    float opacity;
    float size_reference;
    float size_scale_min;
    float tint_strength;
    float frost_strength;
} prism_glass_policy;

/* Record one glass-group dispatch. `input`/`blurred` are what the lens
 * samples — the sharp capture and its blur for the standalone material, the
 * layered image (frost already applied) and the blur for the layered one.
 * `output` is the persistent storage image the material composites into;
 * `region` its clipped dispatch footprint. The caller owns every barrier:
 * before the first group after a write, and between groups. */
static inline void prism_glass_record_group(VkCommandBuffer command,
                                            flux_compute_pipeline *pipeline, flux_device *device,
                                            const flux_image *input, const flux_image *blurred,
                                            flux_image *output, uint32_t image_width,
                                            uint32_t image_height, liquid_glass_region region,
                                            const prism_liquid_glass_group *group,
                                            const prism_glass_policy *policy) {
    float saturation = liquid_glass_group_or_desc(group->saturation, policy->saturation);
    float tint_strength = liquid_glass_group_or_desc(group->tint_strength, policy->tint_strength);
    float frost_strength =
        liquid_glass_group_or_desc(group->frost_strength, policy->frost_strength);
    float size_reference = fmaxf(policy->size_reference, 0.0f);
    float size_scale_min = fminf(fmaxf(policy->size_scale_min, 0.0f), 1.0f);
    float focus_strength = fminf(fmaxf(group->focus_strength, 0.0f), 1.0f);
    float curvature = 0.0f;

    liquid_glass_push push = {
        .input_handle = flux_image_bindless_handle(input),
        .blurred_handle = flux_image_bindless_handle(blurred),
        .sampler_handle = flux_device_default_sampler_handle(device),
        .output_handle = flux_image_bindless_storage_handle(output),
        .width = image_width,
        .height = image_height,
        .origin_x = region.x,
        .origin_y = region.y,
        .radius0 = fmaxf(group->shapes[0].corner_radius, 0.0f),
        .radius1 = (group->shape_count == 2u ? fmaxf(group->shapes[1].corner_radius, 0.0f) : 0.0f),
        .focus_radius = fmaxf(group->focus.corner_radius, 0.0f),
        .blend_radius = fmaxf(group->blend_radius, 0.0f),
        .opacity = fminf(fmaxf(group->opacity * policy->opacity, 0.0f), 1.0f),
        .refraction = fmaxf(policy->refraction, 0.0f),
        .chromatic_aberration = fmaxf(policy->chromatic_aberration, 0.0f),
        .saturation = fmaxf(saturation, 0.0f),
        .brightness = fmaxf(policy->brightness, 0.0f),
        .edge_width = fmaxf(policy->edge_width, 1.0f),
        .rim_light = fmaxf(policy->rim_light, 0.0f),
        .shadow_alpha = fminf(fmaxf(group->shadow_alpha, 0.0f), 1.0f),
        .light_dir = (uint32_t)prism_glass_f32_to_f16(policy->light_x) |
                     ((uint32_t)prism_glass_f32_to_f16(policy->light_y) << 16u),
        .group_extent = (region.width & 0xFFFFu) | ((region.height & 0xFFFFu) << 16u),
        .tint_color_shape_count = (group->tint_color & 0x00FFFFFFu) | (group->shape_count << 24u),
        .shadow_params = (uint32_t)prism_glass_f32_to_f16(fmaxf(group->shadow_blur, 0.0f)) |
                         ((uint32_t)prism_glass_f32_to_f16(group->shadow_offset_y) << 16u),
        .size_params = (uint32_t)prism_glass_f32_to_f16(size_reference) |
                       ((uint32_t)prism_glass_f32_to_f16(size_scale_min) << 16u),
        .strength_params = (uint32_t)prism_glass_f32_to_f16(fmaxf(tint_strength, 0.0f)) |
                           ((uint32_t)prism_glass_f32_to_f16(fmaxf(frost_strength, 0.0f)) << 16u),
        .focus_curvature = (uint32_t)prism_glass_f32_to_f16(focus_strength) |
                           ((uint32_t)prism_glass_f32_to_f16(curvature) << 16u),
        .tone_params = (uint32_t)prism_glass_f32_to_f16(group->plate_polarity) |
                       ((uint32_t)prism_glass_f32_to_f16(group->backdrop_energy) << 16u),
    };
    prism_glass_copy_shape(push.shape0, group->shapes[0]);
    if (group->shape_count == 2u)
        prism_glass_copy_shape(push.shape1, group->shapes[1]);
    else
        memset(push.shape1, 0, sizeof(push.shape1));
    if (focus_strength > 0.0f)
        prism_glass_copy_shape(push.focus_shape, group->focus);
    else
        memset(push.focus_shape, 0, sizeof(push.focus_shape));

    uint32_t gx = (region.width + PRISM_GLASS_WG - 1u) / PRISM_GLASS_WG;
    uint32_t gy = (region.height + PRISM_GLASS_WG - 1u) / PRISM_GLASS_WG;
    flux_compute_dispatch(command, pipeline, &push, sizeof(push), gx, gy, 1u);
}

#endif /* PRISM_GLASS_DISPATCH_H */
