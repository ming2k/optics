#include "backend.h"
#include "internal.h"

#include <math.h>

/* ------------------------------------------------------------------ */
/*  Stroker                                                           */
/*                                                                    */
/*  Polyline -> stroke outline as a set of pieces -> analytic         */
/*  coverage. Per-vertex offset frames come from the bisector of the  */
/*  adjacent segment normals (offset half-width times the miter       */
/*  factor 1/cos(theta/2)); a vertex whose factor exceeds the miter   */
/*  limit splits into two offsets and gets join geometry (bevel,      */
/*  clamped miter or round fan). Open ends get caps.                  */
/*                                                                    */
/*  Each piece -- segment body, join, cap -- is one closed contour    */
/*  oriented to positive area. The non-zero union of positively       */
/*  oriented simple polygons is exactly their union, so pieces that   */
/*  overlap at a join cover each pixel once and a translucent stroke  */
/*  never double-blends. The pieces go to the same submit_cover the   */
/*  fills use, in physical pixels.                                    */
/* ------------------------------------------------------------------ */

/* Per-vertex offset frame derived from incoming + outgoing tangents. */
stroke_frame compute_frame(optics_gfx_point prev, optics_gfx_point pt, optics_gfx_point next,
                           float half, float miter_limit, bool has_prev, bool has_next) {
    stroke_frame f = {};
    optics_gfx_point d_in = has_prev ? pt_sub(pt, prev) : (optics_gfx_point){0, 0};
    optics_gfx_point d_out = has_next ? pt_sub(next, pt) : (optics_gfx_point){0, 0};
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
    f.n_in = (optics_gfx_point){-d_in.y, d_in.x};
    f.n_out = (optics_gfx_point){-d_out.y, d_out.x};

    optics_gfx_point bisector_sum = pt_add(f.n_in, f.n_out);
    float bis_len = sqrtf(bisector_sum.x * bisector_sum.x + bisector_sum.y * bisector_sum.y);

    /* Turn direction: d_in x d_out > 0 turns toward the left normals, so the
     * left side is the inside of the turn (clamped below) and the right side
     * the outside (where the join goes). */
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

    optics_gfx_point bisector = pt_scale(bisector_sum, 1.0f / bis_len);
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

/* Largest piece the stroker builds: a 32-step round join or a 16-step round
 * cap fan plus its centre. */
#define STROKE_PIECE_MAX 40u

/* Outline being accumulated for one stroke call. */
typedef struct stroke_outline {
    optics_gfx_canvas *c;
    optics_gfx_mat3x2 tx; /* user space -> physical pixels */
    uint32_t pt_count;
    uint32_t contour_count;
    bool failed; /* scratch growth failed; the canvas error is already set */
} stroke_outline;

static float polygon_area2(const optics_gfx_point *p, uint32_t n) {
    float a = 0.0f;
    for (uint32_t i = 0; i < n; ++i) {
        const optics_gfx_point u = p[i];
        const optics_gfx_point v = p[(i + 1) % n];
        a += u.x * v.y - v.x * u.y;
    }
    return a;
}

/* Convex, allowing collinear runs (a round cap's centre lies on its diameter). */
static bool polygon_convex(const optics_gfx_point *p, uint32_t n, float area2) {
    const float tol = -1e-6f * fabsf(area2);
    for (uint32_t i = 0; i < n; ++i) {
        const optics_gfx_point a = p[i];
        const optics_gfx_point b = p[(i + 1) % n];
        const optics_gfx_point e = p[(i + 2) % n];
        float turn = (b.x - a.x) * (e.y - b.y) - (b.y - a.y) * (e.x - b.x);
        if (area2 < 0.0f)
            turn = -turn;
        if (turn < tol)
            return false;
    }
    return true;
}

/* Append `p[0..n)` (physical pixels) as one positively oriented contour. */
static void outline_push(stroke_outline *o, const optics_gfx_point *p, uint32_t n, bool reverse) {
    optics_gfx_canvas *c = o->c;
    if (!canvas_ensure_outline(c, o->pt_count + n, o->contour_count + 1u)) {
        o->failed = true;
        return;
    }
    const uint32_t start = o->pt_count;
    for (uint32_t i = 0; i < n; ++i)
        c->scratch_outline_pts[start + i] = p[reverse ? n - 1u - i : i];
    c->scratch_outline_contours[o->contour_count++] =
        (optics_gfx_canvas_contour){.start = start, .count = n, .closed = true};
    o->pt_count += n;
}

/* Add one outline piece given in user space. A convex piece becomes a single
 * contour. A non-convex one -- only a body quad whose inner offsets crossed
 * can be -- would wind its two lobes in opposite directions and cut a hole
 * under the non-zero rule, so it is split into a fan of triangles, each
 * oriented on its own. Degenerate pieces add nothing. */
static void stroke_piece(stroke_outline *o, const optics_gfx_point *user, uint32_t n) {
    if (o->failed || n < 3 || n > STROKE_PIECE_MAX)
        return;
    optics_gfx_point p[STROKE_PIECE_MAX];
    for (uint32_t i = 0; i < n; ++i)
        p[i] = optics_gfx_mat3x2_transform_point(o->tx, user[i]);
    const float area2 = polygon_area2(p, n);
    if (fabsf(area2) < 1e-9f)
        return;
    if (polygon_convex(p, n, area2)) {
        outline_push(o, p, n, area2 < 0.0f);
        return;
    }
    for (uint32_t i = 1; i + 1 < n; ++i) {
        const optics_gfx_point tri[3] = {p[0], p[i], p[i + 1]};
        const float tri_area2 = polygon_area2(tri, 3);
        if (fabsf(tri_area2) >= 1e-9f)
            outline_push(o, tri, 3, tri_area2 < 0.0f);
    }
}

static void stroke_tri(stroke_outline *o, optics_gfx_point a, optics_gfx_point b,
                       optics_gfx_point e) {
    stroke_piece(o, (const optics_gfx_point[]){a, b, e}, 3);
}

static void stroke_quad(stroke_outline *o, optics_gfx_point a, optics_gfx_point b,
                        optics_gfx_point e, optics_gfx_point f) {
    stroke_piece(o, (const optics_gfx_point[]){a, b, e, f}, 4);
}

/* Circular sector centred on `centre` sweeping `sweep` radians from
 * `start_angle`: the fan of a round join or a round cap. |sweep| <= pi, so
 * the sector is convex and lands as one contour. */
static void stroke_sector(stroke_outline *o, optics_gfx_point centre, float radius,
                          float start_angle, float sweep, uint32_t steps) {
    if (steps == 0 || steps + 2u > STROKE_PIECE_MAX)
        return;
    optics_gfx_point p[STROKE_PIECE_MAX];
    p[0] = centre;
    const float step = sweep / (float)steps;
    for (uint32_t i = 0; i <= steps; ++i) {
        const float a = start_angle + step * (float)i;
        p[i + 1] = (optics_gfx_point){centre.x + radius * cosf(a), centre.y + radius * sinf(a)};
    }
    stroke_piece(o, p, steps + 2u);
}

/* Local radius of curvature of the flattened polyline at `pt`, estimated as
 * arc length over turn angle, and the turn direction. Used only when the
 * source curve's exact curvature is unavailable (a straight source segment, or
 * a corner where no single radius applies). Returns a large value when the
 * polyline is straight or degenerate, so the caller's comparison never fires.
 *
 * The exact value comes from the analytic kernel (ADR-0115) and is preferred:
 * arc-length-over-turn-angle under-estimates R by sin(x)/x and is biased by
 * the flattening density. */
static float vertex_radius(optics_gfx_point prev, optics_gfx_point pt, optics_gfx_point next,
                           bool *turning_left) {
    optics_gfx_point d_in = pt_sub(pt, prev);
    optics_gfx_point d_out = pt_sub(next, pt);
    float l_in = sqrtf(d_in.x * d_in.x + d_in.y * d_in.y);
    float l_out = sqrtf(d_out.x * d_out.x + d_out.y * d_out.y);
    if (l_in < 1e-6f || l_out < 1e-6f)
        return 1e30f;
    float cross = d_in.x * d_out.y - d_in.y * d_out.x;
    if (turning_left)
        *turning_left = cross > 0.0f;
    float turn_abs = fabsf(atan2f(cross, d_in.x * d_out.x + d_in.y * d_out.y));
    if (turn_abs < 1e-6f)
        return 1e30f;
    return 0.5f * (l_in + l_out) / turn_abs;
}

/* Clamp the inner-side offset to the local radius of curvature. When the
 * half-width exceeds it the true inner offset passes through the centre of
 * curvature and turns back on itself; emitting it anyway makes the band
 * overlap itself, which is invisible for opaque paint but double-blends a
 * translucent one. Clamping collapses the inner boundary to the centre (for a
 * circular arc) so the band tiles its region exactly once. The outer side is
 * left alone: its offset grows with the mitre factor, which is correct.
 *
 * The limit is the POLYLINE's own discrete radius, not the source curve's
 * exact curvature. That is deliberate and measured (ADR-0115): the band being
 * emitted is a polyline band, so the limit must be where *its* inner edges
 * converge. Substituting the analytic radius makes the clamp miss the collapse
 * — a cubic approximating a small circle has slightly larger curvature radius
 * than the circle, and the r=2 retrograde case regressed from 0 to 28 units of
 * double blending when the exact value was used. Enforcing both limits
 * (min of the two) reproduces the polyline result exactly, so the analytic
 * radius buys nothing here.
 */
static void clamp_inner_offset(stroke_frame *f, optics_gfx_point pt, optics_gfx_point prev,
                               optics_gfx_point next) {
    optics_gfx_point inner = f->turning_left ? f->left : f->right;
    optics_gfx_point dir = pt_sub(inner, pt);
    float len = sqrtf(dir.x * dir.x + dir.y * dir.y);
    if (len <= 1e-6f)
        return;
    float radius = vertex_radius(prev, pt, next, nullptr);
    if (len <= radius)
        return;
    optics_gfx_point clamped = pt_add(pt, pt_scale(dir, radius / len));
    if (f->turning_left)
        f->left = clamped;
    else
        f->right = clamped;
}

/* Add the outline pieces of one flattened subpath (`pts[0..count)`). Every
 * subpath of a path is stroked, not only the first. */
static void stroke_one_contour(stroke_outline *o, const canvas_paint *paint, float half,
                               float miter_limit, optics_gfx_point *pts, uint32_t count,
                               bool contour_closed, float first_x, float first_y) {
    optics_gfx_canvas *c = o->c;
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

    /* Build per-vertex frames first so segment body can reference
     * both endpoints' offsets, growing c->scratch_frames dynamically. */
    if (!canvas_ensure_frames(c, n))
        return;
    stroke_frame *frames = c->scratch_frames;
    for (uint32_t i = 0; i < n; ++i) {
        bool has_prev = closed ? true : (i > 0);
        bool has_next = closed ? true : (i + 1 < n);
        optics_gfx_point prev = pts[has_prev ? (i == 0 ? n - 1 : i - 1) : 0];
        optics_gfx_point next = pts[has_next ? ((i + 1) % n) : 0];
        frames[i] = compute_frame(prev, pts[i], next, half, miter_limit, has_prev, has_next);
        if (has_prev && has_next)
            clamp_inner_offset(&frames[i], pts[i], prev, next);
    }

    /* Body: one quad per segment. A mitred vertex shares its bisector offsets
     * between the two segments. A vertex whose miter overflowed has none: the
     * segment ending there stops at its own normal (n_in) and the one leaving
     * starts at its own (n_out); the join fills the outer wedge between them
     * and the inner sides overlap, which the union covers once. */
    uint32_t seg_count = closed ? n : (n > 0 ? n - 1 : 0);
    for (uint32_t s = 0; s < seg_count; ++s) {
        const uint32_t ia = s;
        const uint32_t ib = (s + 1) % n;
        const stroke_frame *a = &frames[ia];
        const stroke_frame *b = &frames[ib];
        const optics_gfx_point a_left =
            a->miter_ok ? a->left : pt_add(pts[ia], pt_scale(a->n_out, +half));
        const optics_gfx_point a_right =
            a->miter_ok ? a->right : pt_add(pts[ia], pt_scale(a->n_out, -half));
        const optics_gfx_point b_left =
            b->miter_ok ? b->left : pt_add(pts[ib], pt_scale(b->n_in, +half));
        const optics_gfx_point b_right =
            b->miter_ok ? b->right : pt_add(pts[ib], pt_scale(b->n_in, -half));
        stroke_quad(o, a_right, a_left, b_left, b_right);
    }

    /* Joins at interior vertices where miter overflowed. */
    uint32_t join_first = closed ? 0 : 1;
    uint32_t join_last = closed ? n : (n > 1 ? n - 1 : 0);
    for (uint32_t i = join_first; i < join_last; ++i) {
        const stroke_frame *f = &frames[i];
        if (f->miter_ok)
            continue;

        /* The join fills the outer side of the turn: away from the left
         * normals when turning left, along them when turning right. */
        optics_gfx_point pt = pts[i];
        const float outer = f->turning_left ? -half : +half;
        optics_gfx_point in_outer = pt_add(pt, pt_scale(f->n_in, outer));
        optics_gfx_point out_outer = pt_add(pt, pt_scale(f->n_out, outer));

        switch (paint->join) {
        case OPTICS_GFX_JOIN_BEVEL:
            stroke_tri(o, pt, in_outer, out_outer);
            break;
        case OPTICS_GFX_JOIN_MITER: {
            /* Miter — extend in_outer and out_outer to meeting point.
             * Direction of the bisector × outer-side gives the
             * meeting point at distance half * miter_factor. We
             * already computed miter overflow so this miter is
             * clamped to miter_limit. */
            optics_gfx_point bisector = pt_add(f->n_in, f->n_out);
            float bl = sqrtf(bisector.x * bisector.x + bisector.y * bisector.y);
            if (bl < 1e-6f) {
                stroke_tri(o, pt, in_outer, out_outer);
                break;
            }
            bisector = pt_scale(bisector, 1.0f / bl);
            if (f->turning_left)
                bisector = pt_scale(bisector, -1.0f);
            /* miter cap distance = half * miter_limit along bisector. */
            optics_gfx_point miter = pt_add(pt, pt_scale(bisector, half * miter_limit));
            stroke_quad(o, pt, in_outer, miter, out_outer);
            break;
        }
        case OPTICS_GFX_JOIN_ROUND: {
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
            stroke_sector(o, pt, half, a0, sweep, steps);
            break;
        }
        }
    }

    /* Caps at the two endpoints of an unclosed polyline. */
    if (!closed && n >= 2) {
        const stroke_frame *f0 = &frames[0];
        const stroke_frame *fN = &frames[n - 1];
        optics_gfx_point p0 = pts[0];
        optics_gfx_point pN = pts[n - 1];

        switch (paint->cap) {
        case OPTICS_GFX_CAP_BUTT:
            break;
        case OPTICS_GFX_CAP_SQUARE: {
            /* Extend the body by half along the tangent (= -n_in rotated
             * back to tangent space). Tangent = (n.y, -n.x). */
            optics_gfx_point t0 = {f0->n_in.y, -f0->n_in.x}; /* +tangent at start */
            optics_gfx_point tN = {fN->n_in.y, -fN->n_in.x}; /* +tangent at end   */
            optics_gfx_point sl0 = pt_add(f0->left, pt_scale(t0, -half));
            optics_gfx_point sr0 = pt_add(f0->right, pt_scale(t0, -half));
            optics_gfx_point slN = pt_add(fN->left, pt_scale(tN, +half));
            optics_gfx_point srN = pt_add(fN->right, pt_scale(tN, +half));
            stroke_quad(o, sl0, sr0, f0->right, f0->left);
            stroke_quad(o, fN->left, fN->right, srN, slN);
            break;
        }
        case OPTICS_GFX_CAP_ROUND: {
            /* Half-disc fan centred on the endpoint, sweeping from
             * the right offset to the left offset around the OUTSIDE
             * of the cap (i.e. opposite the tangent direction). */
            float a_start = atan2f(f0->right.y - p0.y, f0->right.x - p0.x);
            stroke_sector(o, p0, half, a_start, 3.14159265359f /* outside half-turn */, 16);
            float a_end = atan2f(fN->left.y - pN.y, fN->left.x - pN.x);
            stroke_sector(o, pN, half, a_end, 3.14159265359f, 16);
            break;
        }
        }
    }
}

void canvas_stroke_path_internal(optics_gfx_canvas *c, const optics_gfx_path *p,
                                 const canvas_paint *paint) {
    if (!c || !c->recording || !p || !paint)
        return;
    if (p->count == 0)
        return;
    if (paint->stroke_width <= 0.0f)
        return;
    if (!c->backend || !c->backend->submit_cover) {
        canvas_fail(c, OPTICS_GFX_ERROR_UNSUPPORTED,
                    "analytic stroke coverage unsupported by canvas backend");
        return;
    }

    optics_gfx_mat3x2 tx = c->states[c->state_top].transform;
    float pixel_scale = optics_gfx_canvas_mat3x2_pixel_scale(tx);

    const float half = paint->stroke_width * 0.5f;
    const float miter_limit = paint->miter_limit > 0.0f ? paint->miter_limit : 4.0f;

    /* Flatten every subpath into c->scratch_pts / c->scratch_contours (grown
     * as needed); the outline goes to its own scratch so the two never alias. */
    flatten_multi fm = canvas_flatten_path(c, p, pixel_scale);
    if (fm.contour_count == 0)
        return;

    stroke_outline o = {.c = c, .tx = tx};
    for (uint32_t ci = 0; ci < fm.contour_count && !o.failed; ++ci) {
        const optics_gfx_canvas_contour *co = &c->scratch_contours[ci];
        if (co->count < 2)
            continue;
        stroke_one_contour(&o, paint, half, miter_limit, c->scratch_pts + co->start, co->count,
                           co->closed, co->first_x, co->first_y);
    }
    if (o.failed || o.contour_count == 0)
        return;

    optics_gfx_result r = c->backend->submit_cover(
        c->backend, c, c->scratch_outline_pts, c->scratch_outline_contours, o.contour_count,
        OPTICS_GFX_FILL_NON_ZERO, paint, c->states[c->state_top].scissor);
    if (r != OPTICS_GFX_OK)
        canvas_fail(c, r, "analytic stroke submission failed");
}
