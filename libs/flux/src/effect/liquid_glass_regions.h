#ifndef FLUX_EFFECT_LIQUID_GLASS_REGIONS_H
#define FLUX_EFFECT_LIQUID_GLASS_REGIONS_H

#include <flux/effect.h>

#include <math.h>
#include <stdbool.h>
#include <stdint.h>

#define LIQUID_GLASS_MAX_GROUPS 64u
#define LIQUID_GLASS_MAX_CLEAR_REGIONS (LIQUID_GLASS_MAX_GROUPS * 2u)

typedef struct liquid_glass_region {
    uint32_t x;
    uint32_t y;
    uint32_t width;
    uint32_t height;
} liquid_glass_region;

static inline bool liquid_glass_finite_rect(flux_rect rect) {
    return isfinite(rect.x) && isfinite(rect.y) && isfinite(rect.w) && isfinite(rect.h) &&
           rect.w > 0.0f && rect.h > 0.0f;
}

static inline bool liquid_glass_rect_contains(flux_rect outer, flux_rect inner) {
    double outer_right = (double)outer.x + outer.w;
    double outer_bottom = (double)outer.y + outer.h;
    double inner_right = (double)inner.x + inner.w;
    double inner_bottom = (double)inner.y + inner.h;
    return inner.x >= outer.x && inner.y >= outer.y && inner_right <= outer_right &&
           inner_bottom <= outer_bottom;
}

/* Validate one body's geometry and optical policy. A positive focus is an
 * interior field of a single body, never a second body or a smooth-union
 * participant, so its bounds must remain inside the primary shape. */
static inline bool liquid_glass_group_is_valid(const flux_liquid_glass_group *group) {
    if (!group || group->shape_count < 1u || group->shape_count > 2u ||
        !isfinite(group->blend_radius) || !isfinite(group->opacity) ||
        !isfinite(group->shadow_alpha) || !isfinite(group->shadow_blur) ||
        !isfinite(group->shadow_offset_y) || !isfinite(group->focus_strength))
        return false;
    for (uint32_t j = 0; j < group->shape_count; ++j) {
        if (!liquid_glass_finite_rect(group->shapes[j].bounds) ||
            !isfinite(group->shapes[j].corner_radius))
            return false;
    }
    if (group->focus_strength > 0.0f &&
        (group->shape_count != 1u || !liquid_glass_finite_rect(group->focus.bounds) ||
         !isfinite(group->focus.corner_radius) ||
         !liquid_glass_rect_contains(group->shapes[0].bounds, group->focus.bounds)))
        return false;
    return true;
}

/* Conservative physical-pixel footprint of one analytic body, including the
 * smooth-union bow, antialiasing, and drop-shadow falloff. The result is
 * clipped to the storage image and is therefore directly dispatchable. */
static inline bool liquid_glass_group_dispatch_bounds(const flux_liquid_glass_group *group,
                                                      float shadow_reach, uint32_t image_width,
                                                      uint32_t image_height,
                                                      liquid_glass_region *out) {
    if (!group || !out || group->shape_count < 1u || group->shape_count > 2u || image_width == 0u ||
        image_height == 0u)
        return false;

    float x0 = group->shapes[0].bounds.x;
    float y0 = group->shapes[0].bounds.y;
    float x1 = x0 + group->shapes[0].bounds.w;
    float y1 = y0 + group->shapes[0].bounds.h;
    if (group->shape_count == 2u) {
        flux_rect second = group->shapes[1].bounds;
        x0 = fminf(x0, second.x);
        y0 = fminf(y0, second.y);
        x1 = fmaxf(x1, second.x + second.w);
        y1 = fmaxf(y1, second.y + second.h);
    }

    float pad = fmaxf(fmaxf(group->blend_radius, 0.0f), fmaxf(shadow_reach, 0.0f)) + 2.0f;
    int64_t ix0 = (int64_t)floorf(x0 - pad);
    int64_t iy0 = (int64_t)floorf(y0 - pad);
    int64_t ix1 = (int64_t)ceilf(x1 + pad);
    int64_t iy1 = (int64_t)ceilf(y1 + pad);
    ix0 = ix0 < 0 ? 0 : ix0;
    iy0 = iy0 < 0 ? 0 : iy0;
    ix1 = ix1 > (int64_t)image_width ? (int64_t)image_width : ix1;
    iy1 = iy1 > (int64_t)image_height ? (int64_t)image_height : iy1;
    if (ix1 <= ix0 || iy1 <= iy0)
        return false;

    *out = (liquid_glass_region){
        .x = (uint32_t)ix0,
        .y = (uint32_t)iy0,
        .width = (uint32_t)(ix1 - ix0),
        .height = (uint32_t)(iy1 - iy0),
    };
    return true;
}

static inline bool liquid_glass_regions_overlap(liquid_glass_region a, liquid_glass_region b) {
    uint64_t ar = (uint64_t)a.x + a.width;
    uint64_t ab = (uint64_t)a.y + a.height;
    uint64_t br = (uint64_t)b.x + b.width;
    uint64_t bb = (uint64_t)b.y + b.height;
    return (uint64_t)a.x < br && (uint64_t)b.x < ar && (uint64_t)a.y < bb && (uint64_t)b.y < ab;
}

static inline liquid_glass_region liquid_glass_region_union(liquid_glass_region a,
                                                            liquid_glass_region b) {
    uint32_t x0 = a.x < b.x ? a.x : b.x;
    uint32_t y0 = a.y < b.y ? a.y : b.y;
    uint64_t ar = (uint64_t)a.x + a.width;
    uint64_t ab = (uint64_t)a.y + a.height;
    uint64_t br = (uint64_t)b.x + b.width;
    uint64_t bb = (uint64_t)b.y + b.height;
    uint64_t x1 = ar > br ? ar : br;
    uint64_t y1 = ab > bb ? ab : bb;
    return (liquid_glass_region){
        .x = x0,
        .y = y0,
        .width = (uint32_t)(x1 - x0),
        .height = (uint32_t)(y1 - y0),
    };
}

static inline bool liquid_glass_add_clear_region(liquid_glass_region region,
                                                 liquid_glass_region *out, uint32_t capacity,
                                                 uint32_t *count) {
    /* Re-start after every union: the enlarged rectangle can now overlap an
     * earlier entry that did not overlap the original input. This computes a
     * transitive merge while leaving truly disjoint HUD/Dock regions apart. */
    for (uint32_t i = 0; i < *count;) {
        if (!liquid_glass_regions_overlap(region, out[i])) {
            ++i;
            continue;
        }
        region = liquid_glass_region_union(region, out[i]);
        out[i] = out[--(*count)];
        i = 0;
    }
    if (*count >= capacity)
        return false;
    out[(*count)++] = region;
    return true;
}

/* Build the clear work for one frame slot. A newly allocated slot is cleared
 * exactly once over its complete extent. Reused slots clear previous AND
 * current footprints: previous removes pixels left by moved/shrunk/disappeared
 * groups, while current makes every overlapping group start from transparent.
 * All clears happen before any liquid dispatch, so a later group's clear can
 * never erase an earlier group's freshly written pixels. */
static inline bool
liquid_glass_build_clear_regions(bool initialized, uint32_t image_width, uint32_t image_height,
                                 const liquid_glass_region *previous, uint32_t previous_count,
                                 const liquid_glass_region *current, uint32_t current_count,
                                 liquid_glass_region *out, uint32_t capacity, uint32_t *out_count) {
    if (!out || !out_count || image_width == 0u || image_height == 0u ||
        previous_count > LIQUID_GLASS_MAX_GROUPS || current_count > LIQUID_GLASS_MAX_GROUPS ||
        (previous_count > 0u && !previous) || (current_count > 0u && !current))
        return false;

    *out_count = 0u;
    if (!initialized) {
        if (capacity == 0u)
            return false;
        out[0] = (liquid_glass_region){0u, 0u, image_width, image_height};
        *out_count = 1u;
        return true;
    }

    const liquid_glass_region *lists[2] = {previous, current};
    uint32_t counts[2] = {previous_count, current_count};
    for (uint32_t list = 0; list < 2u; ++list) {
        for (uint32_t i = 0; i < counts[list]; ++i) {
            liquid_glass_region region = lists[list][i];
            if (region.width == 0u || region.height == 0u || region.x >= image_width ||
                region.y >= image_height || region.width > image_width - region.x ||
                region.height > image_height - region.y)
                return false;
            if (!liquid_glass_add_clear_region(region, out, capacity, out_count))
                return false;
        }
    }
    return true;
}

#endif /* FLUX_EFFECT_LIQUID_GLASS_REGIONS_H */
