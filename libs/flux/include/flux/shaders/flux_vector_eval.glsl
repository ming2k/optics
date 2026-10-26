/*
 * flux_vector_eval.glsl — the single GLSL definition for analytic fragment
 * vector evaluation (ADR-0116, ADR-0117, ADR-0121).
 *
 * <flux/curve.h> and <flux/canvas_tile.h> provide host-side line segment
 * representations; tests/flux/integration/test_scene_vector.c pins the 3D
 * evaluation and tests/flux/unit/test_canvas_coverage.c pins the 2D evaluation.
 *
 * Include this file rather than duplicating the ray-cast winding walk, distance
 * calculation, and Jacobian screen-space anti-aliasing logic across 2D canvas
 * and 3D scene vector shaders.
 *
 * Requires GLSL 4.50 or later. No version directive: include it after yours.
 *
 * OpenGL conventions apply as everywhere else in this repository: `float`,
 * `vec2`, `length()`, `clamp()`, `mix()`, `dFdx()`, `dFdy()` are NOT shadowed.
 */

/* Accumulate winding crossing and Euclidean boundary distance for a 2D line segment.
 *   p: query point (in object space or 2D pixel space)
 *   a, b: endpoints of the line segment
 *   winding: inout winding counter (incremented/decremented on +X ray crossings)
 *   min_dist_sq: inout squared Euclidean distance to closest segment so far
 */
void flux_vector_eval_segment(vec2 p, vec2 a, vec2 b, inout int winding, inout float min_dist_sq)
{
    if (a.y == b.y)
        return;

    float ymin = min(a.y, b.y);
    float ymax = max(a.y, b.y);

    // Ray casting along +X
    if (ymin <= p.y && p.y < ymax) {
        float t = (p.y - a.y) / (b.y - a.y);
        float x_int = mix(a.x, b.x, t);
        if (x_int > p.x) {
            winding += (b.y > a.y) ? 1 : -1;
        }
    }

    // Distance to line segment
    vec2 pa = p - a;
    vec2 ba = b - a;
    float denom = dot(ba, ba);
    float h = (denom > 0.0) ? clamp(dot(pa, ba) / denom, 0.0, 1.0) : 0.0;
    vec2 diff = pa - ba * h;
    float d2 = dot(diff, diff);
    if (d2 < min_dist_sq) {
        min_dist_sq = d2;
    }
}

/* Map accumulated winding and minimum distance to continuous 1x analytic coverage.
 *   winding: accumulated winding number
 *   min_dist_sq: squared distance to closest segment
 *   fill_rule: 0 = NonZero, 1 = EvenOdd
 *   pixel_scale: width of one physical pixel in the query space (1.0 for 2D pixel space)
 */
float flux_vector_eval_coverage(int winding, float min_dist_sq, uint fill_rule, float pixel_scale)
{
    bool inside = (fill_rule == 1u) ? ((winding & 1) != 0) : (winding != 0);
    float dist = sqrt(min_dist_sq);
    float signed_dist = inside ? dist : -dist;
    float scale = (pixel_scale > 1e-7) ? pixel_scale : 1e-7;
    return clamp(signed_dist / scale + 0.5, 0.0, 1.0);
}

/* Map accumulated winding and minimum distance to perspective-correct coverage
 * using screen-space partial derivatives (Jacobian).
 *   winding: accumulated winding number
 *   min_dist_sq: squared distance to closest segment in object space
 *   fill_rule: 0 = NonZero, 1 = EvenOdd
 *   obj_pos: object-space coordinates with screen-space gradients
 */
float flux_vector_eval_coverage_jacobian(int winding, float min_dist_sq, uint fill_rule, vec2 obj_pos)
{
    vec2 dpos_dx = dFdx(obj_pos);
    vec2 dpos_dy = dFdy(obj_pos);
    float pixel_scale = sqrt(max(dot(dpos_dx, dpos_dx), dot(dpos_dy, dpos_dy)));
    return flux_vector_eval_coverage(winding, min_dist_sq, fill_rule, pixel_scale);
}
