/*
 * Signed distance fields for the analytic geometry classes (ADR-0118, ADR-0175),
 * and their derivatives (ADR-0148).
 *
 * Pure math, no device and no allocation — the peer of src/curve/curve.c.
 * A rounded rectangle / continuous-curvature squircle with independent
 * per-corner radii is evaluated as the exact intersection of the axis-aligned
 * box and each active corner's L2/L4 blended arc:
 *
 *   d(p) = max(d_box(p, he), max_{i: r_i > 0, q_i > 0} d_arc(q_i, r_i, c))
 *
 * Both the field and its analytic gradient fall out of one evaluation of this
 * expression, matching <optics/gfx/shaders/optics_gfx_sdf.slang> identically
 * (INV-SDF-02). No pow()/powf() appears here: every exponent is a small
 * integer or 1/4, evaluated via multiplies and nested square roots.
 */

#include <optics/gfx/sdf.h>
#include <stdint.h>

#include <math.h>

/* GLSL/Slang sign(): 0.0 at zero. Spelled out because C's copysignf() is a
 * different function, and the host/device parity assertions compare the
 * resulting direction rather than merely its magnitude. */
static float sdf_signf(float x) {
    return (float)((x > 0.0f) - (x < 0.0f));
}

static float sdf_blend(float curvature) {
    if (!(curvature > 0.0f))
        return 0.0f;
    return curvature > 1.0f ? 1.0f : curvature;
}

typedef enum sdf_crease : uint32_t {
    SDF_CREASE_CONTINUUM, /* hard pick of the dominant edge: the exact limit */
    SDF_CREASE_SHADING,   /* the same, ramped over `aa` across the crease */
} sdf_crease;

/* Evaluate one corner arc in its local positive quadrant (qx > 0, qy > 0),
 * returning its blended L2/L4 distance and (when requested) gradient with
 * respect to (qx, qy). */
static float sdf_corner_arc_eval(float qx, float qy, float r, float c, bool with_normal,
                                 float *out_gx, float *out_gy) {
    float l2_sq = qx * qx + qy * qy;
    float l2 = sqrtf(l2_sq);
    float d_l2 = l2 - r;
    float g_l2x = 0.0f, g_l2y = 0.0f;
    if (with_normal && l2 > 1e-6f) {
        float inv = 1.0f / l2;
        g_l2x = qx * inv;
        g_l2y = qy * inv;
    }

    if (!(c > 0.0f)) {
        if (with_normal) {
            *out_gx = g_l2x;
            *out_gy = g_l2y;
        }
        return d_l2;
    }

    float x2 = qx * qx, y2 = qy * qy;
    float q4 = sqrtf(sqrtf(x2 * x2 + y2 * y2));
    if (!(q4 > 1e-4f)) {
        if (with_normal) {
            *out_gx = g_l2x;
            *out_gy = g_l2y;
        }
        return d_l2;
    }

    /* L4-normalised direction: with nx^4 + ny^4 == 1, g = sqrt(nx^6 + ny^6)
     * lies in [2^(-1/4), 1], keeping intermediate powers bounded. */
    float nx = qx / q4, ny = qy / q4;
    float nx2 = nx * nx, ny2 = ny * ny;
    float nx4 = nx2 * nx2, ny4 = ny2 * ny2;
    float g = sqrtf(nx4 * nx2 + ny4 * ny2);
    float d_l4 = (q4 - r) / g;

    if (with_normal) {
        float nx3 = nx2 * nx, ny3 = ny2 * ny;
        float inv_g = 1.0f / g;
        float kq = (q4 - r) * inv_g * inv_g * 3.0f * inv_g / q4;
        float ax = nx3 * nx2 * (1.0f - nx4);
        float ay = ny3 * ny2 * (1.0f - ny4);
        float bx = nx3 * ny4 * ny2;
        float by = ny3 * nx4 * nx2;
        float g_l4x = nx3 * inv_g - kq * (ax - bx);
        float g_l4y = ny3 * inv_g - kq * (ay - by);
        *out_gx = g_l2x + (g_l4x - g_l2x) * c;
        *out_gy = g_l2y + (g_l4y - g_l2y) * c;
    }
    return d_l2 + (d_l4 - d_l2) * c;
}

/* Single shared body for optics_gfx_sdf_rrect, _gradient, and _sample
 * (INV-SDF-02). */
static optics_gfx_sdf_sample sdf_rrect_eval(optics_gfx_point p, float half_w, float half_h,
                                            optics_gfx_corner_radii radii, float curvature,
                                            float aa, bool with_normal, sdf_crease crease) {
    float hx = (isfinite(half_w) && half_w > 0.0f) ? half_w : 0.0f;
    float hy = (isfinite(half_h) && half_h > 0.0f) ? half_h : 0.0f;
    optics_gfx_corner_radii cr = optics_gfx_corner_radii_normalize(radii, 2.0f * hx, 2.0f * hy);
    float c = sdf_blend(curvature);

    /* Exact axis-aligned box distance and gradient. */
    float bx = fabsf(p.x) - hx;
    float by = fabsf(p.y) - hy;
    float bpx = bx > 0.0f ? bx : 0.0f;
    float bpy = by > 0.0f ? by : 0.0f;
    float b_len = sqrtf(bpx * bpx + bpy * bpy);
    float d = b_len + fminf(fmaxf(bx, by), 0.0f);

    const float aa_eff = aa > 1e-4f ? aa : 1e-4f;
    float gx = 0.0f, gy = 0.0f;
    if (with_normal) {
        if (bx <= 0.0f && by <= 0.0f) {
            if (crease == SDF_CREASE_CONTINUUM) {
                gx = bx > by ? sdf_signf(p.x) : 0.0f;
                gy = bx > by ? 0.0f : sdf_signf(p.y);
            } else {
                float h = 0.5f + (bx - by) / (2.0f * aa_eff);
                h = h < 0.0f ? 0.0f : (h > 1.0f ? 1.0f : h);
                gx = sdf_signf(p.x) * h;
                gy = sdf_signf(p.y) * (1.0f - h);
            }
        } else if (b_len > 1e-6f) {
            float inv = 1.0f / b_len;
            gx = bpx * inv * sdf_signf(p.x);
            gy = bpy * inv * sdf_signf(p.y);
        }
    }

    const float corner_r[4] = {cr.tl, cr.tr, cr.br, cr.bl};
    const float corner_sx[4] = {-1.0f, 1.0f, 1.0f, -1.0f};
    const float corner_sy[4] = {-1.0f, -1.0f, 1.0f, 1.0f};

    for (int i = 0; i < 4; ++i) {
        float r = corner_r[i];
        if (!(r > 0.0f))
            continue;
        float sx = corner_sx[i];
        float sy = corner_sy[i];
        float qx = sx * p.x - (hx - r);
        float qy = sy * p.y - (hy - r);
        if (qx > 0.0f && qy > 0.0f) {
            float agx = 0.0f, agy = 0.0f;
            float da = sdf_corner_arc_eval(qx, qy, r, c, with_normal, &agx, &agy);
            if (with_normal) {
                float sgx = agx * sx;
                float sgy = agy * sy;
                if (crease == SDF_CREASE_SHADING && d < 0.0f && da < 0.0f &&
                    fabsf(da - d) < aa_eff) {
                    float h = 0.5f + (da - d) / (2.0f * aa_eff);
                    h = h < 0.0f ? 0.0f : (h > 1.0f ? 1.0f : h);
                    gx = gx + (sgx - gx) * h;
                    gy = gy + (sgy - gy) * h;
                } else if (da > d) {
                    gx = sgx;
                    gy = sgy;
                }
            }
            if (da > d)
                d = da;
        }
    }

    optics_gfx_sdf_sample out = {.distance = d, .normal = {.x = 0.0f, .y = 1.0f}};
    if (with_normal) {
        float len = sqrtf(gx * gx + gy * gy);
        if (len > 1e-12f) {
            out.normal.x = gx / len;
            out.normal.y = gy / len;
        }
    }
    return out;
}

float optics_gfx_sdf_circle(optics_gfx_point p, float radius) {
    return sqrtf(p.x * p.x + p.y * p.y) - radius;
}

float optics_gfx_sdf_ring(float d, float half) {
    if (!(half > 0.0f))
        return d;
    return fabsf(d) - half;
}

float optics_gfx_sdf_rrect(optics_gfx_point p, float half_w, float half_h,
                           optics_gfx_corner_radii radii, float curvature) {
    return sdf_rrect_eval(p, half_w, half_h, radii, curvature, 1.0f, false, SDF_CREASE_CONTINUUM)
        .distance;
}

optics_gfx_sdf_sample optics_gfx_sdf_rrect_gradient(optics_gfx_point p, float half_w, float half_h,
                                                    optics_gfx_corner_radii radii,
                                                    float curvature) {
    return sdf_rrect_eval(p, half_w, half_h, radii, curvature, 1.0f, true, SDF_CREASE_CONTINUUM);
}

optics_gfx_sdf_sample optics_gfx_sdf_rrect_sample(optics_gfx_point p, float half_w, float half_h,
                                                  optics_gfx_corner_radii radii, float curvature,
                                                  float aa) {
    return sdf_rrect_eval(p, half_w, half_h, radii, curvature, aa, true, SDF_CREASE_SHADING);
}

float optics_gfx_sdf_coverage(float d, float aa) {
    if (!(aa > 1e-4f))
        aa = 1e-4f;
    float cov = 0.5f - d / aa;
    if (cov < 0.0f)
        return 0.0f;
    if (cov > 1.0f)
        return 1.0f;
    return cov;
}

float optics_gfx_sdf_smooth_union(float a, float b, float radius) {
    if (!(radius > 0.001f))
        return a < b ? a : b;
    float h = 0.5f + 0.5f * (b - a) / radius;
    h = h < 0.0f ? 0.0f : (h > 1.0f ? 1.0f : h);
    return (b + (a - b) * h) - radius * h * (1.0f - h);
}

optics_gfx_point optics_gfx_sdf_smooth_union_gradient(optics_gfx_point grad_a,
                                                      optics_gfx_point grad_b, float a, float b,
                                                      float radius) {
    if (!(radius > 0.001f))
        return a < b ? grad_a : grad_b;
    float h = 0.5f + 0.5f * (b - a) / radius;
    h = h < 0.0f ? 0.0f : (h > 1.0f ? 1.0f : h);
    float gx = grad_b.x + (grad_a.x - grad_b.x) * h;
    float gy = grad_b.y + (grad_a.y - grad_b.y) * h;
    float len = sqrtf(gx * gx + gy * gy);
    if (len > 1e-12f) {
        gx /= len;
        gy /= len;
    } else {
        gx = grad_a.x;
        gy = grad_a.y;
    }
    return (optics_gfx_point){.x = gx, .y = gy};
}
