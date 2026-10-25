/*
 * Analytic curve kernel (ADR-0115) — contract tests.
 *
 * These are the falsifiable claims the ADR makes, not a smoke test:
 *   - the reduction error bound holds against a dense sampling of the
 *     original cubic (INV-CRV-06);
 *   - inflections and cusps are located analytically and the reduction
 *     splits at them;
 *   - Wang's formula is monotone in tolerance and handles the straight case;
 *   - offset is exact for a line and exact at the on-curve midpoint of a
 *     quadratic;
 *   - winding agrees with both fill rules, including a doubled-winding ring
 *     where NONZERO and EVENODD must differ;
 *   - every failure is explicit and writes nothing (INV-CRV-04);
 *   - the kernel is deterministic byte-for-byte (INV-CRV-05).
 */
#include "test_helpers.h"

#include <flux/curve.h>
#include <flux/sdf.h>

#include <string.h>

static flux_point P(float x, float y) {
    return (flux_point){x, y};
}

static float dist2(flux_point a, flux_point b) {
    float dx = a.x - b.x, dy = a.y - b.y;
    return dx * dx + dy * dy;
}

/* Minimum squared distance from `p` to a dense sampling of the reduced run.
 * Dense enough that the discretisation error is far below the tolerance the
 * caller asserts against. */
static float cover_dist2(const flux_curve_seg *segs, uint32_t n, flux_point p) {
    float best = 1e30f;
    for (uint32_t i = 0; i < n; ++i) {
        for (int k = 0; k <= 400; ++k) {
            float t = (float)k / 400.0f;
            float d = dist2(flux_curve_quad_eval(segs[i], t), p);
            if (d < best)
                best = d;
        }
    }
    return best;
}

static void test_reduction_error_bound(void) {
    /* S-curve: one inflection at t = 1/2 (see test_inflections). */
    flux_point p0 = P(0, 0), p1 = P(3, 0), p2 = P(0, 3), p3 = P(3, 3);
    const float tol = 0.25f;
    flux_curve_seg segs[64];
    uint32_t n = 0;
    EXPECT(flux_curve_cubic_reduce(p0, p1, p2, p3, tol, segs, 64, &n) == FLUX_OK);
    EXPECT(n >= 2); /* split at the inflection: a quadratic cannot turn */

    /* Every sample of the cubic must be within tol of the reduced run. */
    float worst = 0.0f;
    for (int k = 0; k <= 1000; ++k) {
        float t = (float)k / 1000.0f;
        flux_point q = flux_curve_cubic_eval(p0, p1, p2, p3, t);
        float d2 = cover_dist2(segs, n, q);
        if (d2 > worst)
            worst = d2;
    }
    /* 1e-4 slack absorbs the discrete sampling of the reduced run. */
    EXPECT(worst <= (tol + 1e-4f) * (tol + 1e-4f));

    /* Endpoints are reproduced exactly. */
    EXPECT_NEAR(segs[0].p0.x, p0.x, 1e-5);
    EXPECT_NEAR(segs[0].p0.y, p0.y, 1e-5);
    EXPECT_NEAR(segs[n - 1].p2.x, p3.x, 1e-5);
    EXPECT_NEAR(segs[n - 1].p2.y, p3.y, 1e-5);
}

static void test_reduction_straight_is_one_line(void) {
    /* Evenly spaced collinear control points: the chord is exact. */
    flux_curve_seg segs[8];
    uint32_t n = 0;
    EXPECT(flux_curve_cubic_reduce(P(0, 0), P(1, 0), P(2, 0), P(3, 0), 0.25f, segs, 8, &n) ==
           FLUX_OK);
    EXPECT(n == 1);
    EXPECT(segs[0].op == FLUX_CURVE_LINE);
    EXPECT_NEAR(segs[0].p2.x, 3.0f, 1e-6);
}

static void test_inflections(void) {
    float t[2];
    /* p1, p2 chosen so the curvature sign differs at the two ends: exactly
     * one inflection, at t = 1/2. */
    uint32_t n = flux_curve_cubic_inflections(P(0, 0), P(3, 0), P(0, 3), P(3, 3), t);
    EXPECT(n == 1);
    EXPECT_NEAR(t[0], 0.5, 1e-4);
    /* A symmetric arch has none. */
    EXPECT(flux_curve_cubic_inflections(P(0, 0), P(0, 1), P(1, 1), P(1, 0), t) == 0);
    /* A straight cubic has none. */
    EXPECT(flux_curve_cubic_inflections(P(0, 0), P(1, 0), P(2, 0), P(3, 0), t) == 0);
}

static void test_cusp(void) {
    float t = -1.0f;
    /* Control polygon constructed so B'(0.5) = 0 (see the ADR derivation). */
    EXPECT(flux_curve_cubic_cusp(P(0, 0), P(-1, -1), P(-1, -1.5f), P(0, 0.5f), &t));
    EXPECT_NEAR(t, 0.5, 1e-3);
    /* The inflection example is cusp-free. */
    EXPECT(!flux_curve_cubic_cusp(P(0, 0), P(3, 0), P(0, 3), P(3, 3), &t));
    /* A collinear cubic is degenerate, not a cusp. */
    EXPECT(!flux_curve_cubic_cusp(P(0, 0), P(1, 0), P(2, 0), P(3, 0), &t));
}

static void test_wang(void) {
    flux_point p0 = P(0, 0), p1 = P(0, 2), p2 = P(2, 2), p3 = P(2, 0);
    uint32_t coarse = flux_curve_wang_cubic(p0, p1, p2, p3, 1.0f);
    uint32_t fine = flux_curve_wang_cubic(p0, p1, p2, p3, 0.01f);
    EXPECT(coarse >= 1);
    EXPECT(fine >= coarse); /* monotone in 1/tol */
    /* Straight control polygon: one segment. */
    EXPECT(flux_curve_wang_cubic(P(0, 0), P(1, 0), P(2, 0), P(3, 0), 0.25f) == 1);
    /* Invalid tolerance reports "nothing". */
    EXPECT(flux_curve_wang_cubic(p0, p1, p2, p3, 0.0f) == 0);
    /* Quadratic form: straight quad -> 1; invalid -> 0. */
    EXPECT(flux_curve_wang_quad(P(0, 0), P(1, 0), P(2, 0), 0.25f) == 1);
    EXPECT(flux_curve_wang_quad(p0, p1, p2, 0.0f) == 0);
}

static void test_reduce_fails_closed(void) {
    flux_curve_seg segs[8];
    uint32_t n = 123;
    /* Non-finite input. */
    EXPECT(flux_curve_cubic_reduce(P(0, 0), P(NAN, 0), P(2, 0), P(3, 0), 0.25f, segs, 8, &n) ==
           FLUX_ERROR_INVALID_ARGUMENT);
    EXPECT(n == 0);
    /* Non-positive tolerance. */
    EXPECT(flux_curve_cubic_reduce(P(0, 0), P(1, 1), P(2, 1), P(3, 0), 0.0f, segs, 8, &n) ==
           FLUX_ERROR_INVALID_ARGUMENT);
    EXPECT(n == 0);
    /* No output storage. */
    EXPECT(flux_curve_cubic_reduce(P(0, 0), P(1, 1), P(2, 1), P(3, 0), 0.25f, nullptr, 8, &n) ==
           FLUX_ERROR_INVALID_ARGUMENT);
    /* Capacity too small for a curve that must split: explicit, nothing written. */
    EXPECT(flux_curve_cubic_reduce(P(0, 0), P(3, 0), P(0, 3), P(3, 3), 0.25f, segs, 1, &n) ==
           FLUX_ERROR_OUT_OF_RANGE);
    EXPECT(n == 0);
}

static void test_determinism(void) {
    flux_curve_seg a[32], b[32];
    uint32_t na = 0, nb = 0;
    EXPECT(flux_curve_cubic_reduce(P(0, 0), P(3, 0), P(0, 3), P(3, 3), 0.1f, a, 32, &na) ==
           FLUX_OK);
    EXPECT(flux_curve_cubic_reduce(P(0, 0), P(3, 0), P(0, 3), P(3, 3), 0.1f, b, 32, &nb) ==
           FLUX_OK);
    EXPECT(na == nb);
    if (na == nb)
        EXPECT(memcmp(a, b, (size_t)na * sizeof(a[0])) == 0);
}

static void test_offset_line_is_exact(void) {
    flux_curve_seg line = flux_curve_seg_make(FLUX_CURVE_LINE, P(0, 0), P(5, 0), P(10, 0));
    flux_curve_seg off;
    EXPECT(flux_curve_seg_offset(line, 2.0f, &off) == FLUX_OK);
    EXPECT(off.op == FLUX_CURVE_LINE);
    /* Left normal of +x is +y. */
    EXPECT_NEAR(off.p0.x, 0.0f, 1e-5);
    EXPECT_NEAR(off.p0.y, 2.0f, 1e-5);
    EXPECT_NEAR(off.p2.x, 10.0f, 1e-5);
    EXPECT_NEAR(off.p2.y, 2.0f, 1e-5);
    /* Negative distance offsets the other way. */
    EXPECT(flux_curve_seg_offset(line, -2.0f, &off) == FLUX_OK);
    EXPECT_NEAR(off.p0.y, -2.0f, 1e-5);

    /* Degenerate tangent: explicit failure, no write. */
    flux_curve_seg degenerate = flux_curve_seg_make(FLUX_CURVE_LINE, P(1, 1), P(1, 1), P(1, 1));
    flux_curve_seg untouched = flux_curve_seg_make(FLUX_CURVE_LINE, P(9, 9), P(9, 9), P(9, 9));
    EXPECT(flux_curve_seg_offset(degenerate, 1.0f, &untouched) == FLUX_ERROR_INVALID_STATE);
    EXPECT_NEAR(untouched.p0.x, 9.0f, 1e-6);
}

static void test_offset_quad_passes_through_midpoint(void) {
    flux_curve_seg q = flux_curve_seg_make(FLUX_CURVE_QUAD, P(0, 0), P(1, 1), P(2, 0));
    flux_curve_seg off;
    EXPECT(flux_curve_seg_offset(q, 0.5f, &off) == FLUX_OK);
    EXPECT(off.op == FLUX_CURVE_QUAD);
    /* Endpoints move along the segment normals... */
    flux_point n0 = flux_curve_quad_normal(q, 0.0f);
    EXPECT_NEAR(off.p0.x, q.p0.x + 0.5f * n0.x, 1e-5);
    EXPECT_NEAR(off.p0.y, q.p0.y + 0.5f * n0.y, 1e-5);
    /* ...and the reduced offset is exact at the on-curve midpoint, by
     * construction. */
    flux_point mid = flux_curve_quad_eval(q, 0.5f);
    flux_point nm = flux_curve_quad_normal(q, 0.5f);
    flux_point om = flux_curve_quad_eval(off, 0.5f);
    EXPECT_NEAR(om.x, mid.x + 0.5f * nm.x, 1e-4);
    EXPECT_NEAR(om.y, mid.y + 0.5f * nm.y, 1e-4);
}

static void test_ray_x(void) {
    flux_curve_seg line = flux_curve_seg_make(FLUX_CURVE_LINE, P(0, 0), P(1, 1), P(2, 2));
    float xs[2];
    EXPECT(flux_curve_seg_ray_x(line, 1.0f, xs) == 1);
    EXPECT_NEAR(xs[0], 1.0f, 1e-5);
    /* Horizontal segment never crosses a horizontal ray. */
    flux_curve_seg flat = flux_curve_seg_make(FLUX_CURVE_LINE, P(0, 1), P(1, 1), P(2, 1));
    EXPECT(flux_curve_seg_ray_x(flat, 1.0f, xs) == 0);
    /* A quadratic arch is crossed twice below its apex (apex y = 1 at
     * t = 1/2). For (0,0),(1,2),(2,0): y(t) = 4t(1-t) = 1/2 has roots
     * t = (2∓√2)/4, and x(t) = 2t, so x = (2∓√2)/2. */
    flux_curve_seg quad = flux_curve_seg_make(FLUX_CURVE_QUAD, P(0, 0), P(1, 2), P(2, 0));
    uint32_t n = flux_curve_seg_ray_x(quad, 0.5f, xs);
    EXPECT(n == 2);
    if (n == 2) {
        EXPECT(xs[1] >= xs[0]);
        EXPECT_NEAR(xs[0], (2.0f - 1.41421356f) * 0.5f, 1e-4);
        EXPECT_NEAR(xs[1], (2.0f + 1.41421356f) * 0.5f, 1e-4);
    }
    /* Above the apex there is no crossing. */
    EXPECT(flux_curve_seg_ray_x(quad, 2.0f, xs) == 0);
}

static void test_winding_square(void) {
    /* CCW unit square. */
    flux_curve_seg sq[4] = {
        flux_curve_seg_make(FLUX_CURVE_LINE, P(0, 0), P(0.5f, 0), P(1, 0)),
        flux_curve_seg_make(FLUX_CURVE_LINE, P(1, 0), P(1, 0.5f), P(1, 1)),
        flux_curve_seg_make(FLUX_CURVE_LINE, P(1, 1), P(0.5f, 1), P(0, 1)),
        flux_curve_seg_make(FLUX_CURVE_LINE, P(0, 1), P(0, 0.5f), P(0, 0)),
    };
    flux_curve_cover c = {sq, 4, flux_curve_cover_bounds(sq, 4)};
    EXPECT(flux_curve_cover_winding(c, 0.5f, 0.5f) == 1);
    EXPECT(flux_curve_cover_contains(c, 0.5f, 0.5f, FLUX_CURVE_FILL_NONZERO));
    EXPECT(flux_curve_cover_contains(c, 0.5f, 0.5f, FLUX_CURVE_FILL_EVENODD));
    EXPECT(!flux_curve_cover_contains(c, 2.0f, 0.5f, FLUX_CURVE_FILL_NONZERO));
    EXPECT(!flux_curve_cover_contains(c, 0.5f, 2.0f, FLUX_CURVE_FILL_NONZERO));
    EXPECT(flux_curve_cover_winding(c, 0.5f, 0.5f) > 0); /* CCW is positive */
}

static void test_winding_quadratic_circle(void) {
    /* Unit circle from four quadratic quarter-arcs (control point at the
     * corner). This is the shape the kernel's QUAD output must handle. */
    flux_curve_seg ring[4] = {
        flux_curve_seg_make(FLUX_CURVE_QUAD, P(1, 0), P(1, 1), P(0, 1)),
        flux_curve_seg_make(FLUX_CURVE_QUAD, P(0, 1), P(-1, 1), P(-1, 0)),
        flux_curve_seg_make(FLUX_CURVE_QUAD, P(-1, 0), P(-1, -1), P(0, -1)),
        flux_curve_seg_make(FLUX_CURVE_QUAD, P(0, -1), P(1, -1), P(1, 0)),
    };
    flux_curve_cover c = {ring, 4, flux_curve_cover_bounds(ring, 4)};
    EXPECT(flux_curve_cover_contains(c, 0.0f, 0.0f, FLUX_CURVE_FILL_NONZERO));
    EXPECT(flux_curve_cover_contains(c, 0.5f, 0.0f, FLUX_CURVE_FILL_NONZERO));
    EXPECT(!flux_curve_cover_contains(c, 2.0f, 0.0f, FLUX_CURVE_FILL_NONZERO));
    EXPECT(!flux_curve_cover_contains(c, 0.0f, -2.0f, FLUX_CURVE_FILL_NONZERO));
}

static void test_winding_rules_differ_on_double_wound_ring(void) {
    /* The square traced twice: winding 2 at the centre. NONZERO paints it;
     * EVENODD must not. This is the case that forced ADR-0014's stencil
     * fallback and that the coverage contract now makes a plain count. */
    flux_curve_seg r[8];
    flux_point v[4] = {P(0, 0), P(1, 0), P(1, 1), P(0, 1)};
    for (int lap = 0; lap < 2; ++lap) {
        for (int i = 0; i < 4; ++i) {
            flux_point a = v[i], b = v[(i + 1) % 4];
            r[lap * 4 + i] = flux_curve_seg_make(FLUX_CURVE_LINE, a,
                                                 P((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f), b);
        }
    }
    flux_curve_cover c = {r, 8, flux_curve_cover_bounds(r, 8)};
    EXPECT(flux_curve_cover_winding(c, 0.5f, 0.5f) == 2);
    EXPECT(flux_curve_cover_contains(c, 0.5f, 0.5f, FLUX_CURVE_FILL_NONZERO));
    EXPECT(!flux_curve_cover_contains(c, 0.5f, 0.5f, FLUX_CURVE_FILL_EVENODD));
}

static void test_bounds_and_validation(void) {
    flux_point p0 = P(0, 0), p1 = P(0, 4), p2 = P(4, 4), p3 = P(4, 0);
    flux_curve_seg segs[16];
    uint32_t n = 0;
    EXPECT(flux_curve_cubic_reduce(p0, p1, p2, p3, 0.25f, segs, 16, &n) == FLUX_OK);
    flux_rect b = flux_curve_cover_bounds(segs, n);
    /* Conservative: the bound must contain every point of the cubic (this
     * arch spans y in [0, 3], not [0, 4] — the control hull is looser than
     * the curve itself). */
    bool contained = true;
    for (int k = 0; k <= 1000; ++k) {
        flux_point q = flux_curve_cubic_eval(p0, p1, p2, p3, (float)k / 1000.0f);
        if (q.x < b.x - 1e-4f || q.x > b.x + b.w + 1e-4f || q.y < b.y - 1e-4f ||
            q.y > b.y + b.h + 1e-4f)
            contained = false;
    }
    EXPECT(contained);
    /* The arch does reach y = 3 at t = 1/2, so the bound is tight enough to
     * be useful rather than merely "enormous". */
    EXPECT(b.y + b.h >= 3.0f - 1e-3f);

    EXPECT(flux_curve_point_is_finite(P(1, 2)));
    EXPECT(!flux_curve_point_is_finite(P(INFINITY, 0)));
    EXPECT(!flux_curve_point_is_finite(P(0, NAN)));
    EXPECT(flux_curve_seg_is_finite(segs[0]));
    flux_curve_seg bad = flux_curve_seg_make((flux_curve_op)7, P(0, 0), P(0, 0), P(0, 0));
    EXPECT(!flux_curve_seg_is_finite(bad));
}

/* Closed-form retrograde interval, verified against its defining condition:
 * inside the interval the offset travels backwards along the tangent
 * (T·O' < 0); outside it does not. */
static void test_offset_retrograde(void) {
    /* Quarter-arc quadratic (1,0),(1,1),(0,1): turns left, radius of
     * curvature ranges ~2.0 down to ~0.707. */
    flux_curve_seg q = flux_curve_seg_make(FLUX_CURVE_QUAD, P(1, 0), P(1, 1), P(0, 1));

    float t0 = -1.0f, t1 = -1.0f;
    bool has = false;

    /* dist = 1.0 exceeds the minimum radius of curvature -> retrograde. The
     * interval solves 2t² - 2t + (1 - 0.5^(2/3)) = 0. */
    EXPECT(flux_curve_seg_offset_retrograde(q, 1.0f, &t0, &t1, &has) == FLUX_OK);
    EXPECT(has);
    EXPECT_NEAR(t0, 0.24509f, 1e-3);
    EXPECT_NEAR(t1, 0.75491f, 1e-3);

    /* Independent numerical confirmation of the closed form. */
    for (int k = 1; k < 100; ++k) {
        float t = (float)k / 100.0f;
        if (fabsf(t - t0) < 0.03f || fabsf(t - t1) < 0.03f)
            continue; /* skip the neighbourhood of the cusps, where T·O' is ~0 */
        float tm = t - 1e-4f, tp = t + 1e-4f;
        if (tm < 0.0f)
            tm = 0.0f;
        if (tp > 1.0f)
            tp = 1.0f;
        flux_point bm = flux_curve_quad_eval(q, tm);
        flux_point nm = flux_curve_quad_normal(q, tm);
        flux_point bp = flux_curve_quad_eval(q, tp);
        flux_point np = flux_curve_quad_normal(q, tp);
        float dx = (bp.x + np.x) - (bm.x + nm.x);
        float dy = (bp.y + np.y) - (bm.y + nm.y);
        flux_point tan = flux_curve_quad_tangent(q, t);
        float dot = tan.x * dx + tan.y * dy;
        if (t > t0 && t < t1)
            EXPECT(dot < 0.0f);
        else
            EXPECT(dot > 0.0f);
    }

    /* dist = 0.5 stays inside the minimum radius: no retrograde. */
    EXPECT(flux_curve_seg_offset_retrograde(q, 0.5f, &t0, &t1, &has) == FLUX_OK);
    EXPECT(!has);

    /* Offsetting away from the centre never retrogrades on this curve. */
    EXPECT(flux_curve_seg_offset_retrograde(q, -1.0f, &t0, &t1, &has) == FLUX_OK);
    EXPECT(!has);

    /* A line has zero curvature: never retrograde. */
    flux_curve_seg line = flux_curve_seg_make(FLUX_CURVE_LINE, P(0, 0), P(1, 0), P(2, 0));
    EXPECT(flux_curve_seg_offset_retrograde(line, 5.0f, &t0, &t1, &has) == FLUX_OK);
    EXPECT(!has);

    /* Fails closed on non-finite input, writing nothing. */
    flux_curve_seg bad = flux_curve_seg_make(FLUX_CURVE_QUAD, P(0, 0), P(NAN, 1), P(2, 0));
    t0 = 7.0f;
    EXPECT(flux_curve_seg_offset_retrograde(bad, 1.0f, &t0, &t1, &has) ==
           FLUX_ERROR_INVALID_ARGUMENT);
    EXPECT(t0 == 7.0f);
    EXPECT(!has);
}

/* Cross-validation of the two coverage routes (ADR-0118's peers): winding over
 * curve strips versus the closed-form distance field. They share no code, so
 * agreement on the same shape is independent evidence that both are right — and
 * it is the concrete demonstration that a strip yields continuous coverage,
 * which is what ADR-0116's backends rest on.
 *
 * Two properties of this comparison are deliberate:
 *
 *  - The shape is a SQUARE of half extent 2 (lines only), because a line-only
 *    strip has an exact closed-form distance (a rounded rect of radius 0), so
 *    the comparison isolates *sampling* error. A quadratic "circle" would not
 *    work as a reference: four corner-control quarter arcs are ~2.7% off a true
 *    circle, which swamps sampling and never converges.
 *  - Probes stay where exactly ONE edge governs. `coverage = 0.5 - d` is exact
 *    for an axis-aligned edge, but near a corner two 1-pixel ramps overlap and
 *    the field's min/max combination is not the true area coverage — measured
 *    here at 0.25 against a true 0.125. That is a known property of the
 *    single-edge rule, not a defect of either method, and it is why ADR-0116's
 *    oracle compares against area rather than against the field. */
static void test_cover_coverage_agrees_with_field(void) {
    const float he = 2.0f; /* half extent */
    flux_curve_seg sq[4] = {
        {FLUX_CURVE_LINE, {-he, -he}, {0, -he}, {he, -he}},
        {FLUX_CURVE_LINE, {he, -he}, {he, 0}, {he, he}},
        {FLUX_CURVE_LINE, {he, he}, {0, he}, {-he, he}},
        {FLUX_CURVE_LINE, {-he, he}, {-he, 0}, {-he, -he}},
    };
    flux_curve_cover cover = {sq, 4, flux_curve_cover_bounds(sq, 4)};

    float worst16 = 0.0f, worst32 = 0.0f;
    int probed = 0;
    /* Sweep the 1-pixel ramp hugging the left edge (centre.x near -he) and the
     * right edge (centre.x near +he), staying clear of the top and bottom so
     * only one edge governs. */
    for (int side = 0; side < 2; ++side) {
        for (int i = 0; i < 7; ++i) {
            float centre_x =
                (side == 0 ? -(he - 0.45f) + (float)i * 0.15f : (he - 0.45f) - (float)i * 0.15f);
            for (int j = 0; j < 9; ++j) {
                float centre_y = -0.5f + (float)j * 0.125f;
                float px = centre_x - 0.5f, py = centre_y - 0.5f;
                flux_point centre = {centre_x, centre_y};
                float d = flux_sdf_round_rect(centre, he, he, 0.0f);
                float w16 = flux_curve_cover_coverage(cover, FLUX_CURVE_FILL_NONZERO, px, py, 16);
                float w32 = flux_curve_cover_coverage(cover, FLUX_CURVE_FILL_NONZERO, px, py, 32);
                float field = flux_sdf_coverage(d, 1.0f);
                float e16 = fabsf(w16 - field), e32 = fabsf(w32 - field);
                if (e16 > worst16)
                    worst16 = e16;
                if (e32 > worst32)
                    worst32 = e32;
                probed++;
            }
        }
    }
    EXPECT(probed > 20);
    EXPECT(worst16 <= 0.08f); /* O(1/n) sampling error */
    EXPECT(worst32 <= 0.04f);
    EXPECT(worst32 <= worst16 + 1e-3f); /* refinement never makes it worse */

    /* Degenerate requests are benign rather than NaN. */
    EXPECT(flux_curve_cover_coverage(cover, FLUX_CURVE_FILL_NONZERO, 0.0f, 0.0f, 0) == 0.0f);
    flux_curve_cover empty = {nullptr, 0, {0.0f, 0.0f, 0.0f, 0.0f}};
    EXPECT(flux_curve_cover_coverage(empty, FLUX_CURVE_FILL_NONZERO, 0.0f, 0.0f, 4) == 0.0f);
    /* A pixel well inside is fully covered at any refinement; one far outside
     * is empty. Both are exact, which pins the winding sign convention. */
    EXPECT_NEAR(flux_curve_cover_coverage(cover, FLUX_CURVE_FILL_NONZERO, -0.5f, -0.5f, 8), 1.0f,
                1e-6f);
    EXPECT_NEAR(flux_curve_cover_coverage(cover, FLUX_CURVE_FILL_NONZERO, 5.0f, 5.0f, 8), 0.0f,
                1e-6f);

    /* The curve-strip route produces continuous, non-binary coverage on a
     * curved boundary — the property ADR-0116's backends must have. Checked on
     * a quadratic ring against itself (a corner-control ring is not a circle,
     * so the field is not a reference here). */
    flux_curve_seg ring[4] = {
        {FLUX_CURVE_QUAD, {1, 0}, {1, 1}, {0, 1}},
        {FLUX_CURVE_QUAD, {0, 1}, {-1, 1}, {-1, 0}},
        {FLUX_CURVE_QUAD, {-1, 0}, {-1, -1}, {0, -1}},
        {FLUX_CURVE_QUAD, {0, -1}, {1, -1}, {1, 0}},
    };
    flux_curve_cover rc = {ring, 4, flux_curve_cover_bounds(ring, 4)};
    int partial = 0;
    int levels[17] = {0};
    for (int i = -12; i <= 12; ++i) {
        for (int j = -12; j <= 12; ++j) {
            float c = flux_curve_cover_coverage(rc, FLUX_CURVE_FILL_NONZERO, (float)i * 0.125f,
                                                (float)j * 0.125f, 16);
            if (c > 0.0f && c < 1.0f)
                partial++;
            int lvl = (int)(c * 16.0f + 0.5f);
            if (lvl >= 0 && lvl <= 16)
                levels[lvl]++;
        }
    }
    EXPECT(partial > 20);
    int distinct = 0;
    for (int k = 1; k < 16; ++k)
        if (levels[k] > 0)
            distinct++;
    EXPECT(distinct >= 8); /* far more than a 4x sample count's five levels */
}

int main(void) {
    test_reduction_error_bound();
    test_reduction_straight_is_one_line();
    test_inflections();
    test_cusp();
    test_wang();
    test_reduce_fails_closed();
    test_determinism();
    test_offset_line_is_exact();
    test_offset_quad_passes_through_midpoint();
    test_offset_retrograde();
    test_ray_x();
    test_winding_square();
    test_winding_quadratic_circle();
    test_winding_rules_differ_on_double_wound_ring();
    test_bounds_and_validation();
    test_cover_coverage_agrees_with_field();
    TEST_SUMMARY();
}
