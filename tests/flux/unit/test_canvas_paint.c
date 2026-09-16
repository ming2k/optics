/* Public brush and stroke defaults are independent. */
#include "test_helpers.h"
#include <flux/flux.h>
int main(void) {
    flux_brush brush = flux_brush_solid(0xFF000000u);
    EXPECT(brush.kind == FLUX_BRUSH_SOLID);
    EXPECT(brush.solid.color == 0xFF000000u);
    EXPECT(brush.blend == FLUX_BLEND_SRC_OVER);
    EXPECT(brush.opacity == 1.0f);
    flux_geometry line = flux_geom_line(0, 0, 1, 1);
    EXPECT(line.stroke.width == 1.0f);
    EXPECT(line.stroke.cap == FLUX_CAP_BUTT);
    EXPECT(line.stroke.join == FLUX_JOIN_MITER);
    TEST_SUMMARY();
}
