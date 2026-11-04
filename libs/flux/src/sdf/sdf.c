/*
 * Signed distance fields for the analytic geometry classes (ADR-0118), and
 * their derivatives (ADR-0148).
 *
 * Pure math, no device and no allocation — the peer of src/curve/curve.c.
 * The rounded-rectangle family is a corner decomposition: `q` is the signed
 * corner coordinate, `inner` the interior term, and the field is "distance to
 * the corner arc, plus the interior term". Two corner metrics are blended, L2
 * (circular arc) and L4 (G2 superellipse), and both the field and its gradient
 * fall out of one evaluation of that decomposition — so a consumer's normal is
 * provably the derivative of the expression that produced its distance, rather
 * than a difference of neighbouring samples.
 *
 * This is the same expression the device shaders evaluate (flux_sdf.glsl), so
 * host and device coverage agree by construction rather than by tolerance;
 * test_sdf pins that parity, including the gradients.
 *
 * No pow()/powf() appears here. Every exponent is a small integer or 1/4, so it
 * is a multiply or a nested square root. That is not a micro-optimisation: the
 * previous formulation spent six transcendentals per field evaluation on paths
 * that run per rim pixel of every glass body.
 */

#include <flux/sdf.h>

#include <math.h>

/* GLSL's sign(): 0.0 at zero. Spelled out because C's copysignf() is a
 * different function, and the host/device parity assertions compare the
 * resulting direction rather than merely its magnitude. */
static float sdf_signf(float x) {
    return (float)((x > 0.0f) - (x < 0.0f));
}

/* Corner decomposition. `with_l4` mirrors the shader's compile-time branch: a
 * caller that wants a plain rounded rectangle never pays for the arc. */
typedef struct sdf_corners {
    float qx, qy;
    float inner;
    float d_l2, g_l2x, g_l2y;
    float d_l4, g_l4x, g_l4y;
} sdf_corners;

static sdf_corners sdf_corners_of(flux_point p, float half_w, float half_h, float radius,
                                  bool with_l4) {
    if (!(half_w > 0.0f))
        half_w = 0.0f;
    if (!(half_h > 0.0f))
        half_h = 0.0f;
    if (!(radius > 0.0f))
        radius = 0.0f;
    float lim = half_w < half_h ? half_w : half_h;
    if (radius > lim)
        radius = lim;

    sdf_corners k = {};
    k.qx = fabsf(p.x) - (half_w - radius);
    k.qy = fabsf(p.y) - (half_h - radius);
    float mx = k.qx > 0.0f ? k.qx : 0.0f;
    float my = k.qy > 0.0f ? k.qy : 0.0f;
    float inner = k.qx > k.qy ? k.qx : k.qy;
    if (inner > 0.0f)
        inner = 0.0f;
    k.inner = inner;

    /* Circular corner. The degenerate branch is the medial axis of a box, where
     * the true gradient does not exist and the caller's crease rule decides. */
    float l2_sq = mx * mx + my * my;
    k.d_l2 = sqrtf(l2_sq) + inner - radius;
    if (l2_sq > 1e-12f) {
        float inv = 1.0f / sqrtf(l2_sq);
        k.g_l2x = mx * inv;
        k.g_l2y = my * inv;
    }

    /* The q4 == 0 limit: the L4 field equals the L2 field there, and its
     * gradient is the caller's to decide (both q <= 0 means "interior, use the
     * crease rule"). */
    k.d_l4 = inner - radius;
    if (!with_l4)
        return k;

    float x2 = mx * mx, y2 = my * my;
    float q4_4 = x2 * x2 + y2 * y2; /* x^4 + y^4 */
    float q4 = sqrtf(sqrtf(q4_4));  /* ^(1/4) */
    if (!(q4 > 1e-4f))
        return k;

    /* L4-normalised direction. q4 is 1-homogeneous, so its gradient depends on
     * this direction alone, and the normalisation factors are bounded: with
     * nx^4 + ny^4 == 1, g = sqrt(nx^6 + ny^6) lies in [2^(-1/4), 1]. That is
     * what makes the expression safe in half precision — the un-normalised form
     * overflowed fp16 at roughly 24 px of half extent. */
    float nx = mx / q4, ny = my / q4;
    float nx2 = nx * nx, ny2 = ny * ny;
    float nx3 = nx2 * nx, ny3 = ny2 * ny;
    float nx4 = nx2 * nx2, ny4 = ny2 * ny2;
    float g = sqrtf(nx4 * nx2 + ny4 * ny2);
    k.d_l4 = (q4 - radius) / g + inner;

    /* d q4/dp == n^3. With dn/dp = (I - n n^3)/q4, the chain rule through
     * sqrt(sum n^6) gives 3 n^3 (n^2 - g^2)/(g q4), and the quotient rule on
     * (q4 - r)/g collects into the two grouped terms below. */
    float inv_g = 1.0f / g;
    float kq = (q4 - radius) * inv_g * inv_g * 3.0f * inv_g / q4;
    float ax = nx3 * nx2 * (1.0f - nx4);
    float ay = ny3 * ny2 * (1.0f - ny4);
    float bx = nx3 * ny4 * ny2;
    float by = ny3 * nx4 * nx2;
    k.g_l4x = nx3 * inv_g - kq * (ax - bx);
    k.g_l4y = ny3 * inv_g - kq * (ay - by);
    return k;
}

static float sdf_blend(float curvature) {
    if (!(curvature > 0.0f))
        return 0.0f;
    return curvature > 1.0f ? 1.0f : curvature;
}

/* How a normal is resolved inside the interior, where the field degenerates to
 * max(q.x, q.y) - r and the true gradient is discontinuous across the medial
 * axis. Two answers are legitimate there and only one is shadable, so the
 * choice is a typed parameter on the single body rather than a second copy of
 * the body (INV-SDF-02: one evaluation, so host and device cannot drift). */
typedef enum sdf_crease {
    SDF_CREASE_CONTINUUM, /* hard pick of the dominant edge: the exact limit */
    SDF_CREASE_SHADING,   /* the same, ramped over `aa` across the crease */
} sdf_crease;

/* The one body every squircle entry point sits on: field, and optionally the
 * unit normal under the requested crease rule. */
static flux_sdf_sample sdf_squircle_eval(flux_point p, float half_w, float half_h, float radius,
                                         float curvature, float aa, bool with_normal,
                                         sdf_crease crease) {
    float c = sdf_blend(curvature);
    sdf_corners k = sdf_corners_of(p, half_w, half_h, radius, c > 0.0f);

    flux_sdf_sample out = {.distance = k.d_l2 + (k.d_l4 - k.d_l2) * c,
                           .normal = {.x = 0.0f, .y = 1.0f}};
    if (!with_normal)
        return out;

    float gx, gy;
    if (k.qx <= 0.0f && k.qy <= 0.0f) {
        /* Interior (both corner coordinates non-positive — exactly where the
         * field is a max() of two planes). `<=` and not `<`: at q.x == 0 with
         * q.y < 0 the exterior branch has q_pos == 0 and therefore no
         * gradient at all, so the boundary pixel belongs here. */
        if (crease == SDF_CREASE_CONTINUUM) {
            /* max(q.x, q.y): a hard pick of the dominant edge. */
            gx = k.qx > k.qy ? sdf_signf(p.x) : 0.0f;
            gy = k.qx > k.qy ? 0.0f : sdf_signf(p.y);
        } else {
            /* h == 1 is "the x edge dominates", linear in between: the
             * weighted average of the two edge directions, which is what a
             * central difference of span 2*aa reports. This is the only place
             * the shading normal is not the exact analytic gradient. */
            float h = 0.5f + (k.qx - k.qy) / (2.0f * (aa > 1e-4f ? aa : 1e-4f));
            h = h < 0.0f ? 0.0f : (h > 1.0f ? 1.0f : h);
            gx = sdf_signf(p.x) * h;
            gy = sdf_signf(p.y) * (1.0f - h);
        }
    } else {
        gx = (k.g_l2x + (k.g_l4x - k.g_l2x) * c) * sdf_signf(p.x);
        gy = (k.g_l2y + (k.g_l4y - k.g_l2y) * c) * sdf_signf(p.y);
    }
    float len = sqrtf(gx * gx + gy * gy);
    if (len > 1e-12f) {
        out.normal.x = gx / len;
        out.normal.y = gy / len;
    }
    return out;
}

float flux_sdf_circle(flux_point p, float radius) {
    return sqrtf(p.x * p.x + p.y * p.y) - radius;
}

float flux_sdf_round_rect(flux_point p, float half_w, float half_h, float radius) {
    return sdf_corners_of(p, half_w, half_h, radius, false).d_l2;
}

float flux_sdf_ring(float d, float half) {
    if (!(half > 0.0f))
        return d;
    return fabsf(d) - half;
}

float flux_sdf_squircle(flux_point p, float half_w, float half_h, float radius, float curvature) {
    return sdf_squircle_eval(p, half_w, half_h, radius, curvature, 1.0f, false,
                             SDF_CREASE_CONTINUUM)
        .distance;
}

flux_sdf_sample flux_sdf_squircle_sample(flux_point p, float half_w, float half_h, float radius,
                                         float curvature, float aa) {
    return sdf_squircle_eval(p, half_w, half_h, radius, curvature, aa, true, SDF_CREASE_SHADING);
}

flux_sdf_sample flux_sdf_squircle_gradient(flux_point p, float half_w, float half_h, float radius,
                                           float curvature) {
    return sdf_squircle_eval(p, half_w, half_h, radius, curvature, 1.0f, true,
                             SDF_CREASE_CONTINUUM);
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

float flux_sdf_smooth_union(float a, float b, float radius) {
    if (!(radius > 0.001f))
        return a < b ? a : b;
    float h = 0.5f + 0.5f * (b - a) / radius;
    h = h < 0.0f ? 0.0f : (h > 1.0f ? 1.0f : h);
    return (b + (a - b) * h) - radius * h * (1.0f - h);
}

flux_point flux_sdf_smooth_union_gradient(flux_point grad_a, flux_point grad_b, float a, float b,
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
    return (flux_point){.x = gx, .y = gy};
}
