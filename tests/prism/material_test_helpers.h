#ifndef PRISM_TEST_MATERIAL_HELPERS_H
#define PRISM_TEST_MATERIAL_HELPERS_H

/*
 * Shared helpers for the prism integration suites.
 *
 * A prism material paints only inside its own footprints, so its output image is
 * slab-sized rather than capture-sized (prism_material_output documents why).
 * Every suite therefore has to draw the result at its placement instead of
 * blitting it edge to edge. Keeping that in one helper means a change to the
 * placement contract touches one line, not every test.
 */

#include <flux/flux.h>
#include <prism/prism.h>

/* Draw a material output where it belongs. Everything inside the slab but outside
 * the material's footprints is transparent, so this is always the correct draw. */
static inline void test_draw_material(flux_canvas *canvas, prism_material_output out) {
    if (!out.image)
        return;
    flux_canvas_draw_image(
        canvas, out.image,
        (flux_rect){out.origin_x, out.origin_y, (float)out.width, (float)out.height}, nullptr);
}

#endif /* PRISM_TEST_MATERIAL_HELPERS_H */
