#ifndef OPTICS_MATERIAL_TEST_MATERIAL_HELPERS_H
#define OPTICS_MATERIAL_TEST_MATERIAL_HELPERS_H

/*
 * Shared helpers for the material integration suites.
 *
 * A material paints only inside its own footprints, so its output image is
 * slab-sized rather than capture-sized (optics_material_output documents why).
 * Every suite therefore has to draw the result at its placement instead of
 * blitting it edge to edge. Keeping that in one helper means a change to the
 * placement contract touches one line, not every test.
 */

#include <optics/gfx.h>
#include <optics/material.h>

/* Draw a material output where it belongs. Everything inside the slab but outside
 * the material's footprints is transparent, so this is always the correct draw. */
static inline void test_draw_material(optics_gfx_canvas *canvas, optics_material_output out) {
    if (!out.image)
        return;
    optics_gfx_canvas_draw_image(
        canvas, out.image,
        (optics_gfx_rect){out.origin_x, out.origin_y, (float)out.width, (float)out.height},
        nullptr);
}

#endif /* OPTICS_MATERIAL_TEST_MATERIAL_HELPERS_H */
