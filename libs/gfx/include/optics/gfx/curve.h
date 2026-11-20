/*
 * flux/curve.h — analytic curve kernel (ADR-0115).
 *
 * The single authority for curve geometry semantics. It reduces Bézier
 * input to line/quadratic segments with a *bounded* deviation, offsets them
 * analytically, and exposes the primitives (bounds, ray intersection,
 * winding) a rasterizer needs to compute per-pixel coverage. It never emits
 * triangles and never names a backend.
 *
 * Leaf module: depends only on <flux/math.h> and the core error surface. It
 * does not include canvas, scene, or compute (ADR-0016, INV-CRV-02).
 *
 * Determinism: for identical input the kernel produces identical output on
 * every platform and optimisation level (INV-CRV-05). Analytic root finding
 * uses double internally where the closed form is ill-conditioned; no
 * recursion over the heap, no allocation (INV-CRV-03), no global state.
 */

#ifndef OPTICS_GFX_CURVE_H
#define OPTICS_GFX_CURVE_H

#include <optics/gfx/core.h>
#include <optics/gfx/math.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================== */
/*  Reduced segments                                                  */
/* ================================================================== */

typedef enum optics_gfx_curve_op : uint32_t {
    OPTICS_GFX_CURVE_LINE = 0, /* straight p0 -> p2; p1 is the midpoint */
    OPTICS_GFX_CURVE_QUAD = 1, /* quadratic Bézier through p0, p1, p2 */
} optics_gfx_curve_op;

/* A reduced curve segment. The kernel emits only these; there is no cubic
 * segment type on purpose — a cubic is always reduced before it is handed
 * to a rasterizer, so a backend can never reinterpret raw control points
 * (INV-CRV-01). */
typedef struct optics_gfx_curve_seg {
    uint32_t op; /* flux_curve_op */
    optics_gfx_point p0;
    optics_gfx_point p1; /* control point; for FLUX_CURVE_LINE it is (p0+p2)/2 */
    optics_gfx_point p2;
} optics_gfx_curve_seg;

/* Standard 2D line segment in physical device space (ADR-0121).
 * Exactly 16 bytes for direct zero-copy GPU BDA / SSBO transport. */
typedef struct optics_gfx_segment {
    float x0, y0;
    float x1, y1;
} optics_gfx_segment;

/* Construct a reduced segment. `p1` is ignored for FLUX_CURVE_LINE. */
OPTICS_GFX_API optics_gfx_curve_seg optics_gfx_curve_seg_make(optics_gfx_curve_op op,
                                                              optics_gfx_point p0,
                                                              optics_gfx_point p1,
                                                              optics_gfx_point p2);

/* Fill rule for containment. Mirrors flux_fill_rule (canvas) but is
 * declared here so the kernel stays a leaf module. The caller maps one to
 * the other; they are the same two values. */
typedef enum optics_gfx_curve_fill : uint32_t {
    OPTICS_GFX_CURVE_FILL_NONZERO = 0,
    OPTICS_GFX_CURVE_FILL_EVENODD = 1,
} optics_gfx_curve_fill;

/* A run of reduced segments forming a closed path, with a conservative
 * bounding box. The segment storage is caller-owned (arena or stack); the
 * kernel neither allocates it nor keeps a reference to it. */
typedef struct optics_gfx_curve_cover {
    const optics_gfx_curve_seg *segs;
    uint32_t count;
    optics_gfx_rect bounds; /* conservative (control-hull) bound of the run */
} optics_gfx_curve_cover;

/* Worst-case deviation of the single-quadratic reduction of a cubic, as a
 * fraction of |P3 - 3·P2 + 3·P1 - P0|. Exact: the residual is
 * (E/6)·3t(1-t)(1-2t), whose maximum magnitude is |E|/(6·2√3). Callers and
 * tests may use it directly instead of re-deriving it. */
#define OPTICS_GFX_CURVE_QUAD_REDUCTION_ERROR (1.0f / (6.0f * 3.4641016f)) /* ≈ 0.0481125 */

/* ================================================================== */
/*  Evaluation                                                        */
/* ================================================================== */

OPTICS_GFX_API optics_gfx_point optics_gfx_curve_cubic_eval(optics_gfx_point p0,
                                                            optics_gfx_point p1,
                                                            optics_gfx_point p2,
                                                            optics_gfx_point p3, float t);
OPTICS_GFX_API optics_gfx_point optics_gfx_curve_quad_eval(optics_gfx_curve_seg s, float t);
/* Unit tangent at t; zero vector when the derivative vanishes (cusp). */
OPTICS_GFX_API optics_gfx_point optics_gfx_curve_quad_tangent(optics_gfx_curve_seg s, float t);
/* Unit left normal at t (tangent rotated +90°). */
OPTICS_GFX_API optics_gfx_point optics_gfx_curve_quad_normal(optics_gfx_curve_seg s, float t);

/* ================================================================== */
/*  Analytic decomposition (ADR-0115 points 1–2)                      */
/* ================================================================== */

/* Parameters in (0,1) where the cubic's curvature changes sign. Writes at
 * most 2 values; returns the count. Pure closed form, no search. */
OPTICS_GFX_API uint32_t optics_gfx_curve_cubic_inflections(optics_gfx_point p0, optics_gfx_point p1,
                                                           optics_gfx_point p2, optics_gfx_point p3,
                                                           float out_t[2]);

/* True and writes t when the cubic has a genuine cusp (B'(t) = 0 with a
 * reversed tangent) for some t in (0,1). Closed-form candidate roots,
 * verified against the derivative; no Newton search. */
OPTICS_GFX_API bool optics_gfx_curve_cubic_cusp(optics_gfx_point p0, optics_gfx_point p1,
                                                optics_gfx_point p2, optics_gfx_point p3,
                                                float *out_t);

/* ================================================================== */
/*  Segment count (Wang's formula, ADR-0115 point 3)                  */
/* ================================================================== */

/* Minimal uniform segment count that keeps the chord deviation of a cubic
 * below `tol` (tol > 0): ceil( sqrt( (3/(4·tol)) · max‖Δ²P‖ ) ). */
OPTICS_GFX_API uint32_t optics_gfx_curve_wang_cubic(optics_gfx_point p0, optics_gfx_point p1,
                                                    optics_gfx_point p2, optics_gfx_point p3,
                                                    float tol);
/* Quadratic form: ceil( sqrt( (1/(4·tol)) · ‖P0 - 2·P1 + P2‖ ) ). */
OPTICS_GFX_API uint32_t optics_gfx_curve_wang_quad(optics_gfx_point p0, optics_gfx_point p1,
                                                   optics_gfx_point p2, float tol);

/* ================================================================== */
/*  Reduction (ADR-0115 point 1)                                      */
/* ================================================================== */

/* Reduce a cubic to line/quadratic segments whose maximum deviation from
 * the cubic is <= tol. The cubic is first split at its inflections and cusp
 * (a quadratic cannot represent an S-curve or a cusp), then each monotone
 * piece is reduced with the exact FLUX_CURVE_QUAD_REDUCTION_ERROR bound,
 * subdividing at t = 1/2 while the bound is exceeded.
 *
 * Writes at most `cap` segments to `out`. Returns FLUX_OK and the produced
 * count on success. On any failure (tol <= 0, non-finite input, unsatisfiable
 * within `cap`, or exhaustion of the subdivision budget) it writes nothing,
 * sets `*out_count` to 0, and returns an explicit error — never a partial or
 * silently-wrong strip (INV-CRV-04). */
OPTICS_GFX_NODISCARD OPTICS_GFX_API optics_gfx_result optics_gfx_curve_cubic_reduce(
    optics_gfx_point p0, optics_gfx_point p1, optics_gfx_point p2, optics_gfx_point p3, float tol,
    optics_gfx_curve_seg *out, uint32_t cap, uint32_t *out_count);

/* ================================================================== */
/*  Analytic offset (ADR-0115 point 4)                                */
/* ================================================================== */

/* Offset a reduced segment by `dist` along its left normal. The line case is
 * exact. The quadratic case is the quadratic interpolating the offset at
 * t = 0, 1/2, 1; its deviation from the true parallel curve is bounded and
 * returns to zero as the segment straightens. `dist` may be negative.
 *
 * The offset is undefined where the segment's tangent vanishes (a cusp
 * endpoint), so this is fallible: on a degenerate normal it writes nothing
 * and returns FLUX_ERROR_INVALID_STATE rather than emitting a NaN or an
 * un-offset segment (INV-CRV-04). */
OPTICS_GFX_NODISCARD OPTICS_GFX_API optics_gfx_result
optics_gfx_curve_seg_offset(optics_gfx_curve_seg s, float dist, optics_gfx_curve_seg *out);

/* Retrograde interval of an offset segment.
 *
 * Offsetting a segment toward its centre of curvature by more than the local
 * radius of curvature makes the offset travel *backwards* along the tangent:
 * the offset curve self-intersects and a naive stroke emits overlapping
 * geometry there (the translucent dark-seam artifact). For a quadratic this is
 * closed-form: the curvature numerator `cross(B', B'')` is constant, so the
 * cusp condition `dist · κ(t) = 1` reduces to a quadratic in t. A line has
 * zero curvature and never retrogrades.
 *
 * On success writes the interval `[*t0, *t1]` (clamped to [0,1]) and sets
 * `*has` when a non-empty retrograde interval exists within the segment. Fails
 * on non-finite input, writing nothing (INV-CRV-04). */
OPTICS_GFX_NODISCARD OPTICS_GFX_API optics_gfx_result optics_gfx_curve_seg_offset_retrograde(
    optics_gfx_curve_seg s, float dist, float *t0, float *t1, bool *has);

/* ================================================================== */
/*  Coverage primitives (ADR-0115 point 5)                            */
/* ================================================================== */

/* Conservative control-hull bounds of a reduced segment. */
OPTICS_GFX_API optics_gfx_rect optics_gfx_curve_seg_bounds(optics_gfx_curve_seg s);

/* x-coordinates where the segment crosses the horizontal line y = yv, in
 * ascending order, at most 2, over the closed parameter domain [0,1]. Used
 * to build a rasterizer's per-row/per-tile winding backdrop. A horizontal
 * segment reports no crossing. */
OPTICS_GFX_API uint32_t optics_gfx_curve_seg_ray_x(optics_gfx_curve_seg s, float yv,
                                                   float out_x[2]);

/* Conservative bounds of a run of segments. */
OPTICS_GFX_API optics_gfx_rect optics_gfx_curve_cover_bounds(const optics_gfx_curve_seg *segs,
                                                             uint32_t count);

/* Signed winding number of the closed path at (x, y): +1 per upward crossing
 * of the ray to +x, −1 per downward crossing. A shared vertex between
 * consecutive segments is counted exactly once (half-open parameter domain),
 * so an open run under-counts by design — close your cover. */
OPTICS_GFX_API int32_t optics_gfx_curve_cover_winding(optics_gfx_curve_cover cover, float x,
                                                      float y);

/* Inside test under `rule`. NONZERO: winding != 0. EVENODD: winding & 1. */
OPTICS_GFX_API bool optics_gfx_curve_cover_contains(optics_gfx_curve_cover cover, float x, float y,
                                                    optics_gfx_curve_fill rule);

/* Coverage of the pixel square [x, x+1) x [y, y+1) under `rule`, from an
 * n x n grid of winding samples.
 *
 * This is SUPERSAMPLED coverage, and is named so deliberately: it is a
 * *reference* evaluator, not the analytic area the coverage backends of
 * ADR-0116 must produce. Its purpose is cross-validation — it computes
 * coverage from the winding of curve strips, an entirely different route from
 * the closed-form distance fields in <flux/sdf.h>, so agreement between the two
 * on the same shape is independent evidence that both are right (see
 * test_curve's circle test). `n` is clamped to [1, 64].
 *
 * Sampling error is O(1/n) for a straight edge and smaller for a curved one,
 * so n = 16 (256 samples) already agrees with the exact field to a few percent
 * of a pixel. */
OPTICS_GFX_API float optics_gfx_curve_cover_coverage(optics_gfx_curve_cover cover,
                                                     optics_gfx_curve_fill rule, float x, float y,
                                                     uint32_t n);

/* ================================================================== */
/*  Validation                                                        */
/* ================================================================== */

OPTICS_GFX_API bool optics_gfx_curve_point_is_finite(optics_gfx_point p);
OPTICS_GFX_API bool optics_gfx_curve_seg_is_finite(optics_gfx_curve_seg s);

#ifdef __cplusplus
}
#endif

#endif /* FLUX_CURVE_H */
