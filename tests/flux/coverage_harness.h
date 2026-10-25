/*
 * Coverage acceptance harness (ADR-0119, stage S0).
 *
 * This is the ground truth every coverage backend is measured against, and it
 * is deliberately NOT a backend: it lives in the test tree, it is slow, and it
 * shares no algorithm with any rasterizer in libflux. A backend that validated
 * itself against its own helper would prove nothing (ADR-0112's postmortem,
 * applied to tests).
 *
 * Two independent references, each with a different failure mode:
 *
 *   cov_harness_area()        exact per-pixel planar area, computed by clipping
 *                             the flattened contours to each pixel and taking
 *                             signed areas. One arithmetic identity pins it:
 *                             the coverage sum must equal the geometry's signed
 *                             area, exactly.
 *   cov_harness_supersample() n x n winding samples (flux_curve_cover_coverage).
 *                             Trivially correct, converges as O(1/n); used to
 *                             confirm the area reference on shapes where the two
 *                             must agree.
 *
 * Scoping, stated up front so no gate rests on the wrong thing: the area
 * reference computes the **algebraic** (winding-weighted) area. That equals true
 * nonzero coverage when contours are simple and properly wound — fills, holes,
 * glyph outlines — and it deliberately does NOT equal nonzero coverage for a
 * self-intersecting contour, where overlapping lobes cancel algebraically but
 * paint in nonzero. `test_cover_reference` asserts that disagreement as a
 * negative control rather than hiding it.
 *
 * Coverage is reported in [0, 1] as float, not 8-bit: the whole point of
 * INV-COV-02 is counting distinct levels, and quantising the yardstick to 255
 * steps would cap what it can measure.
 */
#ifndef COVERAGE_HARNESS_H
#define COVERAGE_HARNESS_H

#include <flux/curve.h>
#include <flux/flux.h>

#include <stdbool.h>
#include <stdint.h>

/* Finite flattening tolerance for references, in pixels. Small enough that the
 * polygon-vs-curve difference is far below any coverage tolerance under test. */
#define COV_FLATTEN_TOL 1e-3f

/* A coverage plane, coverage in [0, 1], row-major. */
typedef struct cov_plane {
    uint32_t w, h;
    float *px;
} cov_plane;

typedef struct cov_report {
    float mean_abs; /* mean |got - ref| over all pixels */
    float max_abs;  /* worst single-pixel deviation */
    uint32_t over;  /* pixels deviating by more than the caller's threshold */
    uint32_t total; /* pixels compared */
    int levels;     /* distinct coverage levels in `got`, in units of 1/255 */
    bool clamped;   /* the reference needed clamping (should never happen) */

    /* Per-row localisation. A global mean says a rasterizer is wrong; the row
     * carrying the worst *total* deviation says where. This is what turned
     * "1.6% of the area is missing" into "the row containing a vertical
     * tangent" during S1 (ADR-0119), so it belongs in the report rather than in
     * a debug patch. */
    uint32_t worst_row;     /* row with the largest row-sum deviation */
    double worst_row_delta; /* signed: got row sum minus reference row sum */
} cov_report;

/* Signed area of the flattened cover, summed over its segments. For a closed
 * cover this is the algebraic area the coverage plane must sum to. */
double cov_harness_signed_area(flux_curve_cover cover);

/* Exact algebraic-area coverage of `cover` over a w x h pixel grid whose pixel
 * (i, j) spans [ox+i, ox+i+1) x [oy+j, oy+j+1). Returns FLUX_OK, or an explicit
 * error if the flattened contours exceed the harness's fixed capacity (fail
 * closed rather than truncating a reference).
 *
 * The caller owns `out->px` (w*h floats); this function fills it. */
FLUX_NODISCARD flux_result cov_harness_area(flux_curve_cover cover, float ox, float oy, uint32_t w,
                                            uint32_t h, cov_plane *out);

/* Supersampled coverage of `cover` under `rule`, same grid, n x n samples per
 * pixel (n clamped by flux_curve_cover_coverage to [1,64]). */
FLUX_NODISCARD flux_result cov_harness_supersample(flux_curve_cover cover, flux_curve_fill rule,
                                                   float ox, float oy, uint32_t w, uint32_t h,
                                                   uint32_t n, cov_plane *out);

/* Convenience: allocate a zeroed plane. Returns false on allocation failure. */
bool cov_plane_init(cov_plane *p, uint32_t w, uint32_t h);
void cov_plane_free(cov_plane *p);

/* Compare `got` against `ref` and fill one report. `threshold` is the
 * per-pixel deviation counted as a violation. */
void cov_harness_compare(const cov_plane *got, const cov_plane *ref, float threshold,
                         cov_report *out);

/* Number of distinct coverage levels in `plane`, quantised to 1/255 (the finest
 * an 8-bit output could carry). INV-COV-02 requires this to exceed 16 for a
 * high-contrast oblique edge — a 4x sample count can only produce 5. */
int cov_harness_levels(const cov_plane *plane);

#endif /* COVERAGE_HARNESS_H */
