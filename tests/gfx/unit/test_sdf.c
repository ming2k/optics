/*
 * Signed distance field primitives (ADR-0118, ADR-0148, ADR-0175).
 *
 * Verifies:
 *   - exact sign and magnitude for circles, boxes, uniform & per-corner
 *     rounded rectangles, and continuous-curvature squircles;
 *   - CSS/Skia proportional radius normalization (including FLT_MAX/Inf/NaN);
 *   - Eikonal property (|grad d| == 1) and analytic derivative == central
 *     difference of the distance field for both uniform and asymmetric corners;
 *   - interior crease resolution over `aa` vs continuum limit;
 *   - differential parity with the independent circular rrect reference;
 *   - degenerate input finiteness.
 */
#include "optics_test.h"

#include <optics/gfx/sdf.h>

#include <float.h>
#include <math.h>

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
    OT_EXPECT_NEAR(optics_gfx_sdf_circle((optics_gfx_point){3.0f, 0.0f}, 3.0f), 0.0f, 1e-5);
    OT_EXPECT_NEAR(optics_gfx_sdf_circle((optics_gfx_point){0.0f, 0.0f}, 3.0f), -3.0f, 1e-5);
    OT_EXPECT_NEAR(optics_gfx_sdf_circle((optics_gfx_point){6.0f, 0.0f}, 3.0f), 3.0f, 1e-5);
    OT_EXPECT_NEAR(optics_gfx_sdf_circle((optics_gfx_point){0.0f, -3.0f}, 3.0f), 0.0f, 1e-5);
    OT_EXPECT_NEAR(optics_gfx_sdf_circle((optics_gfx_point){0.0f, 5.0f}, 3.0f), 2.0f, 1e-5);
}

static void test_corner_radii_normalize(void) {
    /* Quarter-arc [0, 0, r, 0] in an r x r box claims the full dimension. */
    optics_gfx_corner_radii q =
        optics_gfx_corner_radii_normalize(optics_gfx_corner_radii_all(0, 0, 24, 0), 24, 24);
    OT_EXPECT_NEAR(q.tl, 0.0f, 1e-6f);
    OT_EXPECT_NEAR(q.tr, 0.0f, 1e-6f);
    OT_EXPECT_NEAR(q.br, 24.0f, 1e-6f);
    OT_EXPECT_NEAR(q.bl, 0.0f, 1e-6f);

    /* Proportional CSS scaling preserves ratios when edges overflow. */
    optics_gfx_corner_radii scaled =
        optics_gfx_corner_radii_normalize(optics_gfx_corner_radii_all(100, 20, 0, 0), 100, 20);
    OT_EXPECT_NEAR(scaled.tl, 20.0f, 1e-5f);
    OT_EXPECT_NEAR(scaled.tr, 4.0f, 1e-5f);
    OT_EXPECT_NEAR(scaled.br, 0.0f, 1e-6f);
    OT_EXPECT_NEAR(scaled.bl, 0.0f, 1e-6f);

    /* Huge pill sentinel (FLT_MAX) scales cleanly without +inf overflow. */
    optics_gfx_corner_radii pill =
        optics_gfx_corner_radii_normalize(optics_gfx_corner_radii_uniform(FLT_MAX), 100.0f, 20.0f);
    OT_EXPECT_NEAR(pill.tl, 10.0f, 1e-4f);
    OT_EXPECT_NEAR(pill.tr, 10.0f, 1e-4f);
    OT_EXPECT_NEAR(pill.br, 10.0f, 1e-4f);
    OT_EXPECT_NEAR(pill.bl, 10.0f, 1e-4f);

    /* NaN, Inf, negative, and zero extents stay finite and clamp to 0. */
    optics_gfx_corner_radii bad = optics_gfx_corner_radii_normalize(
        optics_gfx_corner_radii_all(NAN, INFINITY, -5.0f, 4.0f), 20.0f, 20.0f);
    OT_EXPECT(bad.tl == 0.0f && bad.tr == 0.0f && bad.br == 0.0f && bad.bl == 4.0f);

    optics_gfx_corner_radii zero =
        optics_gfx_corner_radii_normalize(optics_gfx_corner_radii_uniform(10.0f), 0.0f, 20.0f);
    OT_EXPECT(zero.tl == 0.0f && zero.tr == 0.0f && zero.br == 0.0f && zero.bl == 0.0f);

    optics_gfx_corner_radii nan_dim =
        optics_gfx_corner_radii_normalize(optics_gfx_corner_radii_uniform(10.0f), NAN, 20.0f);
    OT_EXPECT(nan_dim.tl == 0.0f && nan_dim.tr == 0.0f && nan_dim.br == 0.0f && nan_dim.bl == 0.0f);
}

static void test_rrect_uniform(void) {
    const float hw = 10.0f, hh = 6.0f, r = 3.0f;
    const optics_gfx_corner_radii ur = optics_gfx_corner_radii_uniform(r);
    const optics_gfx_corner_radii zero = {};

    OT_EXPECT_NEAR(optics_gfx_sdf_rrect((optics_gfx_point){0.0f, 0.0f}, hw, hh, ur, 0.0f), -6.0f,
                   1e-5);
    OT_EXPECT_NEAR(optics_gfx_sdf_rrect((optics_gfx_point){hw, 0.0f}, hw, hh, ur, 0.0f), 0.0f,
                   1e-5);
    OT_EXPECT_NEAR(optics_gfx_sdf_rrect((optics_gfx_point){0.0f, hh}, hw, hh, ur, 0.0f), 0.0f,
                   1e-5);
    OT_EXPECT_NEAR(optics_gfx_sdf_rrect((optics_gfx_point){0.0f, -hh}, hw, hh, ur, 0.0f), 0.0f,
                   1e-5);
    OT_EXPECT_NEAR(optics_gfx_sdf_rrect((optics_gfx_point){14.0f, 0.0f}, hw, hh, ur, 0.0f), 4.0f,
                   1e-5);
    OT_EXPECT_NEAR(optics_gfx_sdf_rrect((optics_gfx_point){hw, hh}, hw, hh, ur, 0.0f),
                   r * (1.41421356f - 1.0f), 1e-5);
    OT_EXPECT_NEAR(optics_gfx_sdf_rrect((optics_gfx_point){hw, hh}, hw, hh, zero, 0.0f), 0.0f,
                   1e-5);
    OT_EXPECT_NEAR(optics_gfx_sdf_rrect((optics_gfx_point){0.0f, 0.0f}, hw, hh,
                                        optics_gfx_corner_radii_uniform(99.0f), 0.0f),
                   optics_gfx_sdf_rrect((optics_gfx_point){0.0f, 0.0f}, hw, hh,
                                        optics_gfx_corner_radii_uniform(hh), 0.0f),
                   1e-6);
}

static void test_rrect_per_corner(void) {
    /* Quarter-circle [0, 0, 20, 0] in a 20x20 box (hx = 10, hy = 10):
     * arc centre sits at the top-left corner (-10, -10). */
    const optics_gfx_corner_radii q = optics_gfx_corner_radii_all(0.0f, 0.0f, 20.0f, 0.0f);
    /* Crisp 90° top-left vertex is on the boundary. */
    OT_EXPECT_NEAR(optics_gfx_sdf_rrect((optics_gfx_point){-10.0f, -10.0f}, 10.0f, 10.0f, q, 0.0f),
                   0.0f, 1e-5f);
    /* Bottom-right box corner is outside the radius-20 arc by 20*(sqrt(2)-1). */
    OT_EXPECT_NEAR(optics_gfx_sdf_rrect((optics_gfx_point){10.0f, 10.0f}, 10.0f, 10.0f, q, 0.0f),
                   20.0f * (1.41421356f - 1.0f), 1e-4f);
    /* Point on the 45° arc: (-10 + 20/sqrt(2), -10 + 20/sqrt(2)) has d == 0. */
    float a45 = -10.0f + 20.0f * 0.70710678f;
    OT_EXPECT_NEAR(optics_gfx_sdf_rrect((optics_gfx_point){a45, a45}, 10.0f, 10.0f, q, 0.0f), 0.0f,
                   1e-4f);

    /* Diagonal leaf [20, 0, 20, 0] in 20x20: both opposite arcs are active at
     * the centre (0, 0), giving d = sqrt(10^2 + 10^2) - 20. */
    const optics_gfx_corner_radii leaf = optics_gfx_corner_radii_all(20.0f, 0.0f, 20.0f, 0.0f);
    OT_EXPECT_NEAR(optics_gfx_sdf_rrect((optics_gfx_point){0.0f, 0.0f}, 10.0f, 10.0f, leaf, 0.0f),
                   10.0f * 1.41421356f - 20.0f, 1e-4f);
    /* Sharp tips at (+10, -10) and (-10, +10) lie on the boundary. */
    OT_EXPECT_NEAR(
        optics_gfx_sdf_rrect((optics_gfx_point){10.0f, -10.0f}, 10.0f, 10.0f, leaf, 0.0f), 0.0f,
        1e-5f);
    OT_EXPECT_NEAR(
        optics_gfx_sdf_rrect((optics_gfx_point){-10.0f, 10.0f}, 10.0f, 10.0f, leaf, 0.0f), 0.0f,
        1e-5f);
}

static void test_ring(void) {
    OT_EXPECT_NEAR(optics_gfx_sdf_ring(0.0f, 2.0f), -2.0f, 1e-6);
    OT_EXPECT_NEAR(optics_gfx_sdf_ring(2.0f, 2.0f), 0.0f, 1e-6);
    OT_EXPECT_NEAR(optics_gfx_sdf_ring(-2.0f, 2.0f), 0.0f, 1e-6);
    OT_EXPECT_NEAR(optics_gfx_sdf_ring(5.0f, 2.0f), 3.0f, 1e-6);
    OT_EXPECT_NEAR(optics_gfx_sdf_ring(-7.0f, 0.0f), -7.0f, 1e-6);
    OT_EXPECT_NEAR(optics_gfx_sdf_ring(-7.0f, -1.0f), -7.0f, 1e-6);
}

static void test_coverage(void) {
    OT_EXPECT_NEAR(optics_gfx_sdf_coverage(0.0f, 1.0f), 0.5f, 1e-5);
    OT_EXPECT_NEAR(optics_gfx_sdf_coverage(-0.5f, 1.0f), 1.0f, 1e-5);
    OT_EXPECT_NEAR(optics_gfx_sdf_coverage(0.5f, 1.0f), 0.0f, 1e-5);
    OT_EXPECT_NEAR(optics_gfx_sdf_coverage(-100.0f, 1.0f), 1.0f, 1e-6);
    OT_EXPECT_NEAR(optics_gfx_sdf_coverage(100.0f, 1.0f), 0.0f, 1e-6);
    OT_EXPECT(optics_gfx_sdf_coverage(-0.2f, 1.0f) > optics_gfx_sdf_coverage(0.0f, 1.0f));
    OT_EXPECT(optics_gfx_sdf_coverage(0.0f, 1.0f) > optics_gfx_sdf_coverage(0.2f, 1.0f));
    OT_EXPECT(optics_gfx_sdf_coverage(0.2f, 4.0f) > optics_gfx_sdf_coverage(0.2f, 1.0f));
    float c = optics_gfx_sdf_coverage(0.0f, 0.0f);
    OT_EXPECT(isfinite(c));
    OT_EXPECT_NEAR(c, 0.5f, 1e-3);
}

static void test_eikonal(void) {
    const float hw = 10.0f, hh = 6.0f, r = 3.0f;
    const optics_gfx_corner_radii ur = optics_gfx_corner_radii_uniform(r);
    const float h = 1e-2f;
    for (int i = -40; i <= 40; ++i) {
        float x = (float)i * 0.25f;
        for (int j = -24; j <= 24; ++j) {
            float y = (float)j * 0.25f;
            if (fabsf(x) > hw - r + 1.0f || fabsf(y) > hh - r + 1.0f)
                continue;
            if (fabsf(x) < 0.1f || fabsf(y) < 0.1f)
                continue;
            float bx = fabsf(x) - hw;
            float by = fabsf(y) - hh;
            if (fabsf(bx - by) < 0.75f)
                continue;
            float dx = optics_gfx_sdf_rrect((optics_gfx_point){x + h, y}, hw, hh, ur, 0.0f) -
                       optics_gfx_sdf_rrect((optics_gfx_point){x - h, y}, hw, hh, ur, 0.0f);
            float dy = optics_gfx_sdf_rrect((optics_gfx_point){x, y + h}, hw, hh, ur, 0.0f) -
                       optics_gfx_sdf_rrect((optics_gfx_point){x, y - h}, hw, hh, ur, 0.0f);
            float grad = sqrtf((dx / (2.0f * h)) * (dx / (2.0f * h)) +
                               (dy / (2.0f * h)) * (dy / (2.0f * h)));
            OT_EXPECT_NEAR(grad, 1.0f, 5e-3f);
        }
    }
}

static void test_shader_parity(void) {
    const float cx = 12.5f, cy = -7.25f, hw = 9.0f, hh = 5.5f, r = 2.0f;
    const optics_gfx_corner_radii ur = optics_gfx_corner_radii_uniform(r);
    int checked = 0;
    for (int i = -60; i <= 60; ++i) {
        for (int j = -60; j <= 60; ++j) {
            float px = cx + (float)i * 0.2f;
            float py = cy + (float)j * 0.2f;
            float hw_arg = (i % 7 == 0) ? 0.0f : 1.5f;
            float mine = optics_gfx_sdf_ring(
                optics_gfx_sdf_rrect((optics_gfx_point){px - cx, py - cy}, hw, hh, ur, 0.0f),
                hw_arg);
            float ref = shader_reference(px, py, cx, cy, hw, hh, r, hw_arg);
            OT_EXPECT_NEAR(mine, ref, 1e-5);
            float cov_mine = optics_gfx_sdf_coverage(mine, 1.0f);
            float cov_ref = 0.5f - ref;
            cov_ref = cov_ref < 0.0f ? 0.0f : (cov_ref > 1.0f ? 1.0f : cov_ref);
            OT_EXPECT_NEAR(cov_mine, cov_ref, 1e-5);
            checked++;
        }
    }
    OT_EXPECT(checked > 14000);
}

static void test_degenerate(void) {
    OT_EXPECT(isfinite(optics_gfx_sdf_rrect((optics_gfx_point){0.0f, 0.0f}, 0.0f, 0.0f,
                                            (optics_gfx_corner_radii){}, 1.0f)));
    OT_EXPECT(isfinite(optics_gfx_sdf_rrect((optics_gfx_point){1.0f, 1.0f}, -5.0f, -5.0f,
                                            optics_gfx_corner_radii_uniform(2.0f), 1.0f)));
    OT_EXPECT(isfinite(optics_gfx_sdf_rrect((optics_gfx_point){1.0f, 1.0f}, 4.0f, 4.0f,
                                            optics_gfx_corner_radii_uniform(-3.0f), 1.0f)));
    OT_EXPECT(isfinite(optics_gfx_sdf_rrect((optics_gfx_point){1.0f, 1.0f}, 4.0f, 4.0f,
                                            optics_gfx_corner_radii_all(NAN, INFINITY, FLT_MAX, 2),
                                            1.0f)));
    OT_EXPECT(isfinite(optics_gfx_sdf_circle((optics_gfx_point){0.0f, 0.0f}, 0.0f)));
}

static void test_squircle_curvature(void) {
    const float hw = 8.0f, hh = 5.0f, r = 2.0f;
    const optics_gfx_corner_radii ur = optics_gfx_corner_radii_uniform(r);

    optics_gfx_point corner = {hw - r + r * 0.95f, hh - r + r * 0.95f};
    float d0 = optics_gfx_sdf_rrect(corner, hw, hh, ur, 0.0f);
    float d1 = optics_gfx_sdf_rrect(corner, hw, hh, ur, 1.0f);
    OT_EXPECT(d0 > 0.0f);
    OT_EXPECT(d1 < d0);
    float prev = d0;
    for (int k = 1; k <= 10; ++k) {
        float d = optics_gfx_sdf_rrect(corner, hw, hh, ur, (float)k / 10.0f);
        OT_EXPECT(d <= prev + 1e-6f);
        prev = d;
    }

    optics_gfx_point edge = {hw, 0.0f};
    for (int k = 0; k <= 4; ++k)
        OT_EXPECT_NEAR(optics_gfx_sdf_rrect(edge, hw, hh, ur, (float)k / 4.0f), 0.0f, 1e-4);
}

/* ------------------------------------------------------------------ */
/*  Derivatives (ADR-0148, ADR-0175)                                  */
/* ------------------------------------------------------------------ */

static void test_gradient_is_the_derivative(void) {
    /* Optimal float32 central-difference step (~cbrt(FLT_EPSILON)) avoids
     * catastrophic cancellation when |d| ~ 10 while keeping O(h^2) tiny. */
    const float h = 5e-3f;
    const struct {
        float hw, hh;
        optics_gfx_corner_radii radii;
    } shapes[] = {
        {9.0f, 5.5f, {2.0f, 2.0f, 2.0f, 2.0f}},
        {10.0f, 10.0f, {0.0f, 0.0f, 20.0f, 0.0f}},
        {12.0f, 6.0f, {0.0f, 0.0f, 12.0f, 12.0f}},
        {10.0f, 7.0f, {2.0f, 5.0f, 8.0f, 0.0f}},
    };
    const float curve[4] = {0.0f, 0.35f, 0.7f, 1.0f};
    int checked = 0;

    for (size_t si = 0; si < sizeof(shapes) / sizeof(shapes[0]); ++si) {
        float hw = shapes[si].hw, hh = shapes[si].hh;
        optics_gfx_corner_radii radii = shapes[si].radii;
        for (int c = 0; c < 4; ++c) {
            for (int i = -40; i <= 40; ++i) {
                for (int j = -40; j <= 40; ++j) {
                    float px = (float)i * 0.35f, py = (float)j * 0.35f;
                    optics_gfx_point p = {px, py};
                    float bx = fabsf(px) - hw;
                    float by = fabsf(py) - hh;
                    /* Skip non-differentiable creases: axes, box boundary kinks,
                     * and interior medial axis. */
                    if (fabsf(px) < 0.1f || fabsf(py) < 0.1f)
                        continue;
                    if (bx <= 0.0f && by <= 0.0f && fabsf(bx - by) < 1.0f)
                        continue;
                    if (fabsf(bx) < 0.2f || fabsf(by) < 0.2f)
                        continue;

                    optics_gfx_sdf_sample g =
                        optics_gfx_sdf_rrect_gradient(p, hw, hh, radii, curve[c]);
                    /* Check gradient smoothness over a 4h stencil to skip the
                     * zero-measure seam where max(d_box, d_arc) switches terms. */
                    optics_gfx_sdf_sample gp = optics_gfx_sdf_rrect_gradient(
                        (optics_gfx_point){px + 2.0f * h, py + 2.0f * h}, hw, hh, radii, curve[c]);
                    optics_gfx_sdf_sample gm = optics_gfx_sdf_rrect_gradient(
                        (optics_gfx_point){px - 2.0f * h, py - 2.0f * h}, hw, hh, radii, curve[c]);
                    if (fabsf(gp.normal.x - gm.normal.x) > 0.005f ||
                        fabsf(gp.normal.y - gm.normal.y) > 0.005f)
                        continue;

                    float dx = optics_gfx_sdf_rrect((optics_gfx_point){px + h, py}, hw, hh, radii,
                                                    curve[c]) -
                               optics_gfx_sdf_rrect((optics_gfx_point){px - h, py}, hw, hh, radii,
                                                    curve[c]);
                    float dy = optics_gfx_sdf_rrect((optics_gfx_point){px, py + h}, hw, hh, radii,
                                                    curve[c]) -
                               optics_gfx_sdf_rrect((optics_gfx_point){px, py - h}, hw, hh, radii,
                                                    curve[c]);
                    float len = sqrtf(dx * dx + dy * dy);
                    if (len < 1e-4f)
                        continue;
                    OT_EXPECT_NEAR(g.normal.x, dx / len, 3e-3f);
                    OT_EXPECT_NEAR(g.normal.y, dy / len, 3e-3f);
                    OT_EXPECT_NEAR(sqrtf(g.normal.x * g.normal.x + g.normal.y * g.normal.y), 1.0f,
                                   1e-5f);
                    OT_EXPECT_NEAR(g.distance, optics_gfx_sdf_rrect(p, hw, hh, radii, curve[c]),
                                   1e-6f);
                    checked++;
                }
            }
        }
    }
    OT_EXPECT(checked > 20000);
}

static void test_normal_is_analytic_not_sampled(void) {
    const float hw = 12.0f, hh = 7.0f;
    const optics_gfx_corner_radii ur = optics_gfx_corner_radii_uniform(3.0f);
    const optics_gfx_corner_radii zero = {};
    const float aa_values[3] = {0.25f, 1.0f, 4.0f};
    int compared = 0;

    for (int c = 0; c <= 10; c += 5) {
        float curvature = (float)c / 10.0f;
        for (int i = -50; i <= 50; ++i) {
            for (int j = -50; j <= 50; ++j) {
                float px = (float)i * 0.25f, py = (float)j * 0.25f;
                optics_gfx_point p = {px, py};
                /* Outside the box interior (bx > 0 or by > 0), `aa` is never
                 * consulted, so the shading normal is strictly independent of aa. */
                if (fabsf(px) <= hw && fabsf(py) <= hh)
                    continue;
                optics_gfx_sdf_sample base =
                    optics_gfx_sdf_rrect_sample(p, hw, hh, ur, curvature, 1.0f);
                for (int k = 0; k < 3; ++k) {
                    optics_gfx_sdf_sample s =
                        optics_gfx_sdf_rrect_sample(p, hw, hh, ur, curvature, aa_values[k]);
                    OT_EXPECT(s.normal.x == base.normal.x);
                    OT_EXPECT(s.normal.y == base.normal.y);
                    OT_EXPECT(s.distance == base.distance);
                    compared++;
                }
            }
        }
    }
    OT_EXPECT(compared > 20000);

    optics_gfx_point inside = {6.0f, 2.0f}; /* bx = -6, by = -5: |bx - by| == 1 */
    optics_gfx_sdf_sample tight = optics_gfx_sdf_rrect_sample(inside, hw, hh, zero, 0.0f, 0.05f);
    optics_gfx_sdf_sample wide = optics_gfx_sdf_rrect_sample(inside, hw, hh, zero, 0.0f, 4.0f);
    OT_EXPECT(tight.normal.x != wide.normal.x);
    optics_gfx_sdf_sample limit = optics_gfx_sdf_rrect_gradient(inside, hw, hh, zero, 0.0f);
    OT_EXPECT_NEAR(tight.normal.x, limit.normal.x, 1e-2f);
    OT_EXPECT_NEAR(tight.normal.y, limit.normal.y, 1e-2f);
    OT_EXPECT_NEAR(limit.normal.y, 1.0f, 1e-6f);
    OT_EXPECT(wide.normal.x > 0.2f && wide.normal.x < 0.7f);
    OT_EXPECT(wide.normal.y > 0.7f && wide.normal.y < 0.98f);
}

static void test_crease_is_resolved_over_the_footprint(void) {
    const float hw = 10.0f, hh = 10.0f, aa = 1.0f;
    const optics_gfx_corner_radii zero = {};
    float worst_step = 0.0f;
    optics_gfx_sdf_sample prev =
        optics_gfx_sdf_rrect_sample((optics_gfx_point){2.8f, 2.6f}, hw, hh, zero, 0.0f, aa);
    for (int i = 1; i <= 400; ++i) {
        float t = (float)i * 0.001f;
        optics_gfx_sdf_sample s = optics_gfx_sdf_rrect_sample(
            (optics_gfx_point){2.8f + t, 2.6f - t}, hw, hh, zero, 0.0f, aa);
        float step = sqrtf((s.normal.x - prev.normal.x) * (s.normal.x - prev.normal.x) +
                           (s.normal.y - prev.normal.y) * (s.normal.y - prev.normal.y));
        if (step > worst_step)
            worst_step = step;
        prev = s;
    }
    OT_EXPECT(worst_step < 0.02f);

    optics_gfx_point a = {-0.05f, 0.05f};
    optics_gfx_point b = {0.05f, -0.05f};
    optics_gfx_sdf_sample ga = optics_gfx_sdf_rrect_gradient(a, hw, hh, zero, 0.0f);
    optics_gfx_sdf_sample gb = optics_gfx_sdf_rrect_gradient(b, hw, hh, zero, 0.0f);
    float flip = sqrtf((ga.normal.x - gb.normal.x) * (ga.normal.x - gb.normal.x) +
                       (ga.normal.y - gb.normal.y) * (ga.normal.y - gb.normal.y));
    OT_EXPECT(flip > 1.0f);
}

static void test_gradient_degenerate(void) {
    const optics_gfx_corner_radii zero = {};
    const optics_gfx_point pts[4] = {{0.0f, 0.0f}, {4.0f, 4.0f}, {-4.0f, 4.0f}, {1e6f, 1e6f}};
    for (int i = 0; i < 4; ++i) {
        optics_gfx_sdf_sample s = optics_gfx_sdf_rrect_sample(pts[i], 0.0f, 0.0f, zero, 1.0f, 1.0f);
        OT_EXPECT(isfinite(s.distance));
        OT_EXPECT(isfinite(s.normal.x));
        OT_EXPECT(isfinite(s.normal.y));
        OT_EXPECT_NEAR(sqrtf(s.normal.x * s.normal.x + s.normal.y * s.normal.y), 1.0f, 1e-5f);

        optics_gfx_sdf_sample g = optics_gfx_sdf_rrect_gradient(pts[i], 0.0f, 0.0f, zero, 1.0f);
        OT_EXPECT(isfinite(g.normal.x));
        OT_EXPECT(isfinite(g.normal.y));
        OT_EXPECT_NEAR(sqrtf(g.normal.x * g.normal.x + g.normal.y * g.normal.y), 1.0f, 1e-5f);
    }
    optics_gfx_sdf_sample s =
        optics_gfx_sdf_rrect_sample((optics_gfx_point){1.0f, 1.0f}, -5.0f, -5.0f,
                                    optics_gfx_corner_radii_uniform(99.0f), 1.0f, 1.0f);
    OT_EXPECT(isfinite(s.normal.x));
    OT_EXPECT(isfinite(s.distance));
}

static void test_l4_normalisation_is_bounded(void) {
    const double lo_expected = 0.8408964152537145; /* 2^-0.25 */
    double observed_lo = 1e9, observed_hi = -1e9;
    for (int i = 0; i <= 200000; ++i) {
        double u = (double)i / 200000.0;
        double nx = pow(u, 0.25);
        double ny = pow(1.0 - u, 0.25);
        OT_EXPECT_NEAR(nx * nx * nx * nx + ny * ny * ny * ny, 1.0, 1e-12);
        double nx2 = nx * nx, ny2 = ny * ny;
        double g = sqrt(nx2 * nx2 * nx2 + ny2 * ny2 * ny2);
        if (g < observed_lo)
            observed_lo = g;
        if (g > observed_hi)
            observed_hi = g;
    }
    OT_EXPECT_NEAR(observed_lo, lo_expected, 1e-9);
    OT_EXPECT_NEAR(observed_hi, 1.0, 1e-12);
    OT_EXPECT(observed_lo < 0.85);
    OT_EXPECT(observed_hi > 0.999999);
}

int main(void) {
    test_circle();
    test_corner_radii_normalize();
    test_rrect_uniform();
    test_rrect_per_corner();
    test_ring();
    test_coverage();
    test_squircle_curvature();
    test_eikonal();
    test_shader_parity();
    test_degenerate();
    test_gradient_is_the_derivative();
    test_normal_is_analytic_not_sampled();
    test_crease_is_resolved_over_the_footprint();
    test_gradient_degenerate();
    test_l4_normalisation_is_bounded();
    return OT_REPORT();
}
