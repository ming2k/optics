/* Pure policy tests for persistent liquid-glass output clearing. */
#include "../../../libs/material/src/regions.h"
#include "optics_test.h"

#include <string.h>

static bool same_region(optics_material__rect a, optics_material__rect b) {
    return a.x == b.x && a.y == b.y && a.width == b.width && a.height == b.height;
}

static bool contains_region(const optics_material__rect *regions, uint32_t count,
                            optics_material__rect expected) {
    for (uint32_t i = 0; i < count; ++i) {
        if (same_region(regions[i], expected))
            return true;
    }
    return false;
}

int main(void) {
    /* Focus is an interior field of one body, not a second outline/body.
     * Shapes are a caller-owned borrowed array (see the group doc). */
    optics_material_liquid_glass_shape focused_shapes[2] = {
        {.bounds = {10.0f, 20.0f, 100.0f, 60.0f}, .corner_radius = 18.0f},
        {},
    };
    optics_material_liquid_glass_group focused = OPTICS_MATERIAL_LIQUID_GLASS_GROUP_INIT;
    focused.shapes = focused_shapes;
    focused.shape_count = 1u;
    focused.focus = (optics_material_liquid_glass_shape){
        .bounds = {20.0f, 28.0f, 40.0f, 44.0f},
        .corner_radius = 12.0f,
    };
    focused.focus_strength = 1.0f;
    OT_EXPECT(liquid_glass_group_is_valid(&focused));
    focused_shapes[1] = focused.focus;
    focused.shape_count = 2u;
    OT_EXPECT(!liquid_glass_group_is_valid(&focused));
    focused.shape_count = 1u;
    focused.focus.bounds.x = 80.0f;
    focused.focus.bounds.w = 40.0f;
    OT_EXPECT(!liquid_glass_group_is_valid(&focused));

    /* A group without a shapes array is invalid (the borrowed pointer is
     * part of the contract — INIT leaves it unset on purpose). */
    optics_material_liquid_glass_group null_shapes = OPTICS_MATERIAL_LIQUID_GLASS_GROUP_INIT;
    OT_EXPECT(!liquid_glass_group_is_valid(&null_shapes));

    /* Bounds include smooth-union/shadow padding, round outward, and clamp. */
    optics_material_liquid_glass_shape group_shapes[2] = {
        {.bounds = {10.25f, 20.5f, 30.0f, 10.0f}, .corner_radius = 5.0f},
        {},
    };
    optics_material_liquid_glass_group group = OPTICS_MATERIAL_LIQUID_GLASS_GROUP_INIT;
    group.shapes = group_shapes;
    group.shape_count = 1u;
    group.blend_radius = 4.0f;
    optics_material__rect bounds;
    OT_EXPECT(liquid_glass_group_dispatch_bounds(&group, 0.0f, 100u, 80u, &bounds));
    OT_EXPECT(same_region(bounds, (optics_material__rect){4u, 14u, 43u, 23u}));

    group.shape_count = 2u;
    group_shapes[0].bounds = (optics_gfx_rect){-5.0f, 2.0f, 10.0f, 8.0f};
    group_shapes[1].bounds = (optics_gfx_rect){90.0f, 70.0f, 20.0f, 20.0f};
    OT_EXPECT(liquid_glass_group_dispatch_bounds(&group, 12.0f, 100u, 80u, &bounds));
    OT_EXPECT(same_region(bounds, (optics_material__rect){0u, 0u, 100u, 80u}));
    group.shape_count = 1u;
    group_shapes[0].bounds = (optics_gfx_rect){200.0f, 10.0f, 8.0f, 8.0f};
    OT_EXPECT(!liquid_glass_group_dispatch_bounds(&group, 0.0f, 100u, 80u, &bounds));

    /* Override/adaptive fields: INIT macro defaults are the inherit/disabled
     * sentinel and validate; zero-init is an explicit zero override, not
     * inherit. */
    optics_material_liquid_glass_shape override_shapes[1] = {
        {.bounds = {0.0f, 0.0f, 16.0f, 16.0f}, .corner_radius = 4.0f},
    };
    optics_material_liquid_glass_group overrides = OPTICS_MATERIAL_LIQUID_GLASS_GROUP_INIT;
    overrides.shapes = override_shapes;
    overrides.shape_count = 1u;
    OT_EXPECT(overrides.frost_strength < 0.0f && overrides.tint_strength < 0.0f &&
              overrides.saturation < 0.0f && overrides.contact_ao < 0.0f &&
              overrides.ambient_fresnel < 0.0f);
    OT_EXPECT(liquid_glass_group_is_valid(&overrides));

    /* NaN is rejected in each of the fields; any other sentinel or
     * in-range value is accepted. */
    float *fields[5] = {&overrides.frost_strength, &overrides.tint_strength, &overrides.saturation,
                        &overrides.contact_ao, &overrides.ambient_fresnel};
    for (uint32_t i = 0; i < 5u; ++i) {
        float keep = *fields[i];
        *fields[i] = NAN;
        OT_EXPECT(!liquid_glass_group_is_valid(&overrides));
        *fields[i] = keep;
        OT_EXPECT(liquid_glass_group_is_valid(&overrides));
    }
    /* contact_ao / ambient_fresnel accept <0 (disabled) or [0,1];
     * anything above the range is rejected. */
    overrides.contact_ao = 0.5f;
    overrides.ambient_fresnel = 0.7f;
    OT_EXPECT(liquid_glass_group_is_valid(&overrides));
    overrides.contact_ao = 1.0f + 1e-6f;
    OT_EXPECT(!liquid_glass_group_is_valid(&overrides));
    overrides.contact_ao = -1.0f;
    OT_EXPECT(liquid_glass_group_is_valid(&overrides));
    overrides.ambient_fresnel = 1.5f;
    OT_EXPECT(!liquid_glass_group_is_valid(&overrides));
    overrides.ambient_fresnel = -1.0f;
    OT_EXPECT(liquid_glass_group_is_valid(&overrides));
    /* The strength overrides accept any finite value: <0 inherits. */
    overrides.frost_strength = 0.0f;
    overrides.tint_strength = 2.5f;
    overrides.saturation = -0.5f;
    OT_EXPECT(liquid_glass_group_is_valid(&overrides));

    /* Inherit resolution: negative takes the desc value, non-negative is
     * verbatim (an explicit 0 pins the knob to zero). */
    OT_EXPECT(liquid_glass_group_or_desc(-1.0f, 0.8f) == 0.8f);
    OT_EXPECT(liquid_glass_group_or_desc(-0.001f, 0.8f) == 0.8f);
    OT_EXPECT(liquid_glass_group_or_desc(0.0f, 0.8f) == 0.0f);
    OT_EXPECT(liquid_glass_group_or_desc(0.3f, 0.8f) == 0.3f);

    /* Stats bounds: primary shape clipped to the image, rounded outward;
     * off-screen or zero-area bodies reduce an empty region. */
    optics_material__rect stats = liquid_glass_group_stats_bounds(&group, 100u, 80u);
    OT_EXPECT(stats.width == 0u && stats.height == 0u); /* group sits at x=200 */
    group_shapes[0].bounds = (optics_gfx_rect){-4.25f, 10.5f, 20.0f, 30.0f};
    stats = liquid_glass_group_stats_bounds(&group, 100u, 80u);
    OT_EXPECT(same_region(stats, (optics_material__rect){0u, 10u, 16u, 31u}));
    group_shapes[0].bounds = (optics_gfx_rect){90.0f, 60.0f, 40.0f, 40.0f};
    stats = liquid_glass_group_stats_bounds(&group, 100u, 80u);
    OT_EXPECT(same_region(stats, (optics_material__rect){90u, 60u, 10u, 20u}));

    optics_material__rect previous[LIQUID_GLASS_MAX_GROUPS] = {};
    optics_material__rect current[LIQUID_GLASS_MAX_GROUPS] = {};
    optics_material__rect clear[LIQUID_GLASS_MAX_CLEAR_REGIONS] = {};
    uint32_t clear_count = 0u;

    /* A new slot ignores stale bookkeeping and clears its full image once. */
    previous[0] = (optics_material__rect){4u, 4u, 10u, 10u};
    current[0] = (optics_material__rect){60u, 4u, 10u, 10u};
    OT_EXPECT(liquid_glass_build_clear_regions(false, 100u, 80u, previous, 1u, current, 1u, clear,
                                               LIQUID_GLASS_MAX_CLEAR_REGIONS, &clear_count));
    OT_EXPECT(clear_count == 1u);
    OT_EXPECT(same_region(clear[0], (optics_material__rect){0u, 0u, 100u, 80u}));

    /* A moved body clears both old and new footprints without joining the
     * disjoint regions into a full-width bounding box. */
    OT_EXPECT(liquid_glass_build_clear_regions(true, 100u, 80u, previous, 1u, current, 1u, clear,
                                               LIQUID_GLASS_MAX_CLEAR_REGIONS, &clear_count));
    OT_EXPECT(clear_count == 2u);
    OT_EXPECT(contains_region(clear, clear_count, previous[0]));
    OT_EXPECT(contains_region(clear, clear_count, current[0]));

    /* Shrink and transitive overlaps merge conservatively; touching-only
     * regions remain independently dispatchable. */
    previous[0] = (optics_material__rect){10u, 10u, 30u, 30u};
    current[0] = (optics_material__rect){15u, 15u, 5u, 5u};
    OT_EXPECT(liquid_glass_build_clear_regions(true, 100u, 80u, previous, 1u, current, 1u, clear,
                                               LIQUID_GLASS_MAX_CLEAR_REGIONS, &clear_count));
    OT_EXPECT(clear_count == 1u);
    OT_EXPECT(same_region(clear[0], previous[0]));

    previous[0] = (optics_material__rect){0u, 0u, 10u, 10u};
    current[0] = (optics_material__rect){8u, 0u, 10u, 10u};
    current[1] = (optics_material__rect){16u, 0u, 10u, 10u};
    OT_EXPECT(liquid_glass_build_clear_regions(true, 100u, 80u, previous, 1u, current, 2u, clear,
                                               LIQUID_GLASS_MAX_CLEAR_REGIONS, &clear_count));
    OT_EXPECT(clear_count == 1u);
    OT_EXPECT(same_region(clear[0], (optics_material__rect){0u, 0u, 26u, 10u}));

    previous[0] = (optics_material__rect){0u, 0u, 10u, 10u};
    current[0] = (optics_material__rect){10u, 0u, 10u, 10u};
    OT_EXPECT(liquid_glass_build_clear_regions(true, 100u, 80u, previous, 1u, current, 1u, clear,
                                               LIQUID_GLASS_MAX_CLEAR_REGIONS, &clear_count));
    OT_EXPECT(clear_count == 2u);

    /* Empty current state removes every previous footprint. */
    previous[0] = (optics_material__rect){2u, 2u, 4u, 4u};
    previous[1] = (optics_material__rect){20u, 20u, 4u, 4u};
    OT_EXPECT(liquid_glass_build_clear_regions(true, 100u, 80u, previous, 2u, nullptr, 0u, clear,
                                               LIQUID_GLASS_MAX_CLEAR_REGIONS, &clear_count));
    OT_EXPECT(clear_count == 2u);
    OT_EXPECT(contains_region(clear, clear_count, previous[0]));
    OT_EXPECT(contains_region(clear, clear_count, previous[1]));

    /* Worst case is exactly 64 previous + 64 current disjoint regions. */
    for (uint32_t i = 0; i < LIQUID_GLASS_MAX_GROUPS; ++i) {
        previous[i] = (optics_material__rect){i * 4u, 0u, 1u, 1u};
        current[i] = (optics_material__rect){i * 4u + 2u, 0u, 1u, 1u};
    }
    memset(clear, 0, sizeof(clear));
    OT_EXPECT(liquid_glass_build_clear_regions(true, 300u, 2u, previous, LIQUID_GLASS_MAX_GROUPS,
                                               current, LIQUID_GLASS_MAX_GROUPS, clear,
                                               LIQUID_GLASS_MAX_CLEAR_REGIONS, &clear_count));
    OT_EXPECT(clear_count == LIQUID_GLASS_MAX_CLEAR_REGIONS);

    /* A dispatch budget too small for the coalesced set must NOT fail the
     * call: capacity limits how many rectangles we can dispatch, never which
     * descriptors are legal. It degrades to one full-image clear, which is a
     * superset of every footprint and therefore still correct. */
    memset(clear, 0, sizeof(clear));
    OT_EXPECT(liquid_glass_build_clear_regions(true, 300u, 2u, previous, LIQUID_GLASS_MAX_GROUPS,
                                               current, LIQUID_GLASS_MAX_GROUPS, clear,
                                               LIQUID_GLASS_MAX_CLEAR_REGIONS - 1u, &clear_count));
    OT_EXPECT(clear_count == 1u);
    OT_EXPECT(clear[0].x == 0u && clear[0].y == 0u);
    OT_EXPECT(clear[0].width == 300u && clear[0].height == 2u);

    /* The true overflow floor: a single-region budget always succeeds because
     * one full-image rectangle is always dispatchable. */
    memset(clear, 0, sizeof(clear));
    OT_EXPECT(liquid_glass_build_clear_regions(true, 300u, 2u, previous, LIQUID_GLASS_MAX_GROUPS,
                                               current, LIQUID_GLASS_MAX_GROUPS, clear, 1u,
                                               &clear_count));
    OT_EXPECT(clear_count == 1u);
    OT_EXPECT(clear[0].width == 300u && clear[0].height == 2u);

    /* A zero budget is a caller bug, not a descriptor problem: there is no
     * rectangle to write the fallback into. */
    OT_EXPECT(!liquid_glass_build_clear_regions(true, 300u, 2u, previous, LIQUID_GLASS_MAX_GROUPS,
                                                current, LIQUID_GLASS_MAX_GROUPS, clear, 0u,
                                                &clear_count));

    /* Footprints outside the image stay a hard error: they are malformed
     * geometry, not a capacity shortfall, and silently clipping them would
     * hide a caller bug in the dispatch bounds. */
    {
        optics_material__rect bad[1] = {{(uint32_t)300, 0u, 4u, 1u}};
        OT_EXPECT(!liquid_glass_build_clear_regions(true, 300u, 2u, bad, 1u, nullptr, 0u, clear,
                                                    LIQUID_GLASS_MAX_CLEAR_REGIONS, &clear_count));
    }

    /* --- ordering-barrier predicate ---
     *
     * `liquid_glass_region_overlaps_any` decides whether a material dispatch
     * needs an ordering barrier against the dispatches already recorded. Vulkan
     * has no sub-rectangle image barrier, so the only way to drop N-1
     * whole-image barriers per frame is to prove the dispatches write disjoint
     * memory. This predicate is therefore a correctness gate, not an
     * optimisation detail, and it is the one part of the decision a GPU cannot
     * exercise: an actual missing barrier is undefined behaviour that current
     * hardware happens to serialize away, so no pixel test can catch a
     * regression here. These cases are exact on purpose. */
    {
        const optics_material__rect a = {10u, 10u, 20u, 20u}; /* x 10..30, y 10..30 */

        /* Disjoint on every side, and by exactly one pixel. */
        optics_material__rect left[1] = {{0u, 0u, 10u, 40u}};  /* right edge == a.x */
        optics_material__rect above[1] = {{0u, 0u, 40u, 10u}}; /* bottom edge == a.y */
        OT_EXPECT(!liquid_glass_region_overlaps_any(a, left, 1u));
        OT_EXPECT(!liquid_glass_region_overlaps_any(a, above, 1u));
        /* Symmetry: adjacency is not overlap in either direction. */
        OT_EXPECT(!liquid_glass_regions_overlap(left[0], a));
        OT_EXPECT(!liquid_glass_regions_overlap(a, left[0]));

        /* A single shared pixel is overlap. */
        optics_material__rect touching[1] = {{29u, 29u, 5u, 5u}}; /* overlaps at (29,29) */
        OT_EXPECT(liquid_glass_region_overlaps_any(a, touching, 1u));

        /* Containment in both directions. */
        optics_material__rect inside[1] = {{15u, 15u, 2u, 2u}};
        optics_material__rect around[1] = {{0u, 0u, 64u, 64u}};
        OT_EXPECT(liquid_glass_region_overlaps_any(a, inside, 1u));
        OT_EXPECT(liquid_glass_region_overlaps_any(a, around, 1u));
        OT_EXPECT(liquid_glass_region_overlaps_any(inside[0], &a, 1u));

        /* Identical rectangles (the stacked-card case) overlap. */
        optics_material__rect same[1] = {a};
        OT_EXPECT(liquid_glass_region_overlaps_any(a, same, 1u));

        /* One match anywhere in the list is enough; an empty list never
         * matches (the first dispatch of a frame needs no barrier). */
        optics_material__rect many[4] = {
            {0u, 0u, 4u, 4u}, {100u, 100u, 4u, 4u}, {12u, 12u, 1u, 1u}, {200u, 200u, 4u, 4u}};
        OT_EXPECT(liquid_glass_region_overlaps_any(a, many, 4u));
        OT_EXPECT(liquid_glass_region_overlaps_any(a, many, 3u));
        OT_EXPECT(!liquid_glass_region_overlaps_any(a, many, 2u)); /* first two are far away */
        OT_EXPECT(!liquid_glass_region_overlaps_any(a, many, 0u));

        /* A row of disjoint bodies — the toolbar layout the optimisation
         * exists for — must never report an overlap. */
        optics_material__rect row[8];
        for (uint32_t i = 0; i < 8u; ++i)
            row[i] = (optics_material__rect){i * 32u, 0u, 30u, 40u};
        for (uint32_t i = 0; i < 8u; ++i)
            OT_EXPECT(!liquid_glass_region_overlaps_any(row[i], row, i));
    }

    return OT_REPORT();
}
