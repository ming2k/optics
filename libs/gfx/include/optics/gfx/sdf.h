/*
 * gfx/sdf.h — signed distance fields for the analytic geometry classes
 * (ADR-0118, ADR-0175), and their derivatives (ADR-0148).
 *
 * A closed-form distance field is exact and resolution-independent: coverage
 * is a smooth function of the screen-space distance from the boundary, so the
 * anti-aliasing is analytic instead of sampled. This is the peer of the curve
 * kernel (<optics/gfx/curve.h>): the kernel covers *parametric* geometry (Bézier
 * curves, which have no closed-form distance), this covers geometry for which
 * a closed-form distance does exist (circles, rounded rectangles, squircles,
 * boxes).
 *
 * A field's DERIVATIVE belongs here with it (ADR-0148). Every derivative here
 * is the analytic gradient of the very expression the corresponding field
 * evaluates, produced from one shared decomposition, so the two cannot
 * disagree.
 *
 * The one place a box field genuinely has no gradient is its interior medial
 * axis, where `max(b.x, b.y)` flips by 90 degrees across the diagonal. That is
 * a sampling question, not a differentiation one: optics_gfx_sdf_rrect_gradient
 * returns the continuum limit, and optics_gfx_sdf_rrect_sample resolves the
 * same crease over the caller's pixel footprint. Shade with the latter.
 *
 * Sign convention: negative inside, positive outside, zero on the boundary;
 * the magnitude is the Euclidean distance in the caller's units.
 */

#ifndef OPTICS_GFX_SDF_H
#define OPTICS_GFX_SDF_H

#include <optics/gfx/core.h>
#include <optics/gfx/math.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Exact signed distance to a circle centred on the origin. */
OPTICS_GFX_API float optics_gfx_sdf_circle(optics_gfx_point p, float radius);

/* Border of half-width `half` around a shape whose signed distance is `d`:
 * the ring is the set |d| <= half. `half <= 0` returns `d` unchanged (a
 * border of zero width is the filled shape). */
OPTICS_GFX_API float optics_gfx_sdf_ring(float d, float half);

/* One field evaluation, carrying both values a shading consumer needs. (A
 * struct rather than two output parameters: the device side returns the same
 * pair as a `float3`, and one evaluation must serve both or "the normal is the
 * derivative of the distance" is only a convention.) */
typedef struct optics_gfx_sdf_sample {
    float distance;          /* signed distance, same convention as every field here */
    optics_gfx_point normal; /* unit outward gradient; (0, 1) where undefined */
} optics_gfx_sdf_sample;

/* Exact analytic signed distance to a rounded rectangle / continuous-curvature
 * squircle centred on the origin with independent per-corner radii
 * `(tl, tr, br, bl)` (ADR-0118, ADR-0175).
 *
 * `curvature == 0` selects circular arcs; `curvature == 1` selects L4 G2
 * squircle arcs; intermediate values blend smoothly between them. All-zero
 * radii yield a plain axis-aligned box. Radii are proportionally normalised
 * per CSS/Skia rules (`optics_gfx_corner_radii_normalize`) so any individual
 * corner may claim up to `min(2 * half_w, 2 * half_h)` (e.g. a quarter-arc
 * `[0, 0, r, 0]` or bezel-flush tab `[0, 0, h, h]`) without mid-axis seams.
 * Negative half extents are treated as zero so degenerate input stays finite. */
OPTICS_GFX_API float optics_gfx_sdf_rrect(optics_gfx_point p, float half_w, float half_h,
                                          optics_gfx_corner_radii radii, float curvature);

/* Analytic unit gradient of optics_gfx_sdf_rrect with respect to `p` — the
 * outward surface normal of the field. Exact, and free of the four field
 * evaluations a finite-difference normal costs.
 *
 * The returned normal is the *continuum* gradient, so it is discontinuous
 * across the interior medial axis where the box field degenerates to a `max()`
 * of two planes. That is the correct limit and the wrong thing to shade with;
 * shade with optics_gfx_sdf_rrect_sample. */
OPTICS_GFX_API optics_gfx_sdf_sample optics_gfx_sdf_rrect_gradient(optics_gfx_point p, float half_w,
                                                                   float half_h,
                                                                   optics_gfx_corner_radii radii,
                                                                   float curvature);

/* Field and shading normal from ONE evaluation.
 *
 * `aa` is the sampling footprint in the field's own units (how many caller
 * units one pixel spans — the same quantity optics_gfx_sdf_coverage takes). It
 * is read in exactly one place: the interior box medial axis, where it resolves
 * the crease over one pixel footprint instead of flipping across zero width.
 * Everywhere else the normal is the exact analytic gradient and is independent
 * of `aa`. */
OPTICS_GFX_API optics_gfx_sdf_sample optics_gfx_sdf_rrect_sample(optics_gfx_point p, float half_w,
                                                                 float half_h,
                                                                 optics_gfx_corner_radii radii,
                                                                 float curvature, float aa);

/* Composition: the polynomial smooth-minimum of two fields — the metaball union
 * a fused body is made of. `radius <= 0` is a hard min. Returns the field;
 * optics_gfx_sdf_smooth_union_gradient returns its unit gradient. */
OPTICS_GFX_API float optics_gfx_sdf_smooth_union(float a, float b, float radius);

/* Unit gradient of optics_gfx_sdf_smooth_union with respect to position, from
 * the two fields' unit gradients and their values. */
OPTICS_GFX_API optics_gfx_point optics_gfx_sdf_smooth_union_gradient(optics_gfx_point grad_a,
                                                                     optics_gfx_point grad_b,
                                                                     float a, float b,
                                                                     float radius);

/* Analytic coverage from a signed distance and the screen-space gradient
 * magnitude of the field (`aa`, i.e. how many caller units one pixel spans;
 * pass the result of the transform's scale extraction). Returns 1 well inside,
 * 0 well outside, and a smooth ramp across the boundary. `aa` is floored at
 * 1e-4 so a degenerate transform yields a hard edge rather than a division
 * blow-up. */
OPTICS_GFX_API float optics_gfx_sdf_coverage(float d, float aa);

#ifdef __cplusplus
}
#endif

#endif /* OPTICS_GFX_SDF_H */
