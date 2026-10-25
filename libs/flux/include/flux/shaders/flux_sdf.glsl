/*
 * flux_sdf.glsl — the single GLSL definition of the analytic distance fields
 * (ADR-0118).
 *
 * <flux/sdf.h> is the host counterpart and evaluates the same expression;
 * tests/flux/unit/test_sdf.c pins the two differentially. Include this file
 * rather than writing another copy: before it existed the rounded-rectangle
 * field was written out eight times (the canvas SDF shader, four Prism
 * materials, one Prism squircle variant, an inline copy in the canvas image
 * shader, and the software backend's host port), which is exactly the
 * duplicated-definition drift that ADR-0112's postmortem identifies.
 *
 * Requires GLSL 4.50 or later. No version directive: include it after yours.
 *
 * OpenGL conventions apply as everywhere else in this repository: `float`,
 * `vec2`, `length()`, `clamp()`, `mix()` are NOT shadowed or redefined here.
 */

/* Signed distance to a rounded rectangle centred on the origin.
 *   half_extent: (hx, hy); negative components are clamped to zero
 *   radius:      clamped to [0, min(hx, hy)]
 * Negative inside, positive outside, zero on the boundary; the magnitude is
 * the Euclidean distance. `radius == 0` is a plain box. */
float flux_sdf_round_rect(vec2 p, vec2 half_extent, float radius)
{
    vec2 he = max(half_extent, vec2(0.0));
    float r = clamp(radius, 0.0, min(he.x, he.y));
    vec2 q = abs(p) - (he - vec2(r));
    return min(max(q.x, q.y), 0.0) + length(max(q, vec2(0.0))) - r;
}

/* Border of half-width `hw` around a shape whose field is `d`: the ring is
 * the set |d| <= hw. `hw <= 0` returns `d` unchanged, so this is a verbatim
 * replacement for the `if (stroke_hw > 0) d = abs(d) - stroke_hw;` idiom.
 * (`hw` rather than `half`: `half` is a reserved word in GLSL.) */
float flux_sdf_ring(float d, float hw)
{
    return (hw > 0.0) ? (abs(d) - hw) : d;
}

/* Squircle / continuous-curvature field: a rounded rectangle whose corner arc
 * blends a circular arc (`curvature == 0`, where this is *exactly*
 * flux_sdf_round_rect) with an L4 superellipse corner (`curvature == 1`).
 * Same conventions as flux_sdf_round_rect; the host counterpart is
 * flux_sdf_squircle in <flux/sdf.h>. */
float flux_sdf_squircle(vec2 p, vec2 half_extent, float radius, float curvature)
{
    vec2 he = max(half_extent, vec2(0.0));
    float r = clamp(radius, 0.0, min(he.x, he.y));
    vec2 q = abs(p) - (he - vec2(r));
    vec2 q_pos = max(q, vec2(0.0));
    float inner = min(max(q.x, q.y), 0.0);

    // Circular-corner estimate: identical to flux_sdf_round_rect.
    float d_l2 = length(q_pos) + inner - r;

    // L4 superellipse corner, gradient-normalised so the estimate stays
    // Eikonal across the corner arc.
    float q4 = pow(pow(q_pos.x, 4.0) + pow(q_pos.y, 4.0), 0.25);
    float grad = (q4 > 1e-4) ? sqrt(pow(q_pos.x, 6.0) + pow(q_pos.y, 6.0)) / pow(q4, 3.0) : 1.0;
    float d_g2 = (q4 - r) / max(grad, 0.5) + inner;

    return mix(d_l2, d_g2, clamp(curvature, 0.0, 1.0));
}
