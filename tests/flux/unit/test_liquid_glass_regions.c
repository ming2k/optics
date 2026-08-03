/* Pure policy tests for persistent liquid-glass output clearing. */
#include "../../../libs/flux/src/effect/liquid_glass_regions.h"
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
    flux_liquid_glass_group focused = FLUX_LIQUID_GLASS_GROUP_INIT;
    focused.shapes[0] = (flux_liquid_glass_shape){
        .bounds = {10.0f, 20.0f, 100.0f, 60.0f},
        .corner_radius = 18.0f,
    };
    focused.focus = (flux_liquid_glass_shape){
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
    flux_liquid_glass_group group = FLUX_LIQUID_GLASS_GROUP_INIT;
    group.shapes[0] = (flux_liquid_glass_shape){
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
