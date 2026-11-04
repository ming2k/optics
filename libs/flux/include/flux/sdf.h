/*
 * flux/sdf.h — signed distance fields for the analytic geometry classes
 * (ADR-0118), and their derivatives (ADR-0148).
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
 * A field's DERIVATIVE belongs here with it (ADR-0148). The normal of an
 * analytic primitive is a closed-form expression, not something to recover by
 * sampling the field four times and differencing: those four taps cost four
 * field evaluations per shaded pixel, they are only first-order accurate, and
 * they make the normal a *neighbour* of the field rather than a property of
 * it. Every derivative here is the analytic gradient of the very expression the
 * corresponding field evaluates, produced from one shared decomposition, so the
 * two cannot disagree.
 *
 * The one place a field genuinely has no gradient is the interior region of a
 * rounded rectangle, where it degenerates to `max(q.x, q.y) - r` and flips by
 * 90 degrees across the diagonal. That is a sampling question, not a
 * differentiation one: flux_sdf_squircle_gradient returns the continuum limit,
 * and flux_sdf_squircle_sample resolves the same crease over the caller's pixel
 * footprint. Shade with the latter.
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

/* One field evaluation, carrying both values a shading consumer needs. (A
 * struct rather than two output parameters: the device side returns the same
 * pair as a `vec3`, and one evaluation must serve both or "the normal is the
 * derivative of the distance" is only a convention.) */
typedef struct flux_sdf_sample {
    float distance;    /* signed distance, same convention as every field here */
    flux_point normal; /* unit outward gradient; (0, 1) where undefined */
} flux_sdf_sample;

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

/* Analytic unit gradient of flux_sdf_squircle with respect to `p` — the outward
 * surface normal of the field. Exact, and free of the four field evaluations a
 * finite-difference normal costs.
 *
 * The returned normal is the *continuum* gradient, so it is discontinuous
 * across the interior diagonal where the field degenerates to a `max()` of two
 * planes. That is the correct limit and the wrong thing to shade with; shade
 * with flux_sdf_squircle_sample. */
FLUX_API flux_sdf_sample flux_sdf_squircle_gradient(flux_point p, float half_w, float half_h,
                                                    float radius, float curvature);

/* Field and shading normal from ONE evaluation.
 *
 * `aa` is the sampling footprint in the field's own units (how many caller
 * units one pixel spans — the same quantity flux_sdf_coverage takes). It is
 * read in exactly one place: the interior, where it resolves the crease over
 * one pixel footprint instead of flipping across zero width. That is the
 * continuous limit of what a central difference of the same span measures, and
 * it is why this entry point exists rather than the caller differencing the
 * field itself. Everywhere else the normal is the exact analytic gradient and
 * is independent of `aa`. */
FLUX_API flux_sdf_sample flux_sdf_squircle_sample(flux_point p, float half_w, float half_h,
                                                  float radius, float curvature, float aa);

/* Composition: the polynomial smooth-minimum of two fields — the metaball union
 * a fused body is made of. `radius <= 0` is a hard min. Returns the field;
 * flux_sdf_smooth_union_gradient returns its unit gradient.
 *
 * The composition lives here, not in a material, for the same reason the fields
 * do: a fused body is one field, and a material that carried its own union
 * would be a second definition of it (ADR-0148). */
FLUX_API float flux_sdf_smooth_union(float a, float b, float radius);

/* Unit gradient of flux_sdf_smooth_union with respect to position, from the two
 * fields' unit gradients and their values.
 *
 * The blend's own `h` definition gives `a - b == radius*(1 - 2h)` exactly, so the
 * chain-rule term through dh/dp vanishes identically and the gradient is exactly
 * linear in h. */
FLUX_API flux_point flux_sdf_smooth_union_gradient(flux_point grad_a, flux_point grad_b, float a,
                                                   float b, float radius);

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
