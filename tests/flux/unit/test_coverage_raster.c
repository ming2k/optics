/*
 * Coverage rasterizer conformance (ADR-0119, stage S1 gate).
 *
 * `canvas_coverage_rasterize` is the algorithm a coverage backend is built on.
 * It is measured against the S0 harness, which uses a different method
 * (per-pixel polygon clipping) by design — a rasterizer validated against its
 * own helper proves nothing.
 *
 * Both are fed the SAME flattened polygon, so nothing here depends on the
 * flattener. The bar is exhaustive rather than approximate: for simple shapes the
 * two exact methods must agree to float precision, per pixel, at integer and
 * fractional grid origins. The remaining cases cover what an algebraic area
 * reference cannot answer (a self-intersecting contour, where only winding
 * works), the two fill rules, and the coverage-level requirement of INV-COV-02.
 */
#include "coverage_harness.h"
#include "test_helpers.h"

#include "../../../libs/flux/src/canvas/internal.h"
#include <flux/canvas.h>
#include <flux/canvas_cpu.h>
#include <flux/flux.h>

#include <math.h>
#include <string.h>

#define W 64u
#define H 64u

/* curve_flatten.c reports through libflux's hidden error sink; this binary
 * compiles it directly, so the symbol needs a local definition (same pattern as
 * test_canvas_flatten). */
void flux_set_last_error(flux_result code, const char *function, const char *file, int line,
                         const char *message, int32_t backend_code) {
    (void)code;
    (void)function;
    (void)file;
    (void)line;
    (void)message;
    (void)backend_code;
}

static flux_point mk(float x, float y) {
    return (flux_point){x, y};
}

/* A LINE-only cover over the same contour points, so the harness's polygon is
 * byte-identical to the rasterizer's input. */
static uint32_t cover_from(flux_curve_seg *segs, uint32_t cap, const flux_point *pts, uint32_t n) {
    uint32_t m = 0;
    for (uint32_t i = 0; i < n && m < cap; ++i) {
        flux_point a = pts[i], b = pts[(i + 1) % n];
        if (a.x == b.x && a.y == b.y)
            continue;
        flux_point mid = {(a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f};
        segs[m++] = flux_curve_seg_make(FLUX_CURVE_LINE, a, mid, b);
    }
    return m;
}

/* Rasterize and compare pixel-for-pixel with the exact-area reference. */
static void expect_exact(const char *label, const flux_point *pts, uint32_t n, float ox, float oy,
                         int min_levels) {
    flux_canvas_contour con = {.start = 0, .count = n, .closed = true};
    cov_plane ref;
    EXPECT(cov_plane_init(&ref, W, H));
    float *got = malloc((size_t)W * H * sizeof(float));
    EXPECT(got != nullptr);
    if (!got) {
        cov_plane_free(&ref);
        return;
    }

    static flux_curve_seg segs[4096];
    uint32_t ns = cover_from(segs, 4096, pts, n);
    flux_curve_cover cover = {segs, ns, flux_curve_cover_bounds(segs, ns)};

    EXPECT(cov_harness_area(cover, ox, oy, W, H, &ref) == FLUX_OK);
    EXPECT(canvas_coverage_rasterize(pts, &con, 1, FLUX_FILL_NON_ZERO, ox, oy, W, H, got) ==
           FLUX_OK);

    cov_plane g = {W, H, got};
    cov_report r;
    cov_harness_compare(&g, &ref, 1e-4f, &r);
    fprintf(stderr, "  %-24s mean=%.7f max=%.7f over1e-4=%u levels=%d worst_row=%u\n", label,
            (double)r.mean_abs, (double)r.max_abs, r.over, r.levels, r.worst_row);
    /* Two independent exact methods: agreement is a float-precision question. */
    EXPECT(r.mean_abs <= 1e-5f);
    EXPECT(r.max_abs <= 1e-4f);
    EXPECT(r.over == 0);
    EXPECT(fabs(r.worst_row_delta) <= 1e-3);
    /* INV-COV-02 applies to edges with varying sub-pixel phase; an axis-aligned
     * square legitimately carries only a handful of levels. */
    if (min_levels > 0)
        EXPECT(r.levels >= min_levels);

    free(got);
    cov_plane_free(&ref);
}

static void test_square_exact(void) {
    flux_point sq[4] = {mk(-5, -5), mk(5, -5), mk(5, 5), mk(-5, 5)};
    expect_exact("square integer", sq, 4, -6.0f, -6.0f, 0);
    expect_exact("square fractional", sq, 4, -6.37f, -5.81f, 0);
    expect_exact("square fractional 2", sq, 4, -5.13f, -6.29f, 0);
    expect_exact("square fractional 3", sq, 4, -5.997f, -6.0013f, 0);
}

/* The case that exposed the integration-parameter bug: a band that does not fill
 * its row. Circles, and anything curved, are exactly this. */
static void test_circle_exact(void) {
    const uint32_t N = 64;
    flux_point circ[64];
    for (uint32_t i = 0; i < N; ++i) {
        double a = (double)i * 2.0 * 3.14159265358979 / (double)N;
        circ[i] = mk((float)(9.0 * cos(a)), (float)(9.0 * sin(a)));
    }
    expect_exact("circle 64-gon", circ, N, -10.31f, -9.77f, 16);
    expect_exact("circle 64-gon offset", circ, N, -10.0f, -9.5f, 16);
    /* A half disc: a 32-gon closed by a chord, which has a long horizontal edge
     * and a vertical tangent at one end. */
    expect_exact("half disc", circ, N / 2, -10.31f, -9.77f, 16);
}

/* The production flattener's own output, so the rasterizer is exercised on the
 * polygon the canvas would actually hand it. */
static void test_flattened_circle_exact(void) {
    flux_arena arena;
    EXPECT(flux_arena_init(&arena, 1u << 18, nullptr) == FLUX_OK);
    flux_path *p = nullptr;
    EXPECT(flux_path_create(&p, &arena) == FLUX_OK);
    flux_path_add_circle(p, 0.0f, 0.0f, 9.0f);

    static flux_point pts[FLUX_CANVAS_PATH_SCRATCH_CAP];
    static flux_canvas_contour cons[FLUX_CANVAS_MAX_CONTOURS];
    flatten_multi fm = flatten_path_to_contours(p, 1.0f, pts, FLUX_CANVAS_PATH_SCRATCH_CAP, cons,
                                                FLUX_CANVAS_MAX_CONTOURS);
    EXPECT(fm.contour_count == 1);
    EXPECT(fm.point_count > 8);
    expect_exact("flattened circle r=9", pts, cons[0].count, -10.31f, -9.77f, 16);

    flux_arena_deinit(&arena);
}

/* Self-intersecting input: the area reference refuses it (algebraic area
 * cancels), and only winding can answer. This is the case the canvas currently
 * answers with a stencil pass. */
static void test_self_intersection_nonzero(void) {
    const int k = 5;
    flux_point v[5];
    for (int i = 0; i < k; ++i) {
        double a = -1.5707963 + (double)i * (2.0 * 3.14159265358979 / (double)k);
        v[i] = mk((float)(6.0 * cos(a)), (float)(6.0 * sin(a)));
    }
    const int order[5] = {0, 2, 4, 1, 3};
    flux_point star[5];
    for (int i = 0; i < k; ++i)
        star[i] = v[order[i]];

    flux_canvas_contour con = {.start = 0, .count = 5, .closed = true};
    float *got = malloc((size_t)W * H * sizeof(float));
    EXPECT(got != nullptr);
    if (!got)
        return;

    flux_curve_seg segs[8];
    uint32_t ns = cover_from(segs, 8, star, 5);
    flux_curve_cover cover = {segs, ns, flux_curve_cover_bounds(segs, ns)};

    cov_plane ref;
    EXPECT(cov_plane_init(&ref, W, H));
    /* The exact-area reference must refuse, not report a cancelling zero. */
    EXPECT(cov_harness_area(cover, -8.0f, -8.0f, W, H, &ref) == FLUX_ERROR_UNSUPPORTED);
    /* The supersampled reference is the correct yardstick here. */
    EXPECT(cov_harness_supersample(cover, FLUX_CURVE_FILL_NONZERO, -8.0f, -8.0f, W, H, 64, &ref) ==
           FLUX_OK);

    EXPECT(canvas_coverage_rasterize(star, &con, 1, FLUX_FILL_NON_ZERO, -8.0f, -8.0f, W, H, got) ==
           FLUX_OK);
    cov_plane g = {W, H, got};
    cov_report r;
    cov_harness_compare(&g, &ref, 0.02f, &r);
    fprintf(stderr, "  %-24s mean=%.6f max=%.6f over2%%=%u levels=%d\n", "star (nonzero)",
            (double)r.mean_abs, (double)r.max_abs, r.over, r.levels);
    /* 64x64 supersampling is the yardstick, so the floor is its own sampling
     * error rather than exactness. */
    EXPECT(r.mean_abs <= 0.004f);
    /* The yardstick here is 64x64 supersampling, not an exact method: at the
     * star's thin spikes a 1/64 grid cannot resolve the edge, so the floor is the
     * reference's own error. The mean (6e-5) is what carries the claim. */
    EXPECT(r.max_abs <= 0.08f);

    /* The star really is painted: its lobes carry winding >= 1. A cancelling
     * area sum would have produced ~0 here. */
    double sum = 0.0;
    for (size_t i = 0; i < (size_t)W * H; ++i)
        sum += (double)got[i];
    fprintf(stderr, "  star total coverage %.3f\n", sum);
    EXPECT(sum > 20.0);

    free(got);
    cov_plane_free(&ref);
}

/* The two fill rules must genuinely differ on nested same-winding contours. */
static void test_fill_rules_differ(void) {
    flux_point pts[8] = {mk(-6, -6), mk(6, -6), mk(6, 6), mk(-6, 6),
                         mk(-3, -3), mk(3, -3), mk(3, 3), mk(-3, 3)};
    flux_canvas_contour cons[2] = {{.start = 0, .count = 4, .closed = true},
                                   {.start = 4, .count = 4, .closed = true}};
    float *nz = malloc((size_t)W * H * sizeof(float));
    float *eo = malloc((size_t)W * H * sizeof(float));
    EXPECT(nz && eo);
    if (!nz || !eo) {
        free(nz);
        free(eo);
        return;
    }
    EXPECT(canvas_coverage_rasterize(pts, cons, 2, FLUX_FILL_NON_ZERO, -8.0f, -8.0f, W, H, nz) ==
           FLUX_OK);
    EXPECT(canvas_coverage_rasterize(pts, cons, 2, FLUX_FILL_EVEN_ODD, -8.0f, -8.0f, W, H, eo) ==
           FLUX_OK);

    double snz = 0.0, seo = 0.0;
    for (size_t i = 0; i < (size_t)W * H; ++i) {
        snz += (double)nz[i];
        seo += (double)eo[i];
    }
    /* Non-zero fills the whole 12x12 square (144); even-odd leaves the inner
     * 6x6 (36) empty, giving 108. */
    EXPECT_NEAR(snz, 144.0, 0.01);
    EXPECT_NEAR(seo, 108.0, 0.01);

    free(nz);
    free(eo);
}

static void test_failure_modes(void) {
    flux_point sq[4] = {mk(-5, -5), mk(5, -5), mk(5, 5), mk(-5, 5)};
    flux_canvas_contour con = {.start = 0, .count = 4, .closed = true};
    float *cov = malloc((size_t)W * H * sizeof(float));
    EXPECT(cov != nullptr);
    if (!cov)
        return;
    EXPECT(canvas_coverage_rasterize(nullptr, &con, 1, FLUX_FILL_NON_ZERO, 0, 0, W, H, cov) ==
           FLUX_ERROR_INVALID_ARGUMENT);
    EXPECT(canvas_coverage_rasterize(sq, nullptr, 1, FLUX_FILL_NON_ZERO, 0, 0, W, H, cov) ==
           FLUX_ERROR_INVALID_ARGUMENT);
    EXPECT(canvas_coverage_rasterize(sq, &con, 1, FLUX_FILL_NON_ZERO, 0, 0, W, H, nullptr) ==
           FLUX_ERROR_INVALID_ARGUMENT);
    /* Degenerate contours rasterize to nothing rather than erroring. */
    flux_canvas_contour one = {.start = 0, .count = 1, .closed = false};
    EXPECT(canvas_coverage_rasterize(sq, &one, 1, FLUX_FILL_NON_ZERO, -6.0f, -6.0f, W, H, cov) ==
           FLUX_OK);
    for (size_t i = 0; i < (size_t)W * H; ++i)
        EXPECT(cov[i] == 0.0f);
    free(cov);
}

/* Phase A gate: >= 1024-vertex fills, verifying both non-zero and even-odd rule stability. */
static void test_high_vertex_count_stress(void) {
    const uint32_t N = 1024;
    flux_point *pts = malloc((size_t)N * sizeof(flux_point));
    float *cov_nz = malloc((size_t)W * H * sizeof(float));
    float *cov_eo = malloc((size_t)W * H * sizeof(float));
    EXPECT(pts && cov_nz && cov_eo);
    if (!pts || !cov_nz || !cov_eo) {
        free(pts);
        free(cov_nz);
        free(cov_eo);
        return;
    }

    /* 1. Large 1024-gon circle */
    for (uint32_t i = 0; i < N; ++i) {
        double a = (double)i * 2.0 * 3.14159265358979 / (double)N;
        pts[i] = mk((float)(20.0 * cos(a)), (float)(20.0 * sin(a)));
    }
    flux_canvas_contour con = {.start = 0, .count = N, .closed = true};
    EXPECT(canvas_coverage_rasterize(pts, &con, 1, FLUX_FILL_NON_ZERO, -32.0f, -32.0f, W, H,
                                     cov_nz) == FLUX_OK);
    double circle_area = 0.0;
    for (size_t i = 0; i < (size_t)W * H; ++i) {
        EXPECT(cov_nz[i] >= 0.0f && cov_nz[i] <= 1.0f);
        circle_area += (double)cov_nz[i];
    }
    /* Pi * 20^2 approx 1256.64 */
    EXPECT_NEAR(circle_area, 1256.64, 2.0);

    /* 2. Self-intersecting 1024-pointed star */
    for (uint32_t i = 0; i < N; ++i) {
        double a = (double)i * 2.0 * 3.14159265358979 / (double)N;
        double r = (i % 2 == 0) ? 25.0 : 8.0;
        pts[i] = mk((float)(r * cos(a)), (float)(r * sin(a)));
    }
    EXPECT(canvas_coverage_rasterize(pts, &con, 1, FLUX_FILL_NON_ZERO, -32.0f, -32.0f, W, H,
                                     cov_nz) == FLUX_OK);
    EXPECT(canvas_coverage_rasterize(pts, &con, 1, FLUX_FILL_EVEN_ODD, -32.0f, -32.0f, W, H,
                                     cov_eo) == FLUX_OK);
    double star_nz = 0.0, star_eo = 0.0;
    for (size_t i = 0; i < (size_t)W * H; ++i) {
        EXPECT(cov_nz[i] >= 0.0f && cov_nz[i] <= 1.0f);
        EXPECT(cov_eo[i] >= 0.0f && cov_eo[i] <= 1.0f);
        star_nz += (double)cov_nz[i];
        star_eo += (double)cov_eo[i];
    }
    EXPECT(star_nz > 100.0);
    EXPECT(star_eo > 100.0);

    free(pts);
    free(cov_nz);
    free(cov_eo);
}

/* Collinear, zero-length, sub-epsilon edges, and horizontal spans. */
static void test_near_degenerate_edges(void) {
    float *cov = malloc((size_t)W * H * sizeof(float));
    EXPECT(cov != nullptr);
    if (!cov)
        return;

    /* 1. Fully collinear polygon (3 vertices forming a line): exact 0 area. */
    flux_point collinear[3] = {mk(-10.0f, -10.0f), mk(0.0f, 0.0f), mk(10.0f, 10.0f)};
    flux_canvas_contour con3 = {.start = 0, .count = 3, .closed = true};
    EXPECT(canvas_coverage_rasterize(collinear, &con3, 1, FLUX_FILL_NON_ZERO, -16.0f, -16.0f, W, H,
                                     cov) == FLUX_OK);
    for (size_t i = 0; i < (size_t)W * H; ++i)
        EXPECT(cov[i] == 0.0f);

    /* 2. Microscopic edges with near-equal y coordinates (delta y < 1e-7). */
    flux_point micro[4] = {
        mk(-5.0f, 0.0f),
        mk(5.0f, 1e-8f),
        mk(5.0f, 10.0f),
        mk(-5.0f, 10.0f + 1e-8f),
    };
    flux_canvas_contour con4 = {.start = 0, .count = 4, .closed = true};
    EXPECT(canvas_coverage_rasterize(micro, &con4, 1, FLUX_FILL_NON_ZERO, -10.0f, -2.0f, W, H,
                                     cov) == FLUX_OK);
    double micro_area = 0.0;
    for (size_t i = 0; i < (size_t)W * H; ++i) {
        EXPECT(cov[i] >= 0.0f && cov[i] <= 1.0f);
        micro_area += (double)cov[i];
    }
    /* 10 x 10 square approx 100 area */
    EXPECT_NEAR(micro_area, 100.0, 0.05);

    /* 3. Pure horizontal edges back and forth. */
    flux_point horiz[4] = {mk(-10.0f, 5.0f), mk(10.0f, 5.0f), mk(10.0f, 5.0f), mk(-10.0f, 5.0f)};
    EXPECT(canvas_coverage_rasterize(horiz, &con4, 1, FLUX_FILL_NON_ZERO, -16.0f, 0.0f, W, H,
                                     cov) == FLUX_OK);
    for (size_t i = 0; i < (size_t)W * H; ++i)
        EXPECT(cov[i] == 0.0f);

    free(cov);
}

int main(void) {
    test_square_exact();
    test_circle_exact();
    test_flattened_circle_exact();
    test_self_intersection_nonzero();
    test_fill_rules_differ();
    test_failure_modes();
    test_high_vertex_count_stress();
    test_near_degenerate_edges();
    TEST_SUMMARY();
}
