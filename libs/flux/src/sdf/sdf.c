/*
 * Signed distance fields for the analytic geometry classes (ADR-0118).
 *
 * Pure math, no device and no allocation — the peer of src/curve/curve.c.
 * The rounded-rectangle field is the classic rounded-box formulation
 *   q = |p| - (half - r);  d = min(max(q.x,q.y), 0) + |max(q,0)| - r
 * which is exact outside the shape and exact along the straight edges. It is
 * the same expression the canvas SDF fragment shader evaluates
 * (canvas_sdf.frag), so the host and device coverage agree by construction
 * rather than by tolerance; test_sdf pins that parity.
 */

#include <flux/sdf.h>

#include <math.h>

float flux_sdf_circle(flux_point p, float radius) {
    return sqrtf(p.x * p.x + p.y * p.y) - radius;
}

float flux_sdf_round_rect(flux_point p, float half_w, float half_h, float radius) {
    if (!(half_w > 0.0f))
        half_w = 0.0f;
    if (!(half_h > 0.0f))
        half_h = 0.0f;
    if (!(radius > 0.0f))
        radius = 0.0f;
    float lim = half_w < half_h ? half_w : half_h;
    if (radius > lim)
        radius = lim;

    float qx = fabsf(p.x) - (half_w - radius);
    float qy = fabsf(p.y) - (half_h - radius);
    float mx = qx > 0.0f ? qx : 0.0f;
    float my = qy > 0.0f ? qy : 0.0f;
    float inner = qx > qy ? qx : qy;
    if (inner > 0.0f)
        inner = 0.0f;
    return inner + sqrtf(mx * mx + my * my) - radius;
}

float flux_sdf_ring(float d, float half) {
    if (!(half > 0.0f))
        return d;
    return fabsf(d) - half;
}

float flux_sdf_squircle(flux_point p, float half_w, float half_h, float radius, float curvature) {
    if (!(half_w > 0.0f))
        half_w = 0.0f;
    if (!(half_h > 0.0f))
        half_h = 0.0f;
    if (!(radius > 0.0f))
        radius = 0.0f;
    float lim = half_w < half_h ? half_w : half_h;
    if (radius > lim)
        radius = lim;

    float qx = fabsf(p.x) - (half_w - radius);
    float qy = fabsf(p.y) - (half_h - radius);
    float mx = qx > 0.0f ? qx : 0.0f;
    float my = qy > 0.0f ? qy : 0.0f;
    float inner = qx > qy ? qx : qy;
    if (inner > 0.0f)
        inner = 0.0f;

    /* Circular-corner estimate: identical to flux_sdf_round_rect, so
     * curvature == 0 reproduces that field exactly. */
    float d_l2 = sqrtf(mx * mx + my * my) + inner - radius;

    /* L4 superellipse corner with a gradient-magnitude normalisation, which
     * makes the estimate Eikonal across the corner arc. */
    float q4 = powf(powf(mx, 4.0f) + powf(my, 4.0f), 0.25f);
    float grad = 1.0f;
    if (q4 > 1e-4f)
        grad = sqrtf(powf(mx, 6.0f) + powf(my, 6.0f)) / powf(q4, 3.0f);
    if (grad < 0.5f)
        grad = 0.5f;
    float d_g2 = (q4 - radius) / grad + inner;

    float c = curvature;
    if (!(c > 0.0f))
        return d_l2;
    if (c > 1.0f)
        c = 1.0f;
    return d_l2 + (d_g2 - d_l2) * c;
}

float flux_sdf_coverage(float d, float aa) {
    if (!(aa > 1e-4f))
        aa = 1e-4f;
    float cov = 0.5f - d / aa;
    if (cov < 0.0f)
        return 0.0f;
    if (cov > 1.0f)
        return 1.0f;
    return cov;
}
