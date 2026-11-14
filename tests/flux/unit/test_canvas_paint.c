/* Public brush and stroke defaults are independent. */
#include "optics_test.h"
#include <flux/flux.h>
int main(void) {
    flux_brush brush = flux_brush_solid(0xFF000000u);
    OT_EXPECT(brush.kind == FLUX_BRUSH_SOLID);
    OT_EXPECT(brush.solid.color == 0xFF000000u);
    OT_EXPECT(brush.blend == FLUX_BLEND_SRC_OVER);
    OT_EXPECT(brush.opacity == 1.0f);
    flux_geometry line = flux_geom_line(0, 0, 1, 1);
    OT_EXPECT(line.stroke.width == 1.0f);
    OT_EXPECT(line.stroke.cap == FLUX_CAP_BUTT);
    OT_EXPECT(line.stroke.join == FLUX_JOIN_MITER);
    return OT_REPORT();
}
