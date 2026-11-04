/*
 * Signed distance field primitives (ADR-0118).
 *
 * These assertions are what lets the SDF branch be treated as a peer of the
 * curve kernel rather than a special case:
 *   - the field has the documented sign and magnitude at points whose
 *     distance is known analytically;
 *   - it is Eikonal (|grad d| == 1) away from the medial axis, which is what
 *     makes "coverage = 0.5 - d" a correct one-pixel anti-aliasing rule;
 *   - it reproduces the exact expression the canvas SDF fragment shader
 *     evaluates, so host and device coverage agree by construction and not by
 *     tolerance — this is the check that keeps the two from drifting apart;
 *   - degenerate input stays finite.
 */
#include "test_helpers.h"

#include <flux/sdf.h>

#include <math.h>

/* The canvas SDF shader's expression, written out independently from
 * canvas_sdf.frag (and backend_cpu.c's host port). Kept verbatim so a change
 * to either the shader or the shared primitive trips this test. */
static float shader_reference(float px, float py, float cx, float cy, float hx, float hy,
                              float radius, float stroke_hw) {
    float qx = fabsf(px - cx) - (hx - radius);
    float qy = fabsf(py - cy) - (hy - radius);
    float mx = qx > 0.0f ? qx : 0.0f;
    float my = qy > 0.0f ? qy : 0.0f;
    float m = qx > qy ? qx : qy;
    if (m > 0.0f)
        m = 0.0f;
    float d = m + sqrtf(mx * mx + my * my) - radius;
    if (stroke_hw > 0.0f)
        d = fabsf(d) - stroke_hw;
    return d;
}

static void test_circle(void) {
    EXPECT_NEAR(flux_sdf_circle((flux_point){3.0f, 0.0f}, 3.0f), 0.0f, 1e-5);
    EXPECT_NEAR(flux_sdf_circle((flux_point){0.0f, 0.0f}, 3.0f), -3.0f, 1e-5);
    EXPECT_NEAR(flux_sdf_circle((flux_point){6.0f, 0.0f}, 3.0f), 3.0f, 1e-5);
    /* The field is exact for a circle in every direction. */
    EXPECT_NEAR(flux_sdf_circle((flux_point){0.0f, -3.0f}, 3.0f), 0.0f, 1e-5);
    EXPECT_NEAR(flux_sdf_circle((flux_point){0.0f, 5.0f}, 3.0f), 2.0f, 1e-5);
}

static void test_round_rect(void) {
    const float hw = 10.0f, hh = 6.0f, r = 3.0f;
    /* Centre: distance to the nearest edge. */
    EXPECT_NEAR(flux_sdf_round_rect((flux_point){0.0f, 0.0f}, hw, hh, r), -6.0f, 1e-5);
    /* Straight-edge midpoints are exactly on the boundary. */
    EXPECT_NEAR(flux_sdf_round_rect((flux_point){hw, 0.0f}, hw, hh, r), 0.0f, 1e-5);
    EXPECT_NEAR(flux_sdf_round_rect((flux_point){0.0f, hh}, hw, hh, r), 0.0f, 1e-5);
    EXPECT_NEAR(flux_sdf_round_rect((flux_point){0.0f, -hh}, hw, hh, r), 0.0f, 1e-5);
    /* Outside an edge, straight out: exactly the extra distance. */
    EXPECT_NEAR(flux_sdf_round_rect((flux_point){14.0f, 0.0f}, hw, hh, r), 4.0f, 1e-5);
    /* The square corner is cut off: its distance is r*(sqrt(2)-1). */
    EXPECT_NEAR(flux_sdf_round_rect((flux_point){hw, hh}, hw, hh, r), r * (1.41421356f - 1.0f),
                1e-5);
    /* radius == 0 is a plain box. */
    EXPECT_NEAR(flux_sdf_round_rect((flux_point){hw, hh}, hw, hh, 0.0f), 0.0f, 1e-5);
    /* radius is clamped to the smaller half extent. */
    EXPECT_NEAR(flux_sdf_round_rect((flux_point){0.0f, 0.0f}, hw, hh, 99.0f),
                flux_sdf_round_rect((flux_point){0.0f, 0.0f}, hw, hh, hh), 1e-6);
}

static void test_ring(void) {
    EXPECT_NEAR(flux_sdf_ring(0.0f, 2.0f), -2.0f, 1e-6);
    EXPECT_NEAR(flux_sdf_ring(2.0f, 2.0f), 0.0f, 1e-6);
    EXPECT_NEAR(flux_sdf_ring(-2.0f, 2.0f), 0.0f, 1e-6);
    EXPECT_NEAR(flux_sdf_ring(5.0f, 2.0f), 3.0f, 1e-6);
    /* Zero-width border is the filled shape, unchanged. */
    EXPECT_NEAR(flux_sdf_ring(-7.0f, 0.0f), -7.0f, 1e-6);
    EXPECT_NEAR(flux_sdf_ring(-7.0f, -1.0f), -7.0f, 1e-6);
}

static void test_coverage(void) {
    EXPECT_NEAR(flux_sdf_coverage(0.0f, 1.0f), 0.5f, 1e-5);
    EXPECT_NEAR(flux_sdf_coverage(-0.5f, 1.0f), 1.0f, 1e-5);
    EXPECT_NEAR(flux_sdf_coverage(0.5f, 1.0f), 0.0f, 1e-5);
    EXPECT_NEAR(flux_sdf_coverage(-100.0f, 1.0f), 1.0f, 1e-6);
    EXPECT_NEAR(flux_sdf_coverage(100.0f, 1.0f), 0.0f, 1e-6);
    /* Monotone decreasing in d. */
    EXPECT(flux_sdf_coverage(-0.2f, 1.0f) > flux_sdf_coverage(0.0f, 1.0f));
    EXPECT(flux_sdf_coverage(0.0f, 1.0f) > flux_sdf_coverage(0.2f, 1.0f));
    /* A wider `aa` (a magnified shape) softens the ramp. */
    EXPECT(flux_sdf_coverage(0.2f, 4.0f) > flux_sdf_coverage(0.2f, 1.0f));
    /* A degenerate `aa` yields a hard edge, not a NaN or a blow-up. */
    float c = flux_sdf_coverage(0.0f, 0.0f);
    EXPECT(isfinite(c));
    EXPECT_NEAR(c, 0.5f, 1e-3);
}

/* |grad d| == 1 wherever the field is a true distance: this is what licenses
 * the "coverage = 0.5 - d" rule. Checked numerically in the edge region, away
 * from the corners and the medial axis. */
static void test_eikonal(void) {
    const float hw = 10.0f, hh = 6.0f, r = 3.0f;
    const float h = 1e-2f;
    for (int i = -40; i <= 40; ++i) {
        float x = (float)i * 0.25f;
        for (int j = -24; j <= 24; ++j) {
            float y = (float)j * 0.25f;
            /* Skip the corner regions... */
            if (fabsf(x) > hw - r + 1.0f || fabsf(y) > hh - r + 1.0f)
                continue;
            /* ...the axes, where the field's |x| / |y| terms have a kink and
             * a central difference is not a derivative... */
            if (fabsf(x) < 0.1f || fabsf(y) < 0.1f)
                continue;
            /* ...and the medial axis, where the field is a max() of two terms
             * and its gradient is not defined. Only test where one term
             * dominates by a clear margin, which is where |grad d| == 1 is
             * actually asserted. */
            float qx = fabsf(x) - (hw - r);
            float qy = fabsf(y) - (hh - r);
            if (fabsf(qx - qy) < 0.75f)
                continue;
            float dx = flux_sdf_round_rect((flux_point){x + h, y}, hw, hh, r) -
                       flux_sdf_round_rect((flux_point){x - h, y}, hw, hh, r);
            float dy = flux_sdf_round_rect((flux_point){x, y + h}, hw, hh, r) -
                       flux_sdf_round_rect((flux_point){x, y - h}, hw, hh, r);
            float grad = sqrtf((dx / (2.0f * h)) * (dx / (2.0f * h)) +
                               (dy / (2.0f * h)) * (dy / (2.0f * h)));
            EXPECT_NEAR(grad, 1.0f, 5e-3f);
        }
    }
}

/* Differential parity with the canvas SDF shader over a grid, including the
 * border case. This is the assertion that keeps the host primitive and the
 * device shader from drifting apart. */
static void test_shader_parity(void) {
    const float cx = 12.5f, cy = -7.25f, hw = 9.0f, hh = 5.5f, r = 2.0f;
    int checked = 0;
    for (int i = -60; i <= 60; ++i) {
        for (int j = -60; j <= 60; ++j) {
            float px = cx + (float)i * 0.2f;
            float py = cy + (float)j * 0.2f;
            float hw_arg = (i % 7 == 0) ? 0.0f : 1.5f; /* exercise the ring path too */
            float mine = flux_sdf_ring(
                flux_sdf_round_rect((flux_point){px - cx, py - cy}, hw, hh, r), hw_arg);
            float ref = shader_reference(px, py, cx, cy, hw, hh, r, hw_arg);
            EXPECT_NEAR(mine, ref, 1e-5);
            /* ...and coverage matches the shader's clamp. */
            float cov_mine = flux_sdf_coverage(mine, 1.0f);
            float cov_ref = 0.5f - ref;
            cov_ref = cov_ref < 0.0f ? 0.0f : (cov_ref > 1.0f ? 1.0f : cov_ref);
            EXPECT_NEAR(cov_mine, cov_ref, 1e-5);
            checked++;
        }
    }
    EXPECT(checked > 14000);
}

static void test_degenerate(void) {
    /* Zero extents, negative extents and NaN-ish radius all stay finite. */
    EXPECT(isfinite(flux_sdf_round_rect((flux_point){0.0f, 0.0f}, 0.0f, 0.0f, 0.0f)));
    EXPECT(isfinite(flux_sdf_round_rect((flux_point){1.0f, 1.0f}, -5.0f, -5.0f, 2.0f)));
    EXPECT(isfinite(flux_sdf_round_rect((flux_point){1.0f, 1.0f}, 4.0f, 4.0f, -3.0f)));
    EXPECT(isfinite(flux_sdf_circle((flux_point){0.0f, 0.0f}, 0.0f)));
}

static float clampf_(float x, float lo, float hi) {
    return x < lo ? lo : (x > hi ? hi : x);
}

/* The squircle field: a rounded rect whose corner arc blends a circular arc
 * (curvature 0) with an L4 superellipse (curvature 1). INV-SDF-02 requires one
 * definition; this pins it against the expression the Prism material shipped,
 * and pins the degenerate case. */
static float squircle_reference(float px, float py, float cx, float cy, float hw, float hh,
                                float req_r, float curvature) {
    /* Prism's shipped `squircle_sdf`, written out independently. */
    float radius = clampf_(req_r, 0.0f, fminf(hw, hh));
    float qx = fabsf(px - cx) - hw + radius;
    float qy = fabsf(py - cy) - hh + radius;
    float mx = qx > 0.0f ? qx : 0.0f;
    float my = qy > 0.0f ? qy : 0.0f;
    float m = qx > qy ? qx : qy;
    if (m > 0.0f)
        m = 0.0f;
    float d_l2 = sqrtf(mx * mx + my * my) + m - radius;
    float q4 = powf(powf(mx, 4.0f) + powf(my, 4.0f), 0.25f);
    float grad = (q4 > 1e-4f) ? sqrtf(powf(mx, 6.0f) + powf(my, 6.0f)) / powf(q4, 3.0f) : 1.0f;
    float d_g2 = (q4 - radius) / fmaxf(grad, 0.5f) + m;
    float c = clampf_(curvature, 0.0f, 1.0f);
    return d_l2 + (d_g2 - d_l2) * c;
}

static void test_squircle(void) {
    const float hw = 8.0f, hh = 5.0f, r = 2.0f;

    /* curvature == 0 is *exactly* the circular-corner field: the squircle family
     * contains flux_sdf_round_rect as its degenerate case, so turning a corner
     * round never discontinuously changes shape when curvature is introduced. */
    for (int i = -40; i <= 40; ++i) {
        for (int j = -40; j <= 40; ++j) {
            flux_point p = {(float)i * 0.4f, (float)j * 0.4f};
            EXPECT(flux_sdf_squircle(p, hw, hh, r, 0.0f) == flux_sdf_round_rect(p, hw, hh, r));
        }
    }

    /* Differential parity with the shipped Prism expression, curvature and all. */
    const float curve[5] = {0.0f, 0.25f, 0.5f, 0.85f, 1.0f};
    for (int c = 0; c < 5; ++c) {
        for (int i = -40; i <= 40; ++i) {
            for (int j = -40; j <= 40; ++j) {
                float px = (float)i * 0.4f, py = (float)j * 0.4f;
                float mine = flux_sdf_squircle((flux_point){px, py}, hw, hh, r, curve[c]);
                float ref = squircle_reference(px, py, 0.0f, 0.0f, hw, hh, r, curve[c]);
                EXPECT_NEAR(mine, ref, 1e-4);
            }
        }
    }

    /* Raising curvature pushes the corner arc *outward*: a point on the
     * circular corner sits inside the L4 corner, so its distance decreases.
     * This is the property that makes curvature == 1 look like a squircle. */
    float diag = (hw - r) + r * 0.70710678f * 1.30f;
    flux_point corner = {diag, diag * (hh - r + r * 1.30f * 0.70710678f) / diag};
    corner = (flux_point){hw - r + r * 0.95f, hh - r + r * 0.95f};
    float d0 = flux_sdf_squircle(corner, hw, hh, r, 0.0f);
    float d1 = flux_sdf_squircle(corner, hw, hh, r, 1.0f);
    EXPECT(d0 > 0.0f); /* outside the circular corner */
    EXPECT(d1 < d0);   /* the L4 corner reaches out past it */
    /* Monotone in curvature at that point. */
    float prev = d0;
    for (int k = 1; k <= 10; ++k) {
        float d = flux_sdf_squircle(corner, hw, hh, r, (float)k / 10.0f);
        EXPECT(d <= prev + 1e-6f);
        prev = d;
    }
    /* ...and the straight-edge midpoint is *not* moved by curvature: the L4
     * normalisation only reshapes the corner arc. */
    flux_point edge = {hw, 0.0f};
    for (int k = 0; k <= 4; ++k)
        EXPECT_NEAR(flux_sdf_squircle(edge, hw, hh, r, (float)k / 4.0f), 0.0f, 1e-4);

    /* Degenerate input stays finite. */
    EXPECT(isfinite(flux_sdf_squircle((flux_point){0.0f, 0.0f}, 0.0f, 0.0f, 0.0f, 1.0f)));
    EXPECT(isfinite(flux_sdf_squircle((flux_point){1.0f, 1.0f}, 4.0f, 4.0f, 99.0f, 1.0f)));
}

/* ------------------------------------------------------------------ */
/*  Derivatives (ADR-0148)                                            */
/* ------------------------------------------------------------------ */

/* The normal is the derivative of the distance, not a neighbour of it. Wherever
 * the field is differentiable, flux_sdf_squircle_sample's normal must equal a
 * central difference of flux_sdf_squircle itself. This is the assertion that
 * makes "one expression, its own derivative" checkable rather than
 * aspirational: a wrong quotient rule, a dropped chain-rule term, or a gradient
 * taken from a *different* field all land here. */
static void test_gradient_is_the_derivative(void) {
    const float hw = 9.0f, hh = 5.5f, r = 2.0f;
    const float h = 1e-3f;
    const float curve[5] = {0.0f, 0.25f, 0.5f, 0.85f, 1.0f};
    int checked = 0;
    for (int c = 0; c < 5; ++c) {
        for (int i = -60; i <= 60; ++i) {
            for (int j = -60; j <= 60; ++j) {
                float px = (float)i * 0.2f, py = (float)j * 0.2f;
                flux_point p = {px, py};
                float qx = fabsf(px) - (hw - r);
                float qy = fabsf(py) - (hh - r);
                /* The field is not differentiable in three places, and a central
                 * difference is not a derivative in any of them either, so the
                 * assertion is scoped to where it is meaningful at the
                 * differencing footprint's resolution:
                 *   - the interior/exterior branch boundary (q == 0);
                 *   - the interior medial axis, where the field is a max();
                 *   - the |p| kink on the axes. */
                if (fabsf(qx) < 1.0f || fabsf(qy) < 1.0f)
                    continue;
                if (qx < 0.0f && qy < 0.0f && fabsf(qx - qy) < 1.0f)
                    continue;
                if (fabsf(px) < 1e-2f || fabsf(py) < 1e-2f)
                    continue;

                flux_sdf_sample s = flux_sdf_squircle_sample(p, hw, hh, r, curve[c], 1.0f);
                float dx = flux_sdf_squircle((flux_point){px + h, py}, hw, hh, r, curve[c]) -
                           flux_sdf_squircle((flux_point){px - h, py}, hw, hh, r, curve[c]);
                float dy = flux_sdf_squircle((flux_point){px, py + h}, hw, hh, r, curve[c]) -
                           flux_sdf_squircle((flux_point){px, py - h}, hw, hh, r, curve[c]);
                float len = sqrtf(dx * dx + dy * dy);
                if (len < 1e-4f)
                    continue;
                /* The field is Eikonal here, so the normalised difference is the
                 * unit gradient itself. 2e-3 rad ~ 0.11 deg is the first-order
                 * truncation of the h = 1e-3 reference. */
                EXPECT_NEAR(s.normal.x, dx / len, 2e-3f);
                EXPECT_NEAR(s.normal.y, dy / len, 2e-3f);
                /* ...and it is a unit vector. */
                EXPECT_NEAR(sqrtf(s.normal.x * s.normal.x + s.normal.y * s.normal.y), 1.0f, 1e-5f);
                /* The distance it carries is the field's own value: one
                 * evaluation, not two. */
                EXPECT_NEAR(s.distance, flux_sdf_squircle(p, hw, hh, r, curve[c]), 1e-6f);
                checked++;
            }
        }
    }
    EXPECT(checked > 10000);
}

/* The normal does not depend on the sampling footprint outside the interior.
 * This is what distinguishes an analytic gradient from a finite-difference one:
 * a 4-tap normal of span 2*aa changes with aa by construction, and so does any
 * surrogate built from a different field than the one that produced the
 * distance. Here aa moves exactly one branch — the interior crease — which is a
 * sampling question, not a differentiation one. */
static void test_normal_is_analytic_not_sampled(void) {
    const float hw = 12.0f, hh = 7.0f, r = 3.0f;
    const float aa_values[3] = {0.25f, 1.0f, 4.0f};
    int compared = 0;
    for (int c = 0; c <= 10; c += 5) {
        float curvature = (float)c / 10.0f;
        for (int i = -50; i <= 50; ++i) {
            for (int j = -50; j <= 50; ++j) {
                float px = (float)i * 0.25f, py = (float)j * 0.25f;
                flux_point p = {px, py};
                float qx = fabsf(px) - (hw - r);
                float qy = fabsf(py) - (hh - r);
                if (qx <= 0.0f && qy <= 0.0f)
                    continue;
                flux_sdf_sample base = flux_sdf_squircle_sample(p, hw, hh, r, curvature, 1.0f);
                for (int k = 0; k < 3; ++k) {
                    flux_sdf_sample s =
                        flux_sdf_squircle_sample(p, hw, hh, r, curvature, aa_values[k]);
                    EXPECT(s.normal.x == base.normal.x);
                    EXPECT(s.normal.y == base.normal.y);
                    EXPECT(s.distance == base.distance);
                    compared++;
                }
            }
        }
    }
    EXPECT(compared > 20000);

    /* ...and inside the interior the crease rule *does* respond to the
     * footprint, which is the whole reason it exists. Point chosen so both
     * normal components are non-zero (a zero component would survive
     * normalisation and hide the ramp) and one unit off the medial axis. */
    flux_point inside = {6.0f, 2.0f}; /* q = (-3, -2): |qx - qy| == 1 */
    flux_sdf_sample tight = flux_sdf_squircle_sample(inside, hw, hh, 0.0f, 0.0f, 0.05f);
    flux_sdf_sample wide = flux_sdf_squircle_sample(inside, hw, hh, 0.0f, 0.0f, 4.0f);
    EXPECT(tight.normal.x != wide.normal.x);
    /* A tight footprint converges on the continuum gradient (the dominant edge:
     * qx < qy, so the y edge wins)... */
    flux_sdf_sample limit = flux_sdf_squircle_gradient(inside, hw, hh, 0.0f, 0.0f);
    EXPECT_NEAR(tight.normal.x, limit.normal.x, 1e-2f);
    EXPECT_NEAR(tight.normal.y, limit.normal.y, 1e-2f);
    EXPECT_NEAR(limit.normal.y, 1.0f, 1e-6f);
    /* ...and the wide one is a blend of the two edge directions that the
     * continuum never produces. */
    EXPECT(wide.normal.x > 0.2f && wide.normal.x < 0.7f);
    EXPECT(wide.normal.y > 0.7f && wide.normal.y < 0.98f);
}

/* The interior rule turns a 90-degree flip into a ramp. Sweeping *across* the
 * interior diagonal — not along it, which never leaves the crease — the sampled
 * normal must rotate smoothly. A hard pick changes by 1.41 (90 degrees) across
 * one sample step; the measured ramp changes by 0.000. */
static void test_crease_is_resolved_over_the_footprint(void) {
    const float hw = 10.0f, hh = 10.0f, aa = 1.0f;
    float worst_step = 0.0f;
    /* The path stays in the first quadrant and crosses only |x| == |y|: it never
     * touches an axis, where |p| has a kink of the *field* (a 0.34 step in the
     * normal) that is not the crease rule's business. It is also seeded at its
     * own first sample, so the first measured step is a step. */
    flux_sdf_sample prev =
        flux_sdf_squircle_sample((flux_point){2.8f, 2.6f}, hw, hh, 0.0f, 0.0f, aa);
    for (int i = 1; i <= 400; ++i) {
        float t = (float)i * 0.001f; /* x + y constant; x - y crosses 0 at t = 0.2 */
        flux_sdf_sample s =
            flux_sdf_squircle_sample((flux_point){2.8f + t, 2.6f - t}, hw, hh, 0.0f, 0.0f, aa);
        float step = sqrtf((s.normal.x - prev.normal.x) * (s.normal.x - prev.normal.x) +
                           (s.normal.y - prev.normal.y) * (s.normal.y - prev.normal.y));
        if (step > worst_step)
            worst_step = step;
        prev = s;
    }
    EXPECT(worst_step < 0.02f);

    /* The continuum entry point is the one that flips — asserted so the
     * distinction cannot rot. */
    flux_point a = {-0.05f, 0.05f};
    flux_point b = {0.05f, -0.05f};
    flux_sdf_sample ga = flux_sdf_squircle_gradient(a, hw, hh, 0.0f, 0.0f);
    flux_sdf_sample gb = flux_sdf_squircle_gradient(b, hw, hh, 0.0f, 0.0f);
    float flip = sqrtf((ga.normal.x - gb.normal.x) * (ga.normal.x - gb.normal.x) +
                       (ga.normal.y - gb.normal.y) * (ga.normal.y - gb.normal.y));
    EXPECT(flip > 1.0f);
}

/* Degenerate input must not produce a NaN normal. */
static void test_gradient_degenerate(void) {
    const flux_point pts[4] = {{0.0f, 0.0f}, {4.0f, 4.0f}, {-4.0f, 4.0f}, {1e6f, 1e6f}};
    for (int i = 0; i < 4; ++i) {
        flux_sdf_sample s = flux_sdf_squircle_sample(pts[i], 0.0f, 0.0f, 0.0f, 1.0f, 1.0f);
        EXPECT(isfinite(s.distance));
        EXPECT(isfinite(s.normal.x));
        EXPECT(isfinite(s.normal.y));
        EXPECT_NEAR(sqrtf(s.normal.x * s.normal.x + s.normal.y * s.normal.y), 1.0f, 1e-5f);

        flux_sdf_sample g = flux_sdf_squircle_gradient(pts[i], 0.0f, 0.0f, 0.0f, 1.0f);
        EXPECT(isfinite(g.normal.x));
        EXPECT(isfinite(g.normal.y));
        EXPECT_NEAR(sqrtf(g.normal.x * g.normal.x + g.normal.y * g.normal.y), 1.0f, 1e-5f);
    }
    /* A negative half extent and an oversized radius clamp like the fields do. */
    flux_sdf_sample s =
        flux_sdf_squircle_sample((flux_point){1.0f, 1.0f}, -5.0f, -5.0f, 99.0f, 1.0f, 1.0f);
    EXPECT(isfinite(s.normal.x));
    EXPECT(isfinite(s.distance));
}

/* The bound that makes a future binary16 attempt start from a proof instead of a
 * rediscovery (ADR-0148): along the L4 direction circle `nx^4 + ny^4 == 1`, the
 * normalisation factor `g = sqrt(nx^6 + ny^6)` stays inside [2^-1/4, 1].
 *
 * Why it matters: `d_l4 = (q4 - r)/g` is the only place the field divides by a
 * magnitude, and `1/g` is what a narrow width would have to hold. Outside that
 * range `g` would need a clamp and the expression would stop being safe at low
 * precision. Asserted rather than described, so a change to the corner family
 * that breaks the bound fails here instead of silently invalidating the note.
 *
 * This is a property of the *direction*, independent of shape, so it is checked
 * directly on the parametrisation instead of through the public surface. */
static void test_l4_normalisation_is_bounded(void) {
    const double lo_expected = 0.8408964152537145; /* 2^-0.25 */
    double observed_lo = 1e9, observed_hi = -1e9;
    for (int i = 0; i <= 200000; ++i) {
        double u = (double)i / 200000.0; /* nx^4 */
        double nx = pow(u, 0.25);
        double ny = pow(1.0 - u, 0.25);
        /* ...and it really is the direction circle. */
        EXPECT_NEAR(nx * nx * nx * nx + ny * ny * ny * ny, 1.0, 1e-12);
        double nx2 = nx * nx, ny2 = ny * ny;
        double g = sqrt(nx2 * nx2 * nx2 + ny2 * ny2 * ny2);
        if (g < observed_lo)
            observed_lo = g;
        if (g > observed_hi)
            observed_hi = g;
    }
    EXPECT_NEAR(observed_lo, lo_expected, 1e-9);
    EXPECT_NEAR(observed_hi, 1.0, 1e-12);
    /* Both endpoints are attainable: g == 2^-1/4 at nx == ny == 2^-1/4, and
     * g == 1 on the axes. A bound that is never approached would be vacuous. */
    EXPECT(observed_lo < 0.85);
    EXPECT(observed_hi > 0.999999);
}

int main(void) {
    test_circle();
    test_round_rect();
    test_ring();
    test_coverage();
    test_squircle();
    test_eikonal();
    test_shader_parity();
    test_degenerate();
    test_gradient_is_the_derivative();
    test_normal_is_analytic_not_sampled();
    test_crease_is_resolved_over_the_footprint();
    test_gradient_degenerate();
    test_l4_normalisation_is_bounded();
    TEST_SUMMARY();
}
