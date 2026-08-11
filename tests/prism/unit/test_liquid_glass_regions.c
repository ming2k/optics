/* Pure policy tests for persistent liquid-glass output clearing. */
#include "../../../libs/prism/src/regions.h"
#include "test_helpers.h"

#include <string.h>

static bool same_region(liquid_glass_region a, liquid_glass_region b) {
    return a.x == b.x && a.y == b.y && a.width == b.width && a.height == b.height;
}

static bool contains_region(const liquid_glass_region *regions, uint32_t count,
                            liquid_glass_region expected) {
    for (uint32_t i = 0; i < count; ++i) {
        if (same_region(regions[i], expected))
            return true;
    }
    return false;
}

int main(void) {
    /* Focus is an interior field of one body, not a second outline/body. */
    prism_liquid_glass_group focused = PRISM_LIQUID_GLASS_GROUP_INIT;
    focused.shapes[0] = (prism_liquid_glass_shape){
        .bounds = {10.0f, 20.0f, 100.0f, 60.0f},
        .corner_radius = 18.0f,
    };
    focused.focus = (prism_liquid_glass_shape){
        .bounds = {20.0f, 28.0f, 40.0f, 44.0f},
        .corner_radius = 12.0f,
    };
    focused.focus_strength = 1.0f;
    EXPECT(liquid_glass_group_is_valid(&focused));
    focused.shape_count = 2u;
    focused.shapes[1] = focused.focus;
    EXPECT(!liquid_glass_group_is_valid(&focused));
    focused.shape_count = 1u;
    focused.focus.bounds.x = 80.0f;
    focused.focus.bounds.w = 40.0f;
    EXPECT(!liquid_glass_group_is_valid(&focused));

    /* Bounds include smooth-union/shadow padding, round outward, and clamp. */
    prism_liquid_glass_group group = PRISM_LIQUID_GLASS_GROUP_INIT;
    group.shapes[0] = (prism_liquid_glass_shape){
        .bounds = {10.25f, 20.5f, 30.0f, 10.0f},
        .corner_radius = 5.0f,
    };
    group.blend_radius = 4.0f;
    liquid_glass_region bounds;
    EXPECT(liquid_glass_group_dispatch_bounds(&group, 0.0f, 100u, 80u, &bounds));
    EXPECT(same_region(bounds, (liquid_glass_region){4u, 14u, 43u, 23u}));

    group.shape_count = 2u;
    group.shapes[0].bounds = (flux_rect){-5.0f, 2.0f, 10.0f, 8.0f};
    group.shapes[1].bounds = (flux_rect){90.0f, 70.0f, 20.0f, 20.0f};
    EXPECT(liquid_glass_group_dispatch_bounds(&group, 12.0f, 100u, 80u, &bounds));
    EXPECT(same_region(bounds, (liquid_glass_region){0u, 0u, 100u, 80u}));
    group.shape_count = 1u;
    group.shapes[0].bounds = (flux_rect){200.0f, 10.0f, 8.0f, 8.0f};
    EXPECT(!liquid_glass_group_dispatch_bounds(&group, 0.0f, 100u, 80u, &bounds));

    /* Override/adaptive fields: INIT macro defaults are the inherit/disabled
     * sentinel and validate; zero-init is an explicit zero override, not
     * inherit. */
    prism_liquid_glass_group overrides = PRISM_LIQUID_GLASS_GROUP_INIT;
    overrides.shapes[0] = (prism_liquid_glass_shape){
        .bounds = {0.0f, 0.0f, 16.0f, 16.0f},
        .corner_radius = 4.0f,
    };
    EXPECT(overrides.frost_strength < 0.0f && overrides.tint_strength < 0.0f &&
           overrides.saturation < 0.0f && overrides.plate_polarity < 0.0f &&
           overrides.backdrop_energy < 0.0f);
    EXPECT(liquid_glass_group_is_valid(&overrides));

    /* NaN is rejected in each of the five fields; any other sentinel or
     * in-range value is accepted. */
    float *fields[5] = {&overrides.frost_strength, &overrides.tint_strength,
                        &overrides.saturation, &overrides.plate_polarity,
                        &overrides.backdrop_energy};
    for (uint32_t i = 0; i < 5u; ++i) {
        float keep = *fields[i];
        *fields[i] = NAN;
        EXPECT(!liquid_glass_group_is_valid(&overrides));
        *fields[i] = keep;
        EXPECT(liquid_glass_group_is_valid(&overrides));
    }
    /* plate_polarity / backdrop_energy accept <0 (disabled) or [0,1];
     * anything above the range is rejected. */
    overrides.plate_polarity = 0.0f;
    overrides.backdrop_energy = 1.0f;
    EXPECT(liquid_glass_group_is_valid(&overrides));
    overrides.plate_polarity = 1.0f + 1e-6f;
    EXPECT(!liquid_glass_group_is_valid(&overrides));
    overrides.plate_polarity = -2.0f;
    EXPECT(liquid_glass_group_is_valid(&overrides));
    overrides.backdrop_energy = 1.5f;
    EXPECT(!liquid_glass_group_is_valid(&overrides));
    overrides.backdrop_energy = -1.0f;
    EXPECT(liquid_glass_group_is_valid(&overrides));
    /* The strength overrides accept any finite value: <0 inherits. */
    overrides.frost_strength = 0.0f;
    overrides.tint_strength = 2.5f;
    overrides.saturation = -0.5f;
    EXPECT(liquid_glass_group_is_valid(&overrides));

    /* Inherit resolution: negative takes the desc value, non-negative is
     * verbatim (an explicit 0 pins the knob to zero). */
    EXPECT(liquid_glass_group_or_desc(-1.0f, 0.8f) == 0.8f);
    EXPECT(liquid_glass_group_or_desc(-0.001f, 0.8f) == 0.8f);
    EXPECT(liquid_glass_group_or_desc(0.0f, 0.8f) == 0.0f);
    EXPECT(liquid_glass_group_or_desc(0.3f, 0.8f) == 0.3f);

    /* Stats bounds: primary shape clipped to the image, rounded outward;
     * off-screen or zero-area bodies reduce an empty region. */
    liquid_glass_region stats = liquid_glass_group_stats_bounds(&group, 100u, 80u);
    EXPECT(stats.width == 0u && stats.height == 0u); /* group sits at x=200 */
    group.shapes[0].bounds = (flux_rect){-4.25f, 10.5f, 20.0f, 30.0f};
    stats = liquid_glass_group_stats_bounds(&group, 100u, 80u);
    EXPECT(same_region(stats, (liquid_glass_region){0u, 10u, 16u, 31u}));
    group.shapes[0].bounds = (flux_rect){90.0f, 60.0f, 40.0f, 40.0f};
    stats = liquid_glass_group_stats_bounds(&group, 100u, 80u);
    EXPECT(same_region(stats, (liquid_glass_region){90u, 60u, 10u, 20u}));

    liquid_glass_region previous[LIQUID_GLASS_MAX_GROUPS] = {0};
    liquid_glass_region current[LIQUID_GLASS_MAX_GROUPS] = {0};
    liquid_glass_region clear[LIQUID_GLASS_MAX_CLEAR_REGIONS] = {0};
    uint32_t clear_count = 0u;

    /* A new slot ignores stale bookkeeping and clears its full image once. */
    previous[0] = (liquid_glass_region){4u, 4u, 10u, 10u};
    current[0] = (liquid_glass_region){60u, 4u, 10u, 10u};
    EXPECT(liquid_glass_build_clear_regions(false, 100u, 80u, previous, 1u, current, 1u, clear,
                                            LIQUID_GLASS_MAX_CLEAR_REGIONS, &clear_count));
    EXPECT(clear_count == 1u);
    EXPECT(same_region(clear[0], (liquid_glass_region){0u, 0u, 100u, 80u}));

    /* A moved body clears both old and new footprints without joining the
     * disjoint regions into a full-width bounding box. */
    EXPECT(liquid_glass_build_clear_regions(true, 100u, 80u, previous, 1u, current, 1u, clear,
                                            LIQUID_GLASS_MAX_CLEAR_REGIONS, &clear_count));
    EXPECT(clear_count == 2u);
    EXPECT(contains_region(clear, clear_count, previous[0]));
    EXPECT(contains_region(clear, clear_count, current[0]));

    /* Shrink and transitive overlaps merge conservatively; touching-only
     * regions remain independently dispatchable. */
    previous[0] = (liquid_glass_region){10u, 10u, 30u, 30u};
    current[0] = (liquid_glass_region){15u, 15u, 5u, 5u};
    EXPECT(liquid_glass_build_clear_regions(true, 100u, 80u, previous, 1u, current, 1u, clear,
                                            LIQUID_GLASS_MAX_CLEAR_REGIONS, &clear_count));
    EXPECT(clear_count == 1u);
    EXPECT(same_region(clear[0], previous[0]));

    previous[0] = (liquid_glass_region){0u, 0u, 10u, 10u};
    current[0] = (liquid_glass_region){8u, 0u, 10u, 10u};
    current[1] = (liquid_glass_region){16u, 0u, 10u, 10u};
    EXPECT(liquid_glass_build_clear_regions(true, 100u, 80u, previous, 1u, current, 2u, clear,
                                            LIQUID_GLASS_MAX_CLEAR_REGIONS, &clear_count));
    EXPECT(clear_count == 1u);
    EXPECT(same_region(clear[0], (liquid_glass_region){0u, 0u, 26u, 10u}));

    previous[0] = (liquid_glass_region){0u, 0u, 10u, 10u};
    current[0] = (liquid_glass_region){10u, 0u, 10u, 10u};
    EXPECT(liquid_glass_build_clear_regions(true, 100u, 80u, previous, 1u, current, 1u, clear,
                                            LIQUID_GLASS_MAX_CLEAR_REGIONS, &clear_count));
    EXPECT(clear_count == 2u);

    /* Empty current state removes every previous footprint. */
    previous[0] = (liquid_glass_region){2u, 2u, 4u, 4u};
    previous[1] = (liquid_glass_region){20u, 20u, 4u, 4u};
    EXPECT(liquid_glass_build_clear_regions(true, 100u, 80u, previous, 2u, nullptr, 0u, clear,
                                            LIQUID_GLASS_MAX_CLEAR_REGIONS, &clear_count));
    EXPECT(clear_count == 2u);
    EXPECT(contains_region(clear, clear_count, previous[0]));
    EXPECT(contains_region(clear, clear_count, previous[1]));

    /* Worst case is exactly 64 previous + 64 current disjoint regions. */
    for (uint32_t i = 0; i < LIQUID_GLASS_MAX_GROUPS; ++i) {
        previous[i] = (liquid_glass_region){i * 4u, 0u, 1u, 1u};
        current[i] = (liquid_glass_region){i * 4u + 2u, 0u, 1u, 1u};
    }
    memset(clear, 0, sizeof(clear));
    EXPECT(liquid_glass_build_clear_regions(true, 300u, 2u, previous, LIQUID_GLASS_MAX_GROUPS,
                                            current, LIQUID_GLASS_MAX_GROUPS, clear,
                                            LIQUID_GLASS_MAX_CLEAR_REGIONS, &clear_count));
    EXPECT(clear_count == LIQUID_GLASS_MAX_CLEAR_REGIONS);
    EXPECT(!liquid_glass_build_clear_regions(true, 300u, 2u, previous, LIQUID_GLASS_MAX_GROUPS,
                                             current, LIQUID_GLASS_MAX_GROUPS, clear,
                                             LIQUID_GLASS_MAX_CLEAR_REGIONS - 1u, &clear_count));

    TEST_SUMMARY();
}
