/*
 * Path → polyline contour bridge (ADR-0115).
 *
 * Curve geometry semantics live in the flux_curve kernel; this file only
 * walks a flux_path and turns its curve verbs into contour point runs for the
 * rasterizer. There is no recursive subdivision and no depth cap and no
 * flatness search here: each curve is sampled at the segment count the
 * kernel's Wang's formula returns, a closed form with a provable deviation
 * bound.
 *
 * Why Wang's formula and not the kernel's cubic reduction: the reduction
 * exists to feed offsetting and analytic coverage (ADR-0115 points 1, 4, 5),
 * where a monotone piece is required. A rasterizer bridge only needs a
 * bounded polyline, and uniform sampling of the source curve at Wang's N is
 * exactly that — one stage instead of two, with the same guarantee. The
 * reduction stays the right tool downstream, not here.
 *
 * The screen-space tolerance is preserved: callers pass the operator norm of
 * the active transform's linear part, and the object-space tolerance is
 * TOLERANCE / pixel_scale, so a curve occupying few screen pixels is
 * flattened coarsely and a magnified one tightly.
 */
#include "internal.h"

#include <optics/gfx/curve.h>

#include <math.h>

/* Same screen-space tolerance the legacy flattener used (0.25 px). */
#define CURVE_FLATTEN_PIXEL_TOLERANCE 0.25f

/* Upper bound on the samples a single curve may contribute. Keeps one
 * pathological curve from consuming the whole scratch budget that later
 * contours in the same path need. Beyond this the polyline is coarser than
 * the tolerance asks for — a bounded, documented degradation, in the same
 * class as the scratch-buffer cap it protects. */
#define CURVE_FLATTEN_MAX_SPAN 16384

typedef struct flatten_state {
    optics_gfx_flatten_grow_fn grow; /* optional grow callback; nullptr for fixed buffers */
    void *grow_ctx;
    optics_gfx_point *pts;
    optics_gfx_canvas_contour *cons;
    uint32_t pts_cap;
    uint32_t cons_cap;
    uint32_t point_count;
    uint32_t contour_count;
    optics_gfx_canvas_contour *cur; /* active contour, or nullptr between contours */
    optics_gfx_point last;          /* last point written, for zero-length suppression */
    bool have_last;
    bool done; /* set when buffer growth fails or fixed cap is reached */
} flatten_state;

static bool ensure_points_room(flatten_state *st) {
    if (st->point_count < st->pts_cap)
        return true;
    if (st->grow && st->grow(st->grow_ctx, st->point_count + 1, 0, &st->pts, &st->pts_cap,
                             &st->cons, &st->cons_cap)) {
        return true;
    }
    if (!st->grow) {
        OPTICS_GFX_FAIL(OPTICS_GFX_ERROR_OUT_OF_RANGE,
                        "canvas path scratch exhausted (FLUX_CANVAS_PATH_SCRATCH_CAP); "
                        "flattened points dropped");
    }
    st->done = true;
    return false;
}

/* Append a point unconditionally, growing scratch via callback when provided. */
static void emit_point(flatten_state *st, float x, float y) {
    if (st->done || !ensure_points_room(st))
        return;
    st->pts[st->point_count++] = (optics_gfx_point){x, y};
    if (st->cur)
        st->cur->count++;
    st->last = (optics_gfx_point){x, y};
    st->have_last = true;
}

/* Append a point unless it repeats the previous one. This is what makes a
 * zero-length curve (all anchors coincident) emit nothing, instead of
 * flooding the buffer with copies of one point. */
static void emit_point_unique(flatten_state *st, float x, float y) {
    if (st->have_last && st->last.x == x && st->last.y == y)
        return;
    emit_point(st, x, y);
}

/* Open a contour at the cursor unless one is already open. Grows canvas contour
 * scratch when owned; returns false on fixed-cap exhaustion or OOM. */
static bool begin_if_needed(flatten_state *st, float cx, float cy) {
    if (st->cur)
        return true;
    if (st->contour_count >= st->cons_cap) {
        if (st->grow && st->grow(st->grow_ctx, 0, st->contour_count + 1, &st->pts, &st->pts_cap,
                                 &st->cons, &st->cons_cap)) {
        } else {
            st->done = true;
            return false;
        }
    }
    optics_gfx_canvas_contour *co = &st->cons[st->contour_count++];
    co->start = st->point_count;
    co->count = 0;
    co->closed = false;
    co->first_x = cx;
    co->first_y = cy;
    st->cur = co;
    emit_point(st, cx, cy);
    return !st->done;
}

/* Sample a quadratic (or line) at `k` uniform steps, emitting the interior and
 * final points (the start point is already in the buffer). */
static void sample_quad(flatten_state *st, optics_gfx_curve_seg s, float tol) {
    uint32_t k = optics_gfx_curve_wang_quad(s.p0, s.p1, s.p2, tol);
    if (k < 1)
        k = 1;
    if (k > CURVE_FLATTEN_MAX_SPAN)
        k = CURVE_FLATTEN_MAX_SPAN;
    float inv = 1.0f / (float)k;
    for (uint32_t i = 1; i <= k; ++i) {
        optics_gfx_point p = optics_gfx_curve_quad_eval(s, (float)i * inv);
        emit_point_unique(st, p.x, p.y);
        if (st->done)
            return;
    }
}

static void flatten_cubic(flatten_state *st, optics_gfx_point p0, optics_gfx_point p1,
                          optics_gfx_point p2, optics_gfx_point p3, float tol) {
    uint32_t k = optics_gfx_curve_wang_cubic(p0, p1, p2, p3, tol);
    if (k < 1)
        k = 1;
    if (k > CURVE_FLATTEN_MAX_SPAN)
        k = CURVE_FLATTEN_MAX_SPAN;
    float inv = 1.0f / (float)k;
    for (uint32_t i = 1; i <= k; ++i) {
        optics_gfx_point p = optics_gfx_curve_cubic_eval(p0, p1, p2, p3, (float)i * inv);
        emit_point_unique(st, p.x, p.y);
        if (st->done)
            return;
    }
}

flatten_multi flatten_path_to_contours_dyn(const optics_gfx_path *p, float pixel_scale,
                                           optics_gfx_point *out_pts, uint32_t pts_cap,
                                           optics_gfx_canvas_contour *out_cons, uint32_t cons_cap,
                                           optics_gfx_flatten_grow_fn grow, void *grow_ctx) {
    flatten_state st = {};
    st.grow = grow;
    st.grow_ctx = grow_ctx;
    st.pts = out_pts;
    st.cons = out_cons;
    st.pts_cap = pts_cap;
    st.cons_cap = cons_cap;

    if (!p || !out_pts || !out_cons) {
        flatten_multi empty = {0, 0};
        return empty;
    }

    float scale = pixel_scale > 0.0f ? pixel_scale : 1.0f;
    float tol = CURVE_FLATTEN_PIXEL_TOLERANCE / scale;
    /* The kernel rejects a non-positive tolerance; a magnitude that
     * underflows to zero is a broken transform, not a licence to emit
     * nothing. Clamp instead of dropping geometry. */
    if (!(tol > 0.0f) || !isfinite(tol))
        tol = 1e-6f;

    float cx = 0.0f, cy = 0.0f;

    for (uint32_t i = 0; i < p->count && !st.done; ++i) {
        const optics_gfx_path_segment *s = &p->segments[i];
        switch (s->op) {
        case OPTICS_GFX_PATH_MOVE:
            st.cur = nullptr;
            st.have_last = false;
            cx = s->pts[0];
            cy = s->pts[1];
            begin_if_needed(&st, cx, cy);
            break;
        case OPTICS_GFX_PATH_LINE:
            if (!begin_if_needed(&st, cx, cy))
                break;
            /* Skip zero-length segments so the stroker never sees duplicate
             * vertices. */
            if (s->pts[0] != cx || s->pts[1] != cy) {
                cx = s->pts[0];
                cy = s->pts[1];
                emit_point_unique(&st, cx, cy);
            }
            break;
        case OPTICS_GFX_PATH_QUAD:
            if (!begin_if_needed(&st, cx, cy))
                break;
            sample_quad(&st,
                        optics_gfx_curve_seg_make(OPTICS_GFX_CURVE_QUAD, (optics_gfx_point){cx, cy},
                                                  (optics_gfx_point){s->pts[0], s->pts[1]},
                                                  (optics_gfx_point){s->pts[2], s->pts[3]}),
                        tol);
            cx = s->pts[2];
            cy = s->pts[3];
            break;
        case OPTICS_GFX_PATH_CUBIC:
            if (!begin_if_needed(&st, cx, cy))
                break;
            flatten_cubic(&st, (optics_gfx_point){cx, cy}, (optics_gfx_point){s->pts[0], s->pts[1]},
                          (optics_gfx_point){s->pts[2], s->pts[3]},
                          (optics_gfx_point){s->pts[4], s->pts[5]}, tol);
            cx = s->pts[4];
            cy = s->pts[5];
            break;
        case OPTICS_GFX_PATH_CLOSE:
            if (st.cur && st.cur->count > 0) {
                /* Trim a near-duplicate of the first point, then emit an
                 * explicit close so the contour is closed. */
                optics_gfx_point prev = st.pts[st.cur->start + st.cur->count - 1];
                float dx = prev.x - st.cur->first_x;
                float dy = prev.y - st.cur->first_y;
                if (dx * dx + dy * dy < 1e-6f) {
                    st.point_count--;
                    st.cur->count--;
                }
                emit_point(&st, st.cur->first_x, st.cur->first_y);
                cx = st.cur->first_x;
                cy = st.cur->first_y;
                st.cur->closed = true;
            }
            st.cur = nullptr; /* a following non-MOVE op opens a new contour */
            break;
        }
    }

    flatten_multi res = {st.point_count, st.contour_count};
    return res;
}

flatten_multi flatten_path_to_contours(const optics_gfx_path *p, float pixel_scale,
                                       optics_gfx_point *out_pts, uint32_t pts_cap,
                                       optics_gfx_canvas_contour *out_cons, uint32_t cons_cap) {
    return flatten_path_to_contours_dyn(p, pixel_scale, out_pts, pts_cap, out_cons, cons_cap,
                                        nullptr, nullptr);
}

/* Operator-norm upper bound of `m`'s 2×2 linear part. Callers pass the
 * result as `pixel_scale` so the per-pixel tolerance survives non-identity
 * transforms. */
float optics_gfx_canvas_mat3x2_pixel_scale(optics_gfx_mat3x2 m) {
    float c0 = m.m[0] * m.m[0] + m.m[1] * m.m[1];
    float c1 = m.m[2] * m.m[2] + m.m[3] * m.m[3];
    float max_col = c0 > c1 ? c0 : c1;
    return sqrtf(max_col);
}
