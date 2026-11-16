/*
 * flux_vector_eval.slang — the single Slang definition for analytic fragment
 * vector evaluation (ADR-0116, ADR-0117, ADR-0121).
 *
 * <flux/curve.h> and <flux/canvas_tile.h> provide host-side line segment
 * representations; tests/flux/integration/test_scene_vector.c pins the 3D
 * evaluation and tests/flux/unit/test_canvas_coverage.c pins the 2D evaluation.
 */

#ifndef FLUX_VECTOR_EVAL_SLANG
#define FLUX_VECTOR_EVAL_SLANG

/* Accumulate winding crossing and Euclidean boundary distance for a 2D line segment.
 *   p: query point (in object space or 2D pixel space)
 *   a, b: endpoints of the line segment
 *   winding: inout winding counter (incremented/decremented on +X ray crossings)
 *   min_dist_sq: inout squared Euclidean distance to closest segment so far
 */
void flux_vector_eval_segment(float2 p, float2 a, float2 b, inout int winding, inout float min_dist_sq)
{
    if (a.y == b.y)
        return;

    float ymin = min(a.y, b.y);
    float ymax = max(a.y, b.y);

    // Ray casting along +X
    if (ymin <= p.y && p.y < ymax) {
        float t = (p.y - a.y) / (b.y - a.y);
        float x_int = lerp(a.x, b.x, t);
        if (x_int > p.x) {
            winding += (b.y > a.y) ? 1 : -1;
        }
    }

    // Distance to line segment
    float2 pa = p - a;
    float2 ba = b - a;
    float denom = dot(ba, ba);
    float h = (denom > 0.0f) ? clamp(dot(pa, ba) / denom, 0.0f, 1.0f) : 0.0f;
    float2 diff = pa - ba * h;
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
float flux_vector_eval_coverage(int winding, float min_dist_sq, uint32_t fill_rule, float pixel_scale)
{
    bool inside = (fill_rule == 1u) ? ((winding & 1) != 0) : (winding != 0);
    float dist = sqrt(min_dist_sq);
    float signed_dist = inside ? dist : -dist;
    float scale = (pixel_scale > 1e-7f) ? pixel_scale : 1e-7f;
    return clamp(signed_dist / scale + 0.5f, 0.0f, 1.0f);
}

/* Map accumulated winding and minimum distance to perspective-correct coverage
 * using screen-space partial derivatives (Jacobian).
 *   winding: accumulated winding number
 *   min_dist_sq: squared distance to closest segment in object space
 *   fill_rule: 0 = NonZero, 1 = EvenOdd
 *   obj_pos: object-space coordinates with screen-space gradients
 */
float flux_vector_eval_coverage_jacobian(int winding, float min_dist_sq, uint32_t fill_rule, float2 obj_pos)
{
    float2 dpos_dx = ddx(obj_pos);
    float2 dpos_dy = ddy(obj_pos);
    float pixel_scale = sqrt(max(dot(dpos_dx, dpos_dx), dot(dpos_dy, dpos_dy)));
    return flux_vector_eval_coverage(winding, min_dist_sq, fill_rule, pixel_scale);
}

/* ------------------------------------------------------------------ */
/*  Exact area coverage (2D device space)                              */
/* ------------------------------------------------------------------ */
/* Coverage of a pixel is the integral of the winding number over the pixel's
 * area. With winding counted by crossings to the +X side, one segment's share
 * for the pixel [px, px+1) x [py, py+1) is
 *
 *     dy * mean( clamp(x(y) - px, 0, 1) )
 *
 * over the part of the segment inside the pixel's row: `dy` is the signed
 * height of that part and the mean is the fraction of the pixel's width lying
 * to the left of the segment. Summed over a closed contour this is the signed
 * covered area — exact for straight segments, with no notion of "nearest
 * edge". Consequences the distance model does not have:
 *   - horizontal segments contribute nothing, correctly (dy == 0);
 *   - an edge shared by two shapes of the same orientation cancels exactly,
 *     so a triangle mesh fills without seams;
 *   - a feature thinner than a pixel gets its true area, not a clamped ramp.
 */

/* Antiderivative of clamp(t, 0, 1). */
float flux_vector_clamp01_integral(float u)
{
    return (u <= 0.0f) ? 0.0f : ((u >= 1.0f) ? u - 0.5f : 0.5f * u * u);
}

/* Add segment a->b's signed area share for the pixel whose top-left corner is
 * `pixel` (integer device coordinates). */
void flux_vector_area_segment(float2 pixel, float2 a, float2 b, inout float area)
{
    float y0 = a.y - pixel.y;
    float y1 = b.y - pixel.y;
    if (y0 == y1)
        return;
    // Clip to the pixel's row, y in [0, 1].
    float t0 = clamp((0.0f - y0) / (y1 - y0), 0.0f, 1.0f);
    float t1 = clamp((1.0f - y0) / (y1 - y0), 0.0f, 1.0f);
    // Height of the part inside the row, signed by the segment's direction
    // of travel: downward (+y) counts positive, upward negative.
    float height = (t1 - t0) * (y1 - y0); // y(t1) - y(t0), never negative
    if (height <= 0.0f)
        return;
    float dy = (y1 > y0) ? height : -height;
    float x0 = a.x - pixel.x;
    float x1 = b.x - pixel.x;
    float u0 = lerp(x0, x1, t0);
    float u1 = lerp(x0, x1, t1);
    // Fraction of the pixel's width left of the segment, averaged over the
    // part in this row. The two saturated cases are decided before any
    // subtraction: far from the pixel `u` is large, and differencing the
    // antiderivative there would cancel catastrophically in single precision.
    float lo = min(u0, u1);
    float hi = max(u0, u1);
    float mean;
    if (lo >= 1.0f) {
        mean = 1.0f;
    } else if (hi <= 0.0f) {
        mean = 0.0f;
    } else if (hi - lo < 1e-4f) {
        mean = clamp(0.5f * (lo + hi), 0.0f, 1.0f);
    } else {
        mean = (flux_vector_clamp01_integral(hi) - flux_vector_clamp01_integral(lo)) / (hi - lo);
    }
    area += dy * mean;
}

/* Map the accumulated signed area to coverage under a fill rule
 * (0 = NonZero, 1 = EvenOdd). */
float flux_vector_area_coverage(float area, uint32_t fill_rule)
{
    float c = abs(area);
    if (fill_rule == 1u) {
        c = c - 2.0f * floor(c * 0.5f);
        if (c > 1.0f)
            c = 2.0f - c;
    }
    return clamp(c, 0.0f, 1.0f);
}

#endif /* FLUX_VECTOR_EVAL_SLANG */
