#include "internal.h"

#include <math.h>

/* ------------------------------------------------------------------ */
/*  Path flattening                                                   */
/*                                                                    */
/*  Subdivide curves until the chord-to-control-point deviation is    */
/*  below FLATTEN_PIXEL_TOLERANCE *in screen space*. Callers supply a */
/*  `pixel_scale` (the operator norm of the active transform's        */
/*  linear part); object-space tolerance is FLATTEN_PIXEL_TOLERANCE / */
/*  pixel_scale. A safety depth cap stops runaway recursion on        */
/*  pathological inputs (e.g. cusps with chord_len → 0).              */
/* ------------------------------------------------------------------ */

#define FLATTEN_PIXEL_TOLERANCE 0.25f
#define FLATTEN_MAX_DEPTH 16

static void emit_point(flux_point *out, uint32_t *count, uint32_t cap, float x, float y) {
    if (*count >= cap) {
        FLUX_FAIL(FLUX_ERROR_OUT_OF_RANGE,
                  "canvas path scratch exhausted "
                  "(FLUX_CANVAS_PATH_SCRATCH_CAP); flattened points dropped");
        return;
    }
    out[*count].x = x;
    out[*count].y = y;
    (*count)++;
}

/* Squared perpendicular distance from `(qx, qy)` to the line through
 * `(ax, ay)`–`(bx, by)`, multiplied by |b - a|² (so callers compare
 * against tol² × |b - a|² instead of dividing). */
static inline float perp_cross_sq(float ax, float ay, float bx, float by, float qx, float qy) {
    float cx = (bx - ax) * (qy - ay) - (by - ay) * (qx - ax);
    return cx * cx;
}

static void flatten_cubic(flux_point *out, uint32_t *count, uint32_t cap, float x0, float y0,
                          float x1, float y1, float x2, float y2, float x3, float y3, float tol_sq,
                          int depth) {
    /* Cheap flatness test: both control points are within `tol` of
     * the chord. Falls through to subdivision if the chord is
     * degenerate (|chord|² ≈ 0); the depth cap bounds that case. */
    float chord_x = x3 - x0, chord_y = y3 - y0;
    float chord_len_sq = chord_x * chord_x + chord_y * chord_y;
    if (chord_len_sq > 1e-12f) {
        float c1sq = perp_cross_sq(x0, y0, x3, y3, x1, y1);
        float c2sq = perp_cross_sq(x0, y0, x3, y3, x2, y2);
        float max_sq = c1sq > c2sq ? c1sq : c2sq;
        if (max_sq <= tol_sq * chord_len_sq) {
            emit_point(out, count, cap, x3, y3);
            return;
        }
    }
    if (depth >= FLATTEN_MAX_DEPTH) {
        emit_point(out, count, cap, x3, y3);
        return;
    }

    float m01x = (x0 + x1) * 0.5f, m01y = (y0 + y1) * 0.5f;
    float m12x = (x1 + x2) * 0.5f, m12y = (y1 + y2) * 0.5f;
    float m23x = (x2 + x3) * 0.5f, m23y = (y2 + y3) * 0.5f;
    float m012x = (m01x + m12x) * 0.5f, m012y = (m01y + m12y) * 0.5f;
    float m123x = (m12x + m23x) * 0.5f, m123y = (m12y + m23y) * 0.5f;
    float midx = (m012x + m123x) * 0.5f, midy = (m012y + m123y) * 0.5f;

    flatten_cubic(out, count, cap, x0, y0, m01x, m01y, m012x, m012y, midx, midy, tol_sq, depth + 1);
    flatten_cubic(out, count, cap, midx, midy, m123x, m123y, m23x, m23y, x3, y3, tol_sq, depth + 1);
}

static void flatten_quad(flux_point *out, uint32_t *count, uint32_t cap, float x0, float y0,
                         float x1, float y1, float x2, float y2, float tol_sq, int depth) {
    float chord_x = x2 - x0, chord_y = y2 - y0;
    float chord_len_sq = chord_x * chord_x + chord_y * chord_y;
    if (chord_len_sq > 1e-12f) {
        float csq = perp_cross_sq(x0, y0, x2, y2, x1, y1);
        if (csq <= tol_sq * chord_len_sq) {
            emit_point(out, count, cap, x2, y2);
            return;
        }
    }
    if (depth >= FLATTEN_MAX_DEPTH) {
        emit_point(out, count, cap, x2, y2);
        return;
    }

    float m01x = (x0 + x1) * 0.5f, m01y = (y0 + y1) * 0.5f;
    float m12x = (x1 + x2) * 0.5f, m12y = (y1 + y2) * 0.5f;
    float midx = (m01x + m12x) * 0.5f, midy = (m01y + m12y) * 0.5f;

    flatten_quad(out, count, cap, x0, y0, m01x, m01y, midx, midy, tol_sq, depth + 1);
    flatten_quad(out, count, cap, midx, midy, m12x, m12y, x2, y2, tol_sq, depth + 1);
}

/* Walk the path, emitting per-contour spans. Each MOVE op begins a
 * new contour; CLOSE finishes one. Implicit contour boundaries (a
 * second MOVE without a preceding CLOSE) are also honoured. */
flatten_multi flatten_path_to_contours(const flux_path *p, float pixel_scale, flux_point *out_pts,
                                       uint32_t pts_cap, flux_canvas_contour *out_cons,
                                       uint32_t cons_cap) {
    flatten_multi res = {0};
    float cx = 0.0f, cy = 0.0f;
    flux_canvas_contour *cur = nullptr;

    /* Pre-square the tolerance in object space. `pixel_scale` is the
     * operator-norm upper bound of the active transform's linear
     * part; small scales (UI default = 1) keep object tolerance ≈
     * 0.25 px, larger scales tighten it so screen-space deviation
     * stays under the pixel threshold. */
    float scale = pixel_scale > 0.0f ? pixel_scale : 1.0f;
    float tol = FLATTEN_PIXEL_TOLERANCE / scale;
    float tol_sq = tol * tol;

#define BEGIN_CONTOUR(fx, fy)                                                                      \
    do {                                                                                           \
        if (res.contour_count >= cons_cap)                                                         \
            return res;                                                                            \
        cur = &out_cons[res.contour_count++];                                                      \
        cur->start = res.point_count;                                                              \
        cur->count = 0;                                                                            \
        cur->closed = false;                                                                       \
        cur->first_x = (fx);                                                                       \
        cur->first_y = (fy);                                                                       \
    } while (0)

#define EMIT_POINT(x, y)                                                                           \
    do {                                                                                           \
        if (res.point_count < pts_cap) {                                                           \
            out_pts[res.point_count++] = (flux_point){(x), (y)};                                   \
            if (cur)                                                                               \
                cur->count++;                                                                      \
        }                                                                                          \
    } while (0)

    for (uint32_t i = 0; i < p->count; ++i) {
        const flux_path_segment *s = &p->segments[i];
        switch (s->op) {
        case FLUX_PATH_MOVE:
            cx = s->pts[0];
            cy = s->pts[1];
            BEGIN_CONTOUR(cx, cy);
            EMIT_POINT(cx, cy);
            break;
        case FLUX_PATH_LINE:
            if (!cur) {
                BEGIN_CONTOUR(cx, cy);
                EMIT_POINT(cx, cy);
            }
            /* Skip zero-length segments so the stroker doesn't see
             * duplicate vertices. */
            if (cx != s->pts[0] || cy != s->pts[1]) {
                cx = s->pts[0];
                cy = s->pts[1];
                EMIT_POINT(cx, cy);
            }
            break;
        case FLUX_PATH_QUAD: {
            if (!cur) {
                BEGIN_CONTOUR(cx, cy);
                EMIT_POINT(cx, cy);
            }
            uint32_t before = res.point_count;
            flatten_quad(out_pts, &res.point_count, pts_cap, cx, cy, s->pts[0], s->pts[1],
                         s->pts[2], s->pts[3], tol_sq, 0);
            if (cur)
                cur->count += res.point_count - before;
            cx = s->pts[2];
            cy = s->pts[3];
            break;
        }
        case FLUX_PATH_CUBIC: {
            if (!cur) {
                BEGIN_CONTOUR(cx, cy);
                EMIT_POINT(cx, cy);
            }
            uint32_t before = res.point_count;
            flatten_cubic(out_pts, &res.point_count, pts_cap, cx, cy, s->pts[0], s->pts[1],
                          s->pts[2], s->pts[3], s->pts[4], s->pts[5], tol_sq, 0);
            if (cur)
                cur->count += res.point_count - before;
            cx = s->pts[4];
            cy = s->pts[5];
            break;
        }
        case FLUX_PATH_CLOSE:
            if (cur && cur->count > 0) {
                /* Trim near-duplicate of first point, then emit
                 * explicit close to guarantee the contour is closed. */
                flux_point last = out_pts[cur->start + cur->count - 1];
                float dx = last.x - cur->first_x;
                float dy = last.y - cur->first_y;
                if (dx * dx + dy * dy < 1e-6f) {
                    res.point_count--;
                    cur->count--;
                }
                EMIT_POINT(cur->first_x, cur->first_y);
                cx = cur->first_x;
                cy = cur->first_y;
                cur->closed = true;
            }
            cur = nullptr; /* next op opens a new contour if not MOVE */
            break;
        }
    }
#undef BEGIN_CONTOUR
#undef EMIT_POINT
    return res;
}

/* Operator-norm upper bound for a 2D affine transform's linear part. */
float flux_canvas_mat3x2_pixel_scale(flux_mat3x2 m) {
    float c0 = m.m[0] * m.m[0] + m.m[1] * m.m[1];
    float c1 = m.m[2] * m.m[2] + m.m[3] * m.m[3];
    float max_col = c0 > c1 ? c0 : c1;
    return sqrtf(max_col);
}
