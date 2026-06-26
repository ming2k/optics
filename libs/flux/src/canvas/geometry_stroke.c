#include "internal.h"

#include <math.h>

/* ------------------------------------------------------------------ */
/*  Stroker                                                           */
/*                                                                    */
/*  Polyline -> outline polygon -> triangle strip via bisector        */
/*  offsetting. Per-vertex normals computed from the segments on      */
/*  either side; offset distance is half-width times the miter        */
/*  factor (1/cos(theta/2)). When miter_factor exceeds miter_limit    */
/*  we break the vertex into two offset positions and emit join       */
/*  geometry (bevel triangle or round arc fan) between them.          */
/*  Endpoints get cap geometry (none / square / round).               */
/*                                                                    */
/*  Both sides offset symmetrically; for translucent strokes this     */
/*  produces no overdraw at joins.                                    */
/* ------------------------------------------------------------------ */

/* Per-vertex offset frame derived from incoming + outgoing tangents. */
stroke_frame compute_frame(flux_point prev, flux_point pt, flux_point next, float half,
                           float miter_limit, bool has_prev, bool has_next) {
    stroke_frame f = {0};
    flux_point d_in = has_prev ? pt_sub(pt, prev) : (flux_point){0, 0};
    flux_point d_out = has_next ? pt_sub(next, pt) : (flux_point){0, 0};
    float l_in = sqrtf(d_in.x * d_in.x + d_in.y * d_in.y);
    float l_out = sqrtf(d_out.x * d_out.x + d_out.y * d_out.y);
    if (l_in > 0.0f) {
        d_in.x /= l_in;
        d_in.y /= l_in;
    } else if (has_prev)
        d_in = d_out; /* zero-length in: use outgoing */
    if (l_out > 0.0f) {
        d_out.x /= l_out;
        d_out.y /= l_out;
    } else if (has_next)
        d_out = d_in; /* zero-length out: use incoming */
    if (!has_prev)
        d_in = d_out; /* first vertex: in == out */
    if (!has_next)
        d_out = d_in; /* last  vertex: out == in */

    /* Left normal: rotate tangent 90° CCW. */
    f.n_in = (flux_point){-d_in.y, d_in.x};
    f.n_out = (flux_point){-d_out.y, d_out.x};

    flux_point bisector_sum = pt_add(f.n_in, f.n_out);
    float bis_len = sqrtf(bisector_sum.x * bisector_sum.x + bisector_sum.y * bisector_sum.y);

    /* Cross product of tangents tells us turn direction:
     *   d_in × d_out > 0 → turning left (outside on left)
     *   d_in × d_out < 0 → turning right
     * Used to pick which side gets the join geometry. */
    float cross = d_in.x * d_out.y - d_in.y * d_out.x;
    f.turning_left = cross > 0.0f;

    if (bis_len < 1e-6f) {
        /* 180° flip (cusp) — bisector is degenerate. Use n_in for both
         * sides; the join logic emits an explicit cap-like turnaround. */
        f.left = pt_add(pt, pt_scale(f.n_in, +half));
        f.right = pt_add(pt, pt_scale(f.n_in, -half));
        f.miter_ok = false;
        return f;
    }

    flux_point bisector = pt_scale(bisector_sum, 1.0f / bis_len);
    /* cos(theta/2) = dot(bisector, n_in) — guaranteed >= 0 because
     * we built bisector from n_in + n_out (both unit, same hemisphere). */
    float cos_half = bisector.x * f.n_in.x + bisector.y * f.n_in.y;
    if (cos_half < 1e-3f)
        cos_half = 1e-3f;
    float miter_factor = 1.0f / cos_half;

    if (miter_factor > miter_limit) {
        /* Miter overflows — keep per-segment outer offsets and let
         * the join geometry fill between. */
        f.left = pt_add(pt, pt_scale(f.n_in, +half));
        f.right = pt_add(pt, pt_scale(f.n_in, -half));
        f.miter_ok = false;
    } else {
        f.left = pt_add(pt, pt_scale(bisector, +half * miter_factor));
        f.right = pt_add(pt, pt_scale(bisector, -half * miter_factor));
        f.miter_ok = true;
    }
    return f;
}

/* Emit a circular arc (fan from `centre`) sweeping from start_angle
 * to end_angle (radians). Caller picks the angular step. */
void emit_arc(flux_canvas_vertex *verts, uint32_t *count, uint32_t cap, flux_mat3x2 tx,
              flux_color color, flux_point centre, float radius, float start_angle, float sweep,
              uint32_t steps) {
    if (steps == 0)
        return;
    float step = sweep / (float)steps;
    flux_point prev = {
        centre.x + radius * cosf(start_angle),
        centre.y + radius * sinf(start_angle),
    };
    for (uint32_t i = 1; i <= steps; ++i) {
        float a = start_angle + step * (float)i;
        flux_point cur = {
            centre.x + radius * cosf(a),
            centre.y + radius * sinf(a),
        };
        emit_tri(verts, count, cap, tx, color, centre, prev, cur);
        prev = cur;
    }
}

/* Stroke a single flattened contour (`pts[0..count)`) into the shared
 * `verts` buffer. Factored out of flux_canvas_stroke_path so a path with
 * several subpaths (a move_to starts a new one — e.g. the divider line in a
 * sidebar glyph, or a gear's hub circle plus its body) strokes every subpath,
 * not just the first. */
static void stroke_one_contour(flux_canvas *c, const flux_paint *paint, flux_mat3x2 tx,
                               flux_color color, float half, float miter_limit, flux_point *pts,
                               uint32_t count, bool contour_closed, float first_x, float first_y,
                               flux_canvas_vertex *verts, uint32_t *v_count_io,
                               uint32_t verts_cap) {
    uint32_t n = count;
    if (n < 2)
        return;

    /* If closed, the last point coincides with the first; rotate
     * indexing so the wrap-around join is handled the same as any
     * interior join. We treat the polyline as cyclic when closed. */
    bool closed = contour_closed && n > 1;
    if (closed) {
        float dx = pts[n - 1].x - first_x;
        float dy = pts[n - 1].y - first_y;
        if (dx * dx + dy * dy > 1e-4f)
            closed = false;
    }
    if (closed)
        n--; /* drop the duplicate closing vertex */
    if (n < 2)
        return;

    uint32_t v_count = *v_count_io;

    /* Build per-vertex frames first so segment body can reference
     * both endpoints' offsets. Bounded by FLUX_CANVAS_PATH_SCRATCH_CAP. */
    stroke_frame *frames = c->scratch_frames;
    if (n > FLUX_CANVAS_PATH_SCRATCH_CAP) {
        FLUX_FAIL(FLUX_ERROR_OUT_OF_RANGE, "stroker polyline exceeds FLUX_CANVAS_PATH_SCRATCH_CAP; "
                                           "tail vertices dropped");
        n = FLUX_CANVAS_PATH_SCRATCH_CAP;
    }
    for (uint32_t i = 0; i < n; ++i) {
        bool has_prev = closed ? true : (i > 0);
        bool has_next = closed ? true : (i + 1 < n);
        flux_point prev = pts[has_prev ? (i == 0 ? n - 1 : i - 1) : 0];
        flux_point next = pts[has_next ? ((i + 1) % n) : 0];
        frames[i] = compute_frame(prev, pts[i], next, half, miter_limit, has_prev, has_next);
    }

    /* Body strip: between consecutive vertices i and i+1, emit two
     * triangles forming the quad (a.right, a.left, b.left, b.right). */
    uint32_t seg_count = closed ? n : (n > 0 ? n - 1 : 0);
    for (uint32_t s = 0; s < seg_count; ++s) {
        const stroke_frame *a = &frames[s];
        const stroke_frame *b = &frames[(s + 1) % n];
        emit_tri(verts, &v_count, verts_cap, tx, color, a->right, a->left, b->left);
        emit_tri(verts, &v_count, verts_cap, tx, color, a->right, b->left, b->right);
    }

    /* Joins at interior vertices where miter overflowed. */
    uint32_t join_first = closed ? 0 : 1;
    uint32_t join_last = closed ? n : (n > 1 ? n - 1 : 0);
    for (uint32_t i = join_first; i < join_last; ++i) {
        const stroke_frame *f = &frames[i];
        if (f->miter_ok)
            continue;

        /* Outer side depends on turn direction. The frame's
         * n_in/n_out give the segment normals at point i. */
        flux_point pt = pts[i];
        /* Per-segment outer points: incoming side and outgoing side. */
        flux_point in_outer = pt_add(
            pt, pt_scale(f->turning_left ? f->n_in : (flux_point){-f->n_in.x, -f->n_in.y}, +half));
        flux_point out_outer =
            pt_add(pt, pt_scale(f->turning_left ? f->n_out : (flux_point){-f->n_out.x, -f->n_out.y},
                                +half));

        switch (paint->join) {
        case FLUX_JOIN_BEVEL:
            emit_tri(verts, &v_count, verts_cap, tx, color, pt, in_outer, out_outer);
            break;
        case FLUX_JOIN_MITER: {
            /* Miter — extend in_outer and out_outer to meeting point.
             * Direction of the bisector × outer-side gives the
             * meeting point at distance half * miter_factor. We
             * already computed miter overflow so this miter is
             * clamped to miter_limit. */
            flux_point bisector = pt_add(f->n_in, f->n_out);
            float bl = sqrtf(bisector.x * bisector.x + bisector.y * bisector.y);
            if (bl < 1e-6f) {
                emit_tri(verts, &v_count, verts_cap, tx, color, pt, in_outer, out_outer);
                break;
            }
            bisector = pt_scale(bisector, 1.0f / bl);
            if (!f->turning_left)
                bisector = pt_scale(bisector, -1.0f);
            /* miter cap distance = half * miter_limit along bisector. */
            flux_point miter = pt_add(pt, pt_scale(bisector, half * miter_limit));
            emit_tri(verts, &v_count, verts_cap, tx, color, pt, in_outer, miter);
            emit_tri(verts, &v_count, verts_cap, tx, color, pt, miter, out_outer);
            break;
        }
        case FLUX_JOIN_ROUND: {
            float a0 = atan2f(in_outer.y - pt.y, in_outer.x - pt.x);
            float a1 = atan2f(out_outer.y - pt.y, out_outer.x - pt.x);
            float sweep = a1 - a0;
            /* Keep sweep on the outside of the turn. */
            if (f->turning_left && sweep < 0.0f)
                sweep += 6.28318530718f;
            if (!f->turning_left && sweep > 0.0f)
                sweep -= 6.28318530718f;
            uint32_t steps = (uint32_t)(fabsf(sweep) * 8.0f); /* ~8 segments per radian */
            if (steps < 2)
                steps = 2;
            if (steps > 32)
                steps = 32;
            emit_arc(verts, &v_count, verts_cap, tx, color, pt, half, a0, sweep, steps);
            break;
        }
        }
    }

    /* Caps at the two endpoints of an unclosed polyline. */
    if (!closed && n >= 2) {
        const stroke_frame *f0 = &frames[0];
        const stroke_frame *fN = &frames[n - 1];
        flux_point p0 = pts[0];
        flux_point pN = pts[n - 1];

        switch (paint->cap) {
        case FLUX_CAP_BUTT:
            break;
        case FLUX_CAP_SQUARE: {
            /* Extend the body by half along the tangent (= -n_in rotated
             * back to tangent space). Tangent = (n.y, -n.x). */
            flux_point t0 = {f0->n_in.y, -f0->n_in.x}; /* +tangent at start */
            flux_point tN = {fN->n_in.y, -fN->n_in.x}; /* +tangent at end   */
            flux_point sl0 = pt_add(f0->left, pt_scale(t0, -half));
            flux_point sr0 = pt_add(f0->right, pt_scale(t0, -half));
            flux_point slN = pt_add(fN->left, pt_scale(tN, +half));
            flux_point srN = pt_add(fN->right, pt_scale(tN, +half));
            emit_tri(verts, &v_count, verts_cap, tx, color, sl0, sr0, f0->right);
            emit_tri(verts, &v_count, verts_cap, tx, color, sl0, f0->right, f0->left);
            emit_tri(verts, &v_count, verts_cap, tx, color, fN->left, fN->right, srN);
            emit_tri(verts, &v_count, verts_cap, tx, color, fN->left, srN, slN);
            break;
        }
        case FLUX_CAP_ROUND: {
            /* Half-disc fan centred on the endpoint, sweeping from
             * the right offset to the left offset around the OUTSIDE
             * of the cap (i.e. opposite the tangent direction). */
            float a_start = atan2f(f0->right.y - p0.y, f0->right.x - p0.x);
            emit_arc(verts, &v_count, verts_cap, tx, color, p0, half, a_start,
                     3.14159265359f /* π half-sweep, picks outside */, 16);
            float a_end = atan2f(fN->left.y - pN.y, fN->left.x - pN.x);
            emit_arc(verts, &v_count, verts_cap, tx, color, pN, half, a_end, 3.14159265359f, 16);
            break;
        }
        }
    }

    *v_count_io = v_count;
}

void flux_canvas_stroke_path(flux_canvas *c, const flux_path *p, const flux_paint *paint) {
    if (!c || !c->recording || !p || !paint)
        return;
    if (p->count == 0)
        return;
    if (paint->stroke_width <= 0.0f)
        return;

    flux_point *pts = c->scratch_pts;
    flux_canvas_contour *cons = c->scratch_contours;

    flux_mat3x2 tx = c->states[c->state_top].transform;
    float pixel_scale = flux_canvas_mat3x2_pixel_scale(tx);
    /* Flatten into every subpath (contour), not just the first — a glyph
     * path commonly holds several (outline + interior strokes). */
    flatten_multi fm = flatten_path_to_contours(p, pixel_scale, pts, FLUX_CANVAS_PATH_SCRATCH_CAP,
                                                cons, FLUX_CANVAS_MAX_CONTOURS);
    if (fm.contour_count == 0)
        return;

    flux_color color = paint->color;
    flux_canvas_vertex *verts = c->scratch_verts;
    const uint32_t verts_cap = FLUX_CANVAS_PATH_SCRATCH_CAP * 3;
    uint32_t v_count = 0;

    const float half = paint->stroke_width * 0.5f;
    const float miter_limit = paint->miter_limit > 0.0f ? paint->miter_limit : 4.0f;

    for (uint32_t ci = 0; ci < fm.contour_count; ++ci) {
        flux_canvas_contour *co = &cons[ci];
        if (co->count < 2)
            continue;
        stroke_one_contour(c, paint, tx, color, half, miter_limit, pts + co->start, co->count,
                           co->closed, co->first_x, co->first_y, verts, &v_count, verts_cap);
    }

    submit_triangles(c, paint, verts, v_count);
}
