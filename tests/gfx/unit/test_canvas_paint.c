/* Public brush and stroke defaults are independent. */
#include "optics_test.h"
#include <optics/gfx.h>
int main(void) {
    optics_gfx_brush brush = optics_gfx_brush_solid(0xFF000000u);
    OT_EXPECT(brush.kind == OPTICS_GFX_BRUSH_SOLID);
    OT_EXPECT(brush.solid.color == 0xFF000000u);
    OT_EXPECT(brush.blend == OPTICS_GFX_BLEND_SRC_OVER);
    OT_EXPECT(brush.opacity == 1.0f);
    optics_gfx_geometry line = optics_gfx_geom_line(0, 0, 1, 1);
    OT_EXPECT(line.stroke.width == 1.0f);
    OT_EXPECT(line.stroke.cap == OPTICS_GFX_CAP_BUTT);
    OT_EXPECT(line.stroke.join == OPTICS_GFX_JOIN_MITER);
    return OT_REPORT();
}
