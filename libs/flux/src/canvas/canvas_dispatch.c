/*
 * Central path fill and stroke dispatcher (ADR-0121).
 *
 * Separates path dispatch authority from the legacy ear-clipping tessellator.
 * Routes paths directly to the modern analytic coverage backend (Vello-class
 * tile-compute or Slug-class fragment evaluation) when supported, or delegates
 * to the legacy triangulation pipeline only when explicit fallback is needed.
 */
#include "backend.h"
#include "internal.h"
#include <flux/core.h>

void canvas_fill_path_internal(flux_canvas *c, const flux_path *p, const canvas_paint *paint) {
    if (!c || !c->recording || !p || !paint)
        return;
    if (p->count == 0)
        return;

    /* Scratch is allocated once at canvas create (which fails without it). */
    if (!c->scratch_pts || !c->scratch_contours || !c->scratch_reflex)
        return;

    /* Primary analytic coverage path (ADR-0116, ADR-0119, ADR-0120, ADR-0121):
     * A backend that can rasterize contours directly takes the geometry as-is,
     * for EITHER fill rule. Winding is accumulated from sorted crossings or
     * GPU ray-casting, so even-odd needs no stencil attachment and no second pass.
     *
     * Active for SRC_OVER with any paint kind the coverage backend evaluates. */
    if (c->backend && c->backend->submit_cover && paint->blend == FLUX_BLEND_SRC_OVER) {
        flux_mat3x2 covex = c->states[c->state_top].transform;
        float cov_scale = flux_canvas_mat3x2_pixel_scale(covex);
        flatten_multi covfm =
            flatten_path_to_contours(p, cov_scale, c->scratch_pts, FLUX_CANVAS_PATH_SCRATCH_CAP,
                                     c->scratch_contours, FLUX_CANVAS_MAX_CONTOURS);
        if (covfm.contour_count > 0 && covfm.point_count >= 3) {
            /* Geometry crosses the seam in PHYSICAL PIXELS. */
            for (uint32_t ci = 0; ci < covfm.point_count; ++ci)
                c->scratch_pts[ci] = flux_mat3x2_transform_point(covex, c->scratch_pts[ci]);
            flux_recti sc = c->states[c->state_top].scissor;
            if (c->backend->submit_cover(c->backend, c, c->scratch_pts, c->scratch_contours,
                                         covfm.contour_count, paint->fill_rule, paint,
                                         sc) == FLUX_OK)
                return;
        }
    }

    /* Fallback path: explicit delegation to the tessellation and stencil pipeline. */
    canvas_tess_fill_path(c, p, paint);
}
