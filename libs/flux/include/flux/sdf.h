/*
 * flux/sdf.h — signed distance fields for the analytic geometry classes
 * (ADR-0118).
 *
 * A closed-form distance field is exact and resolution-independent: coverage
 * is a smooth function of the screen-space distance from the boundary, so the
 * anti-aliasing is analytic instead of sampled. This is the peer of the curve
 * kernel (<flux/curve.h>): the kernel covers *parametric* geometry (Bézier
 * curves, which have no closed-form distance), this covers geometry for which
 * a closed-form distance does exist (circles, rounded rectangles, boxes).
 *
 * Placing them at the same level is deliberate. They are not competing
 * answers to one question; they are the two representations a 2D renderer
 * must serve, and each is the *correct* answer for its own domain:
 *
 *   analytic primitive -> distance field (this header). Exact, and cheaper
 *                         than flattening a shape that has an exact form.
 *   parametric curve   -> flux_curve kernel. The distance to a cubic has no
 *                         closed form, so the kernel reduces, offsets and
 *                         computes winding instead.
 *
 * A *baked* distance field (an SDF/MSDF texture) is a third thing, and it is
 * NOT placed here: it is an approximation whose quality is bounded by the
 * texture resolution, so it is an explicit opt-in optimisation, never a
 * silent fallback (ADR-0118).
 *
 * Sign convention: negative inside, positive outside, zero on the boundary;
 * the magnitude is the Euclidean distance in the caller's units.
 */

#ifndef FLUX_SDF_H
#define FLUX_SDF_H

#include <flux/core.h>
#include <flux/math.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Exact. */
FLUX_API float flux_sdf_circle(flux_point p, float radius);

/* Exact, and `radius == 0` is a plain box. `p` is relative to the shape's
 * centre; `radius` is clamped to [0, min(half_w, half_h)] and negative half
 * extents are treated as zero, so degenerate input yields a finite field
 * rather than a NaN. */
FLUX_API float flux_sdf_round_rect(flux_point p, float half_w, float half_h, float radius);

/* Border of half-width `half` around a shape whose signed distance is `d`:
 * the ring is the set |d| <= half. `half <= 0` returns `d` unchanged (a
 * border of zero width is the filled shape). */
FLUX_API float flux_sdf_ring(float d, float half);

/* Squircle / continuous-curvature field: a rounded rectangle whose corner arc
 * blends a circular arc (`curvature == 0`, where this is *exactly*
 * flux_sdf_round_rect) with an L4 superellipse corner (`curvature == 1`, the
 * "G2 squircle"). Same conventions as flux_sdf_round_rect.
 *
 * This is the definition the Prism materials have shipped since ADR-0046; it
 * is stated here, once, so the field has a single owner (ADR-0118 INV-SDF-02)
 * rather than living inside a material shader. See ADR-0118 for the open half
 * of INV-SDF-06: `flux_path_add_squircle` still draws a *different* corner
 * shape, so a caller must not assume the path and the field agree yet. */
FLUX_API float flux_sdf_squircle(flux_point p, float half_w, float half_h, float radius,
                                 float curvature);

/* Analytic coverage from a signed distance and the screen-space gradient
 * magnitude of the field (`aa`, i.e. how many caller units one pixel spans;
 * pass the result of the transform's scale extraction). Returns 1 well
 * inside, 0 well outside, and a smooth ramp across the boundary. `aa` is
 * floored at 1e-4 so a degenerate transform yields a hard edge rather than a
 * division blow-up. */
FLUX_API float flux_sdf_coverage(float d, float aa);

#ifdef __cplusplus
}
#endif

#endif /* FLUX_SDF_H */
