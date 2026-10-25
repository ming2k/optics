/*
 * Coverage acceptance harness — self-verification (ADR-0119, stage S0).
 *
 * This test does not test a rasterizer. It tests the *yardstick* that every
 * later coverage backend will be measured against, because a wrong reference
 * makes every downstream gate meaningless. Three independent checks:
 *
 *  1. Area identity. The per-pixel coverage the reference produces must sum to
 *     the geometry's signed area. This is exact arithmetic, not a tolerance, and
 *     it catches any clipping or orientation bug immediately.
 *  2. Cross-route convergence. On shapes where both references are valid, the
 *     exact-area plane must approach the supersampled plane as samples
 *     increase, with the error falling like 1/n.
 *  3. Scoping. On a self-intersecting contour the two references must
 *     *disagree* — the area reference computes algebraic area, nonzero fill
 *     paints overlapping lobes. Asserting the disagreement is what keeps a
 *     future gate from resting on the wrong one.
 *
 * Plus the INV-COV-02 assertion: a high-contrast oblique edge must carry more
 * than 16 distinct coverage levels, which a 4x sample count cannot express.
 */
#include "coverage_harness.h"

#include "test_helpers.h"

#include <math.h>
#include <string.h>

#define W 48u
#define H 48u

static flux_point P(float x, float y) {
    return (flux_point){x, y};
}

/* Axis-aligned square as four lines, half extent `he`, centred at the origin. */
static uint32_t make_square(flux_curve_seg *segs, float he) {
    segs[0] = flux_curve_seg_make(FLUX_CURVE_LINE, P(-he, -he), P(0, -he), P(he, -he));
    segs[1] = flux_curve_seg_make(FLUX_CURVE_LINE, P(he, -he), P(he, 0), P(he, he));
    segs[2] = flux_curve_seg_make(FLUX_CURVE_LINE, P(he, he), P(0, he), P(-he, he));
    segs[3] = flux_curve_seg_make(FLUX_CURVE_LINE, P(-he, he), P(-he, 0), P(-he, -he));
    return 4;
}

/* The area identity, and the same identity after a fractional offset that makes
 * every pixel boundary cut the shape — the case a rounding bug hides in. */
static void test_area_identity(void) {
    flux_curve_seg segs[8];
    uint32_t n = make_square(segs, 5.0f);
    flux_curve_cover cover = {segs, n, flux_curve_cover_bounds(segs, n)};

    double expect = cov_harness_signed_area(cover);
    EXPECT_NEAR(expect, 100.0, 1e-3); /* 10 x 10 */

    /* Every offset keeps the square wholly inside the grid: an offset that
     * pushed it off-grid would legitimately produce the area of only the
     * visible part, and asserting the full area there would be a wrong test. */
    const float offsets[3][2] = {{-6.0f, -6.0f}, {-6.37f, -5.81f}, {-5.5f, -6.4f}};
    for (int k = 0; k < 3; ++k) {
        cov_plane pl;
        EXPECT(cov_plane_init(&pl, W, H));
        EXPECT(cov_harness_area(cover, offsets[k][0], offsets[k][1], W, H, &pl) == FLUX_OK);
        double sum = 0.0;
        for (size_t i = 0; i < (size_t)W * H; ++i)
            sum += (double)pl.px[i];
        /* Exact arithmetic: the sum of per-pixel areas IS the area. This holds
         * at integer offsets too, where the plane is legitimately binary. */
        EXPECT_NEAR(sum, expect, 1e-3);
        /* A fractional offset must grade the boundary. (The >16-level
         * requirement of INV-COV-02 belongs to the oblique-edge test below: an
         * axis-aligned square has only four edge phases in view.) */
        if (k > 0)
            EXPECT(cov_harness_levels(&pl) > 2);
        cov_plane_free(&pl);
    }
}

/* A hole: two contours, opposite winding. The identity must still hold, and it
 * must hold with the hole *subtracted* — the check that proves clipping
 * preserves orientation rather than absolute area. */
static void test_area_identity_with_hole(void) {
    flux_curve_seg segs[8];
    uint32_t n = make_square(segs, 5.0f);
    flux_curve_seg hole[4];
    uint32_t hn = make_square(hole, 2.0f);
    /* Reverse the hole so its signed area is negative. Reversing the contour
     * means reversing the segment ORDER *and* swapping each segment's
     * endpoints — swapping order alone leaves the endpoints disconnected. */
    flux_curve_seg rev[4];
    for (uint32_t i = 0; i < hn; ++i) {
        const flux_curve_seg *src = &hole[hn - 1 - i];
        rev[i] = flux_curve_seg_make((flux_curve_op)src->op, src->p2, src->p1, src->p0);
    }
    memcpy(hole, rev, hn * sizeof(hole[0]));
    flux_curve_seg all[12];
    memcpy(all, segs, n * sizeof(segs[0]));
    memcpy(all + n, hole, hn * sizeof(hole[0]));
    flux_curve_cover cover = {all, n + hn, flux_curve_cover_bounds(all, n + hn)};

    double expect = cov_harness_signed_area(cover);
    EXPECT_NEAR(expect, 100.0 - 16.0, 1e-3); /* outer minus hole */

    cov_plane pl;
    EXPECT(cov_plane_init(&pl, W, H));
    EXPECT(cov_harness_area(cover, -6.0f, -6.0f, W, H, &pl) == FLUX_OK);
    double sum = 0.0;
    for (size_t i = 0; i < (size_t)W * H; ++i)
        sum += (double)pl.px[i];
    EXPECT_NEAR(sum, expect, 1e-3);
    /* The middle of the hole carries no coverage. */
    uint32_t mid = (H / 2) * W + (W / 2);
    EXPECT_NEAR(pl.px[mid], 0.0, 1e-6);
    cov_plane_free(&pl);
}

/* Exact-area vs supersampled: both valid on a simple shape, so the exact one is
 * the limit the sampled one must converge to. */
static void test_cross_route_convergence(void) {
    flux_curve_seg segs[8];
    uint32_t n = make_square(segs, 5.0f);
    flux_curve_cover cover = {segs, n, flux_curve_cover_bounds(segs, n)};

    cov_plane area, ss8, ss32;
    EXPECT(cov_plane_init(&area, W, H));
    EXPECT(cov_plane_init(&ss8, W, H));
    EXPECT(cov_plane_init(&ss32, W, H));
    EXPECT(cov_harness_area(cover, -6.37f, -5.81f, W, H, &area) == FLUX_OK);
    EXPECT(cov_harness_supersample(cover, FLUX_CURVE_FILL_NONZERO, -6.37f, -5.81f, W, H, 8, &ss8) ==
           FLUX_OK);
    EXPECT(cov_harness_supersample(cover, FLUX_CURVE_FILL_NONZERO, -6.37f, -5.81f, W, H, 32,
                                   &ss32) == FLUX_OK);

    cov_report r8, r32;
    cov_harness_compare(&ss8, &area, 0.25f, &r8);
    cov_harness_compare(&ss32, &area, 0.05f, &r32);
    /* Sampling error shrinks with n and is bounded by ~1/(2n) per edge. */
    EXPECT(r8.mean_abs <= 0.02f);
    EXPECT(r32.mean_abs <= 0.005f);
    EXPECT(r32.mean_abs <= r8.mean_abs + 1e-4f);
    EXPECT(r8.total == W * H);
    /* The per-row localisation is populated: the row carrying the worst total
     * deviation, and its sign. On a shape this regular the area must be right,
     * so no row may be materially off — that is the property the report exists
     * to expose. */
    EXPECT(r32.worst_row < H);
    EXPECT(fabs(r32.worst_row_delta) <= 0.05);

    cov_plane_free(&area);
    cov_plane_free(&ss8);
    cov_plane_free(&ss32);
}

/* Scoping negative control: a self-intersecting star. The algebraic area and the
 * nonzero-painted area are genuinely different, so the two references must
 * disagree — and this test asserts that they do, so no future gate can quietly
 * use the algebraic area as if it were nonzero coverage. */
static void test_self_intersection_is_out_of_scope(void) {
    /* Five-point star drawn as one contour, crossing itself. */
    const int k = 5;
    flux_point v[5];
    for (int i = 0; i < k; ++i) {
        /* Vertices on a regular pentagon (2*pi/5apart, i.e. 72 degrees), then
         * connected every second vertex by `order` below to form a pentagram.
         * A 4*pi/5 step here would wrap the ring twice and produce a convex
         * polygon instead — which the harness would rightly call simple. */
        double a = -1.5707963 + (double)i * (2.0 * 3.14159265358979 / (double)k);
        v[i] = P((float)(5.0 * cos(a)), (float)(5.0 * sin(a)));
    }
    flux_curve_seg segs[5];
    int order[5] = {0, 2, 4, 1, 3}; /* the pentagram order */
    for (int i = 0; i < k; ++i) {
        flux_point a = v[order[i]], b = v[order[(i + 1) % k]];
        segs[i] =
            flux_curve_seg_make(FLUX_CURVE_LINE, a, P((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f), b);
    }
    flux_curve_cover cover = {segs, 5, flux_curve_cover_bounds(segs, 5)};

    cov_plane area, ss;
    EXPECT(cov_plane_init(&area, W, H));
    EXPECT(cov_plane_init(&ss, W, H));
    /* The area reference must REFUSE this input, not return a small number.
     * Clipping a self-intersecting contour and summing signed areas cancels the
     * lobes and yields ~0 — a yardstick that reports nothing must never be
     * trusted by a gate. */
    EXPECT(cov_harness_area(cover, -6.0f, -6.0f, W, H, &area) == FLUX_ERROR_UNSUPPORTED);
    /* The supersampled reference is the right tool here, and it reports real
     * coverage: the pentagram paints its lobes at winding >= 1. */
    EXPECT(cov_harness_supersample(cover, FLUX_CURVE_FILL_NONZERO, -6.0f, -6.0f, W, H, 32, &ss) ==
           FLUX_OK);
    double sum_ss = 0.0;
    for (size_t i = 0; i < (size_t)W * H; ++i)
        sum_ss += (double)ss.px[i];
    EXPECT(sum_ss > 20.0); /* a 10-unit star covers tens of pixels */
    fprintf(stderr, "  self-intersecting star: area reference refuses, nonzero coverage %.2f\n",
            sum_ss);

    cov_plane_free(&area);
    cov_plane_free(&ss);
}

/* INV-COV-02 on a high-contrast oblique edge: a 4x sample count yields 5 levels,
 * an area-accurate reference must yield far more. */
static void test_oblique_edge_levels(void) {
    /* A triangle whose slanted edge has a phase that varies from row to row.
     * A 45-degree edge would cut every pixel exactly corner-to-corner and
     * legitimately yield only {0, 0.5, 1} — the geometry, not the reference. */
    flux_curve_seg tri[4] = {
        flux_curve_seg_make(FLUX_CURVE_LINE, P(-8, -8), P(0, -8.4f), P(8, -8.8f)),
        flux_curve_seg_make(FLUX_CURVE_LINE, P(8, -8.8f), P(8, 0), P(8, 8)),
        flux_curve_seg_make(FLUX_CURVE_LINE, P(8, 8), P(0, 0), P(-7, 8.3f)),
        flux_curve_seg_make(FLUX_CURVE_LINE, P(-7, 8.3f), P(-7.5f, 0), P(-8, -8)),
    };
    flux_curve_cover cover = {tri, 4, flux_curve_cover_bounds(tri, 4)};

    cov_plane area, ss4;
    EXPECT(cov_plane_init(&area, W, H));
    EXPECT(cov_plane_init(&ss4, W, H));
    EXPECT(cov_harness_area(cover, -8.0f, -8.0f, W, H, &area) == FLUX_OK);
    /* 2x2 per pixel is what a 4x sample count gives. */
    EXPECT(cov_harness_supersample(cover, FLUX_CURVE_FILL_NONZERO, -8.0f, -8.0f, W, H, 2, &ss4) ==
           FLUX_OK);

    int levels_area = cov_harness_levels(&area);
    int levels_ss4 = cov_harness_levels(&ss4);
    fprintf(stderr, "  oblique edge: area reference %d levels, 4x sample count %d levels\n",
            levels_area, levels_ss4);
    EXPECT(levels_area > 16);
    EXPECT(levels_ss4 <= 8); /* the ceiling ADR-0116 exists to remove */
    EXPECT(levels_area > levels_ss4);

    cov_plane_free(&area);
    cov_plane_free(&ss4);
}

/* Fails closed rather than truncating a reference. */
static void test_failure_modes(void) {
    flux_curve_seg segs[8];
    uint32_t n = make_square(segs, 5.0f);
    flux_curve_cover cover = {segs, n, flux_curve_cover_bounds(segs, n)};

    cov_plane pl;
    EXPECT(cov_plane_init(&pl, 4, 4));
    /* Null / mismatched plane is an explicit error, not a crash. */
    EXPECT(cov_harness_area(cover, 0.0f, 0.0f, 4, 4, nullptr) == FLUX_ERROR_INVALID_ARGUMENT);
    EXPECT(cov_harness_area(cover, 0.0f, 0.0f, 8, 8, &pl) == FLUX_ERROR_INVALID_ARGUMENT);
    EXPECT(cov_harness_supersample(cover, FLUX_CURVE_FILL_NONZERO, 0.0f, 0.0f, 8, 8, 4, &pl) ==
           FLUX_ERROR_INVALID_ARGUMENT);
    /* An empty cover is zero coverage, not an error. */
    flux_curve_cover empty = {nullptr, 0, {0.0f, 0.0f, 0.0f, 0.0f}};
    EXPECT(cov_harness_area(empty, 0.0f, 0.0f, 4, 4, &pl) == FLUX_OK);
    for (size_t i = 0; i < 16; ++i)
        EXPECT(pl.px[i] == 0.0f);
    cov_plane_free(&pl);
}

int main(void) {
    test_area_identity();
    test_area_identity_with_hole();
    test_cross_route_convergence();
    test_self_intersection_is_out_of_scope();
    test_oblique_edge_levels();
    test_failure_modes();
    TEST_SUMMARY();
}
