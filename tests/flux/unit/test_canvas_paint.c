/*
 * Test canvas paint defaults.
 */
#include "test_helpers.h"
#include <flux/flux.h>

int main(void) {
    flux_paint p = flux_paint_default();

    EXPECT(p.kind == FLUX_PAINT_SOLID);
    EXPECT(p.color == 0xFF000000);
    EXPECT(p.cap == FLUX_CAP_BUTT);
    EXPECT(p.join == FLUX_JOIN_MITER);
    EXPECT(p.fill_rule == FLUX_FILL_NON_ZERO);
    EXPECT(p.blend == FLUX_BLEND_SRC_OVER);
    EXPECT_NEAR(p.stroke_width, 1.0f, 1e-6);
    EXPECT_NEAR(p.miter_limit, 4.0f, 1e-6);

    TEST_SUMMARY();
}
