/*
 * Central path fill and stroke dispatcher (ADR-0121).
 *
 * Routes paths directly to the analytic coverage backend (tile compute on
 * Vulkan, 1x analytic coverage rasterizer on CPU) without ear-clipping or
 * stencil fallback.
 */
#include "backend.h"
#include "internal.h"
#include <optics/gfx/core.h>

void canvas_fill_path_internal(optics_gfx_canvas *c, const optics_gfx_path *p,
                               const canvas_paint *paint) {
    if (!c || !c->recording || !p || !paint)
        return;
    if (p->count == 0)
        return;

    /* Scratch is allocated once at canvas create (which fails without it). */
    if (!c->scratch_pts || !c->scratch_contours)
        return;

    if (!c->backend || !c->backend->submit_cover) {
        canvas_fail(c, OPTICS_GFX_ERROR_UNSUPPORTED,
                    "analytic path fill unsupported by canvas backend");
        return;
    }

    optics_gfx_mat3x2 covex = c->states[c->state_top].transform;
    float cov_scale = optics_gfx_canvas_mat3x2_pixel_scale(covex);
    flatten_multi covfm = canvas_flatten_path(c, p, cov_scale);
    if (covfm.contour_count == 0 || covfm.point_count < 3)
        return;

    /* Geometry crosses the seam in PHYSICAL PIXELS. */
    for (uint32_t ci = 0; ci < covfm.point_count; ++ci)
        c->scratch_pts[ci] = optics_gfx_mat3x2_transform_point(covex, c->scratch_pts[ci]);
    optics_gfx_recti sc = c->states[c->state_top].scissor;
    optics_gfx_result r =
        c->backend->submit_cover(c->backend, c, c->scratch_pts, c->scratch_contours,
                                 covfm.contour_count, paint->fill_rule, paint, sc);
    if (r != OPTICS_GFX_OK)
        canvas_fail(c, r, "analytic path fill submission failed");
}
