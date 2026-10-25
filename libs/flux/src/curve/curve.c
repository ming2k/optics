/*
 * Analytic curve kernel (ADR-0115).
 *
 * Reduces Bézier input to line/quadratic segments with a bounded deviation,
 * offsets them analytically, and exposes the coverage primitives a
 * rasterizer needs. Pure math: no device, no backend, no allocation, no
 * global state. Analytic root finding runs in double where the closed form
 * is ill-conditioned; the public value types stay float.
 *
 * The reduction error bound used here is exact, not empirical. The
 * difference between a cubic B and the quadratic Q with control point
 * (3·P1 − P0 + 3·P2 − P3)/4 is
 *
 *     B(t) − Q(t) = (E/6) · 3t(1−t)(1−2t),   E = P3 − 3·P2 + 3·P1 − P0,
 *
 * so |B − Q| ≤ |E| · FLUX_CURVE_QUAD_REDUCTION_ERROR with
 * FLUX_CURVE_QUAD_REDUCTION_ERROR = 1/(6·2√3).
 */

#include <flux/curve.h>

#include <math.h>

/* ------------------------------------------------------------------ */
/*  Core error surface                                                 */
/*                                                                    */
/*  flux_set_last_error is declared by src/core/internal.h. It is       */
/*  re-declared here, verbatim and deliberately, so this translation    */
/*  unit does not drag core's Vulkan-bearing internal header into a     */
/*  pure-curve compilation (INV-CRV-02: the kernel has no device or     */
/*  backend dependency). The symbol lives in core/result.c and is       */
/*  always linked into libflux.                                         */
/* ------------------------------------------------------------------ */
void flux_set_last_error(flux_result code, const char *function, const char *file, int line,
                         const char *message, int32_t backend_code);

#define CURVE_FAIL(code, msg)                                                                      \
    do {                                                                                           \
        flux_set_last_error((code), __func__, __FILE__, __LINE__, (msg), 0);                       \
    } while (0)

/* Subdivision budget. A cubic reaches the reduction tolerance within a
 * handful of halvings for any sane tolerance; the cap turns "tolerance is
 * absurdly small" into an explicit error instead of unbounded recursion
 * (INV-CRV-03/04). */
#define CURVE_MAX_SUBDIV 24

/* Relative threshold below which a tangent is treated as vanishing. */
#define CURVE_EPS 1e-12
#define CURVE_TANGENT_EPS 1e-7

/* ================================================================== */
/*  Small helpers (double internally)                                 */
/* ================================================================== */

static inline double vx(flux_point p) {
    return (double)p.x;
}
static inline double vy(flux_point p) {
    return (double)p.y;
}

static inline double d_cross(double ax, double ay, double bx, double by) {
    return ax * by - ay * bx;
}

static inline flux_point pt_make(float x, float y) {
    flux_point p = {x, y};
    return p;
}

static inline flux_point pt_lerp(flux_point a, flux_point b, double t) {
    return pt_make((float)((1.0 - t) * vx(a) + t * vx(b)), (float)((1.0 - t) * vy(a) + t * vy(b)));
}

/* Euclidean length. A plain sqrt rather than hypot: every magnitude here is a
 * scene coordinate (its square stays far from overflow), and hypot's scaling
 * machinery costs several times more on the flattening hot path — this runs
 * once per sampled segment. */
static inline double curve_len(double x, double y) {
    return sqrt(x * x + y * y);
}

static inline double pt_len(flux_point p) {
    return curve_len((double)p.x, (double)p.y);
}

bool flux_curve_point_is_finite(flux_point p) {
    return isfinite(p.x) && isfinite(p.y);
}

bool flux_curve_seg_is_finite(flux_curve_seg s) {
    return (s.op == FLUX_CURVE_LINE || s.op == FLUX_CURVE_QUAD) &&
           flux_curve_point_is_finite(s.p0) && flux_curve_point_is_finite(s.p1) &&
           flux_curve_point_is_finite(s.p2);
}

flux_curve_seg flux_curve_seg_make(flux_curve_op op, flux_point p0, flux_point p1, flux_point p2) {
    flux_curve_seg s = {(uint32_t)op, p0, p1, p2};
    return s;
}

/* Real roots of A·t² + B·t + C = 0, ascending. Returns 0, 1 or 2. The
 * degenerate (A ≈ 0) case reduces to the linear root. */
static int solve_quadratic(double a, double b, double c, double out[2]) {
    if (fabs(a) <= CURVE_EPS) {
        if (fabs(b) <= CURVE_EPS)
            return 0;
        out[0] = -c / b;
        return 1;
    }
    double disc = b * b - 4.0 * a * c;
    if (disc < 0.0)
        return 0;
    double sq = sqrt(disc);
    /* Numerically stable: form the larger-magnitude root first. */
    double q = -0.5 * (b + (b >= 0.0 ? sq : -sq));
    double r0 = q / a;
    double r1 = (fabs(q) <= CURVE_EPS) ? r0 : c / q;
    if (r0 > r1) {
        double tmp = r0;
        r0 = r1;
        r1 = tmp;
    }
    out[0] = r0;
    out[1] = r1;
    return disc == 0.0 ? 1 : 2;
}

/* ================================================================== */
/*  Evaluation                                                        */
/* ================================================================== */

flux_point flux_curve_cubic_eval(flux_point p0, flux_point p1, flux_point p2, flux_point p3,
                                 float t) {
    double u = 1.0 - (double)t;
    double b0 = u * u * u;
    double b1 = 3.0 * u * u * (double)t;
    double b2 = 3.0 * u * (double)t * (double)t;
    double b3 = (double)t * (double)t * (double)t;
    return pt_make((float)(b0 * vx(p0) + b1 * vx(p1) + b2 * vx(p2) + b3 * vx(p3)),
                   (float)(b0 * vy(p0) + b1 * vy(p1) + b2 * vy(p2) + b3 * vy(p3)));
}

flux_point flux_curve_quad_eval(flux_curve_seg s, float t) {
    if (s.op == FLUX_CURVE_LINE)
        return pt_lerp(s.p0, s.p2, (double)t);
    double u = 1.0 - (double)t;
    double b0 = u * u;
    double b1 = 2.0 * u * (double)t;
    double b2 = (double)t * (double)t;
    return pt_make((float)(b0 * vx(s.p0) + b1 * vx(s.p1) + b2 * vx(s.p2)),
                   (float)(b0 * vy(s.p0) + b1 * vy(s.p1) + b2 * vy(s.p2)));
}

/* Un-normalised derivative at t. */
static flux_point quad_deriv(flux_curve_seg s, double t) {
    if (s.op == FLUX_CURVE_LINE)
        return pt_make((float)(vx(s.p2) - vx(s.p0)), (float)(vy(s.p2) - vy(s.p0)));
    double u = 1.0 - t;
    return pt_make((float)(2.0 * (u * (vx(s.p1) - vx(s.p0)) + t * (vx(s.p2) - vx(s.p1)))),
                   (float)(2.0 * (u * (vy(s.p1) - vy(s.p0)) + t * (vy(s.p2) - vy(s.p1)))));
}

flux_point flux_curve_quad_tangent(flux_curve_seg s, float t) {
    flux_point d = quad_deriv(s, (double)t);
    double len = pt_len(d);
    if (len <= CURVE_EPS)
        return pt_make(0.0f, 0.0f);
    return pt_make((float)(vx(d) / len), (float)(vy(d) / len));
}

flux_point flux_curve_quad_normal(flux_curve_seg s, float t) {
    flux_point tan = flux_curve_quad_tangent(s, t);
    /* Left normal: rotate the tangent +90°. */
    return pt_make(-tan.y, tan.x);
}

/* ================================================================== */
/*  Inflections and cusps                                             */
/* ================================================================== */

uint32_t flux_curve_cubic_inflections(flux_point p0, flux_point p1, flux_point p2, flux_point p3,
                                      float out_t[2]) {
    /* B'(t)/3 = u·t² + v·t + w ;  B''(t)/6 = u·t + n, with
     *   u = −P0 + 3P1 − 3P2 + P3
     *   v = 2(P0 − 2P1 + P2),  n = (P0 − 2P1 + P2) = v/2
     *   w = P1 − P0
     * Inflections are where cross(B', B'') vanishes, a quadratic in t:
     *   cross(B',B'')/18 = A2·t² + A1·t + A0. */
    double ux = -vx(p0) + 3.0 * vx(p1) - 3.0 * vx(p2) + vx(p3);
    double uy = -vy(p0) + 3.0 * vy(p1) - 3.0 * vy(p2) + vy(p3);
    double vxx = 2.0 * (vx(p0) - 2.0 * vx(p1) + vx(p2));
    double vyy = 2.0 * (vy(p0) - 2.0 * vy(p1) + vy(p2));
    double nx = vxx * 0.5;
    double ny = vyy * 0.5;
    double wx = vx(p1) - vx(p0);
    double wy = vy(p1) - vy(p0);

    double a2 = d_cross(ux, uy, nx, ny) + d_cross(vxx, vyy, ux, uy);
    double a1 = d_cross(vxx, vyy, nx, ny) + d_cross(wx, wy, ux, uy);
    double a0 = d_cross(wx, wy, nx, ny);

    double roots[2];
    int n = solve_quadratic(a2, a1, a0, roots);
    uint32_t count = 0;
    for (int i = 0; i < n && count < 2; ++i) {
        double t = roots[i];
        if (t > 0.0 && t < 1.0)
            out_t[count++] = (float)t;
    }
    return count;
}

bool flux_curve_cubic_cusp(flux_point p0, flux_point p1, flux_point p2, flux_point p3,
                           float *out_t) {
    double ux = -vx(p0) + 3.0 * vx(p1) - 3.0 * vx(p2) + vx(p3);
    double uy = -vy(p0) + 3.0 * vy(p1) - 3.0 * vy(p2) + vy(p3);
    double vxx = 2.0 * (vx(p0) - 2.0 * vx(p1) + vx(p2));
    double vyy = 2.0 * (vy(p0) - 2.0 * vy(p1) + vy(p2));
    double wx = vx(p1) - vx(p0);
    double wy = vy(p1) - vy(p0);

    /* The control polygon's span sets the "derivative is zero" tolerance. */
    double scale = pt_len(pt_make((float)(vx(p1) - vx(p0)), (float)(vy(p1) - vy(p0))));
    double s2 = pt_len(pt_make((float)(vx(p2) - vx(p1)), (float)(vy(p2) - vy(p1))));
    double s3 = pt_len(pt_make((float)(vx(p3) - vx(p2)), (float)(vy(p3) - vy(p2))));
    if (s2 > scale)
        scale = s2;
    if (s3 > scale)
        scale = s3;
    if (scale <= CURVE_EPS)
        return false; /* degenerate: a point, not a cusp */

    /* B'(t) = 0 is a vector equation. Solve it on the dominant component and
     * verify the other, so a root of one component that is not a
     * simultaneous root is rejected. */
    int use_x = fabs(ux) >= fabs(uy);
    double A = use_x ? ux : uy;
    double B = use_x ? vxx : vyy;
    double C = use_x ? wx : wy;
    double roots[2];
    int n = solve_quadratic(A, B, C, roots);
    double tol = CURVE_TANGENT_EPS * scale;
    for (int i = 0; i < n; ++i) {
        double t = roots[i];
        if (t <= 0.0 || t >= 1.0)
            continue;
        double ddx = 3.0 * (ux * t * t + vxx * t + wx);
        double ddy = 3.0 * (uy * t * t + vyy * t + wy);
        if (curve_len(ddx, ddy) <= tol) {
            if (out_t)
                *out_t = (float)t;
            return true;
        }
    }
    return false;
}

/* ================================================================== */
/*  Wang's formula                                                    */
/* ================================================================== */

uint32_t flux_curve_wang_cubic(flux_point p0, flux_point p1, flux_point p2, flux_point p3,
                               float tol) {
    if (!(tol > 0.0f) || !isfinite(tol))
        return 0;
    double l1 = curve_len(vx(p0) - 2.0 * vx(p1) + vx(p2), vy(p0) - 2.0 * vy(p1) + vy(p2));
    double l2 = curve_len(vx(p1) - 2.0 * vx(p2) + vx(p3), vy(p1) - 2.0 * vy(p2) + vy(p3));
    double l = l1 > l2 ? l1 : l2;
    if (l <= CURVE_EPS)
        return 1; /* control polygon is already straight */
    double n = sqrt((3.0 / (4.0 * (double)tol)) * l);
    if (!(n >= 1.0))
        return 1;
    if (n >= 4294967295.0)
        return 0xFFFFFFFFu;
    return (uint32_t)ceil(n);
}

uint32_t flux_curve_wang_quad(flux_point p0, flux_point p1, flux_point p2, float tol) {
    if (!(tol > 0.0f) || !isfinite(tol))
        return 0;
    double d = curve_len(vx(p0) - 2.0 * vx(p1) + vx(p2), vy(p0) - 2.0 * vy(p1) + vy(p2));
    if (d <= CURVE_EPS)
        return 1;
    double n = sqrt(d / (4.0 * (double)tol));
    if (!(n >= 1.0))
        return 1;
    if (n >= 4294967295.0)
        return 0xFFFFFFFFu;
    return (uint32_t)ceil(n);
}

/* ================================================================== */
/*  Reduction                                                         */
/* ================================================================== */

typedef struct cub4 {
    flux_point p0, p1, p2, p3;
} cub4;

static void cub_split(cub4 c, double t, cub4 *left, cub4 *right) {
    flux_point p01 = pt_lerp(c.p0, c.p1, t);
    flux_point p12 = pt_lerp(c.p1, c.p2, t);
    flux_point p23 = pt_lerp(c.p2, c.p3, t);
    flux_point p012 = pt_lerp(p01, p12, t);
    flux_point p123 = pt_lerp(p12, p23, t);
    flux_point mid = pt_lerp(p012, p123, t);
    *left = (cub4){c.p0, p01, p012, mid};
    *right = (cub4){mid, p123, p23, c.p3};
}

/* Reduce one cusp-free cubic to quads/lines within tol. Returns false when
 * the budget/cap is exhausted (never a partial write past *n). */
static bool reduce_piece(cub4 c, double tol, int depth, flux_curve_seg *out, uint32_t cap,
                         uint32_t *n) {
    double ex = vx(c.p3) - 3.0 * vx(c.p2) + 3.0 * vx(c.p1) - vx(c.p0);
    double ey = vy(c.p3) - 3.0 * vy(c.p2) + 3.0 * vy(c.p1) - vy(c.p0);
    if ((double)FLUX_CURVE_QUAD_REDUCTION_ERROR * curve_len(ex, ey) <= tol) {
        if (*n >= cap)
            return false;
        /* Quadratic through the endpoints with the error-minimising control
         * point. If that control point sits within tol of the chord, the
         * piece is a line and we say so (fewer segments downstream). */
        double q1x = (3.0 * vx(c.p1) - vx(c.p0) + 3.0 * vx(c.p2) - vx(c.p3)) * 0.25;
        double q1y = (3.0 * vy(c.p1) - vy(c.p0) + 3.0 * vy(c.p2) - vy(c.p3)) * 0.25;
        double chx = vx(c.p3) - vx(c.p0);
        double chy = vy(c.p3) - vy(c.p0);
        double chlen = curve_len(chx, chy);
        bool is_line = false;
        if (chlen > CURVE_EPS) {
            double perp = d_cross(chx, chy, q1x - vx(c.p0), q1y - vy(c.p0)) / chlen;
            is_line = fabs(perp) <= tol;
        }
        if (is_line) {
            out[*n] = flux_curve_seg_make(FLUX_CURVE_LINE, c.p0, pt_lerp(c.p0, c.p3, 0.5), c.p3);
        } else {
            out[*n] =
                flux_curve_seg_make(FLUX_CURVE_QUAD, c.p0, pt_make((float)q1x, (float)q1y), c.p3);
        }
        (*n)++;
        return true;
    }
    if (depth >= CURVE_MAX_SUBDIV)
        return false;
    cub4 l, r;
    cub_split(c, 0.5, &l, &r);
    return reduce_piece(l, tol, depth + 1, out, cap, n) &&
           reduce_piece(r, tol, depth + 1, out, cap, n);
}

flux_result flux_curve_cubic_reduce(flux_point p0, flux_point p1, flux_point p2, flux_point p3,
                                    float tol, flux_curve_seg *out, uint32_t cap,
                                    uint32_t *out_count) {
    if (out_count)
        *out_count = 0;
    if (!out || cap == 0) {
        CURVE_FAIL(FLUX_ERROR_INVALID_ARGUMENT, "flux_curve_cubic_reduce: no output storage");
        return FLUX_ERROR_INVALID_ARGUMENT;
    }
    if (!(tol > 0.0f) || !isfinite(tol)) {
        CURVE_FAIL(FLUX_ERROR_INVALID_ARGUMENT, "flux_curve_cubic_reduce: tol must be finite > 0");
        return FLUX_ERROR_INVALID_ARGUMENT;
    }
    if (!flux_curve_point_is_finite(p0) || !flux_curve_point_is_finite(p1) ||
        !flux_curve_point_is_finite(p2) || !flux_curve_point_is_finite(p3)) {
        CURVE_FAIL(FLUX_ERROR_INVALID_ARGUMENT,
                   "flux_curve_cubic_reduce: non-finite control point");
        return FLUX_ERROR_INVALID_ARGUMENT;
    }

    /* Split parameters: inflections first (a quadratic cannot turn), then a
     * cusp (a quadratic cannot reverse). Sorted, de-duplicated, endpoints
     * dropped. */
    double cuts[3];
    uint32_t ncuts = 0;
    float inf[2];
    uint32_t ninf = flux_curve_cubic_inflections(p0, p1, p2, p3, inf);
    for (uint32_t i = 0; i < ninf; ++i)
        cuts[ncuts++] = (double)inf[i];
    float cusp_t;
    if (flux_curve_cubic_cusp(p0, p1, p2, p3, &cusp_t))
        cuts[ncuts++] = (double)cusp_t;
    for (uint32_t i = 0; i + 1 < ncuts; ++i)
        for (uint32_t j = i + 1; j < ncuts; ++j)
            if (cuts[j] < cuts[i]) {
                double t = cuts[i];
                cuts[i] = cuts[j];
                cuts[j] = t;
            }
    uint32_t kept = 0;
    for (uint32_t i = 0; i < ncuts; ++i) {
        if (cuts[i] <= 1e-6 || cuts[i] >= 1.0 - 1e-6)
            continue;
        if (kept > 0 && fabs(cuts[i] - cuts[kept - 1]) <= 1e-6)
            continue;
        cuts[kept++] = cuts[i];
    }
    ncuts = kept;

    cub4 cur = {p0, p1, p2, p3};
    double prev = 0.0;
    uint32_t count = 0;
    for (uint32_t i = 0; i <= ncuts; ++i) {
        cub4 piece;
        if (i < ncuts) {
            double t = (cuts[i] - prev) / (1.0 - prev);
            cub4 l, r;
            cub_split(cur, t, &l, &r);
            piece = l;
            cur = r;
            prev = cuts[i];
        } else {
            piece = cur;
        }
        if (!reduce_piece(piece, (double)tol, 0, out, cap, &count)) {
            if (out_count)
                *out_count = 0;
            CURVE_FAIL(FLUX_ERROR_OUT_OF_RANGE,
                       "flux_curve_cubic_reduce: tolerance not met within cap/budget");
            return FLUX_ERROR_OUT_OF_RANGE;
        }
    }
    if (out_count)
        *out_count = count;
    return FLUX_OK;
}

/* ================================================================== */
/*  Offset                                                            */
/* ================================================================== */

flux_result flux_curve_seg_offset(flux_curve_seg s, float dist, flux_curve_seg *out) {
    if (!out || !flux_curve_seg_is_finite(s) || !isfinite(dist)) {
        CURVE_FAIL(FLUX_ERROR_INVALID_ARGUMENT, "flux_curve_seg_offset: bad segment or distance");
        return FLUX_ERROR_INVALID_ARGUMENT;
    }
    flux_point n0 = flux_curve_quad_normal(s, 0.0f);
    flux_point n2 = flux_curve_quad_normal(s, 1.0f);
    if ((n0.x == 0.0f && n0.y == 0.0f) || (n2.x == 0.0f && n2.y == 0.0f)) {
        CURVE_FAIL(FLUX_ERROR_INVALID_STATE,
                   "flux_curve_seg_offset: tangent vanishes at an endpoint (cusp)");
        return FLUX_ERROR_INVALID_STATE;
    }

    flux_point q0 = pt_make(s.p0.x + dist * n0.x, s.p0.y + dist * n0.y);
    flux_point q2 = pt_make(s.p2.x + dist * n2.x, s.p2.y + dist * n2.y);

    if (s.op == FLUX_CURVE_LINE) {
        *out = flux_curve_seg_make(FLUX_CURVE_LINE, q0, pt_lerp(q0, q2, 0.5), q2);
        return FLUX_OK;
    }

    flux_point nm = flux_curve_quad_normal(s, 0.5f);
    if (nm.x == 0.0f && nm.y == 0.0f) {
        CURVE_FAIL(FLUX_ERROR_INVALID_STATE,
                   "flux_curve_seg_offset: tangent vanishes at the midpoint");
        return FLUX_ERROR_INVALID_STATE;
    }
    flux_point mid = flux_curve_quad_eval(s, 0.5f);
    flux_point m = pt_make(mid.x + dist * nm.x, mid.y + dist * nm.y);
    /* The unique quadratic through q0 and q2 that passes through m at
     * t = 1/2. Exact for a line; bounded for a curved piece. */
    flux_point q1 = pt_make((float)(2.0 * vx(m) - 0.5 * (vx(q0) + vx(q2))),
                            (float)(2.0 * vy(m) - 0.5 * (vy(q0) + vy(q2))));
    *out = flux_curve_seg_make(FLUX_CURVE_QUAD, q0, q1, q2);
    return FLUX_OK;
}

flux_result flux_curve_seg_offset_retrograde(flux_curve_seg s, float dist, float *t0, float *t1,
                                             bool *has) {
    if (!t0 || !t1 || !has) {
        CURVE_FAIL(FLUX_ERROR_INVALID_ARGUMENT,
                   "flux_curve_seg_offset_retrograde: null out-parameter");
        return FLUX_ERROR_INVALID_ARGUMENT;
    }
    if (!flux_curve_seg_is_finite(s) || !isfinite(dist)) {
        CURVE_FAIL(FLUX_ERROR_INVALID_ARGUMENT,
                   "flux_curve_seg_offset_retrograde: non-finite segment or distance");
        return FLUX_ERROR_INVALID_ARGUMENT;
    }
    *t0 = 0.0f;
    *t1 = 0.0f;
    *has = false;
    if (s.op != FLUX_CURVE_QUAD)
        return FLUX_OK; /* a line has zero curvature: it never retrogrades */

    /* B'(t) = 2(U + t·A) with U = P1 − P0, A = P0 − 2P1 + P2, so
     * |B'(t)|² = 4·S(t), S(t) = |A|²·t² + 2(U·A)·t + |U|², and
     * cross(B', B'') = 4·cross(U, A) is *constant*. Hence
     *   κ(t) = cross(U, A) / (2·S(t)^(3/2)),
     * which is signed: positive when the curve turns toward its left normal.
     * Offsetting left (dist > 0) moves toward the centre of curvature, so the
     * offset retrogrades where dist·κ(t) > 1 — that is, where S(t) ≤ R with
     * R = (dist·cross(U,A) / 2)^(2/3). S opens upward, so the retrograde set
     * is the interval between its two roots. */
    double ax = vx(s.p0) - 2.0 * vx(s.p1) + vx(s.p2);
    double ay = vy(s.p0) - 2.0 * vy(s.p1) + vy(s.p2);
    double ux = vx(s.p1) - vx(s.p0);
    double uy = vy(s.p1) - vy(s.p0);
    double m = (double)dist * d_cross(ux, uy, ax, ay);
    if (!(m > 0.0))
        return FLUX_OK; /* offsetting away from the centre: no retrograde */

    double a = ax * ax + ay * ay;
    double b = 2.0 * (ux * ax + uy * ay);
    double c = ux * ux + uy * uy;
    if (a <= CURVE_EPS)
        return FLUX_OK; /* second difference vanishes: effectively a line */
    double r = pow(m * 0.5, 2.0 / 3.0);

    double roots[2];
    int n = solve_quadratic(a, b, c - r, roots);
    if (n < 2)
        return FLUX_OK; /* no interval, or a measure-zero tangency */

    double lo = roots[0];
    double hi = roots[1];
    if (hi <= 0.0 || lo >= 1.0)
        return FLUX_OK; /* the retrograde set lies outside the segment */
    if (lo < 0.0)
        lo = 0.0;
    if (hi > 1.0)
        hi = 1.0;
    if (!(hi > lo))
        return FLUX_OK;
    *t0 = (float)lo;
    *t1 = (float)hi;
    *has = true;
    return FLUX_OK;
}

/* ================================================================== */
/*  Bounds                                                            */
/* ================================================================== */
flux_rect flux_curve_seg_bounds(flux_curve_seg s) {
    float minx = s.p0.x, maxx = s.p0.x, miny = s.p0.y, maxy = s.p0.y;
    const flux_point pts[3] = {s.p0, s.p1, s.p2};
    for (int i = 1; i < 3; ++i) {
        if (pts[i].x < minx)
            minx = pts[i].x;
        if (pts[i].x > maxx)
            maxx = pts[i].x;
        if (pts[i].y < miny)
            miny = pts[i].y;
        if (pts[i].y > maxy)
            maxy = pts[i].y;
    }
    flux_rect r = {minx, miny, maxx - minx, maxy - miny};
    return r;
}

flux_rect flux_curve_cover_bounds(const flux_curve_seg *segs, uint32_t count) {
    if (!segs || count == 0) {
        flux_rect empty = {0.0f, 0.0f, 0.0f, 0.0f};
        return empty;
    }
    flux_rect r = flux_curve_seg_bounds(segs[0]);
    float minx = r.x, miny = r.y, maxx = r.x + r.w, maxy = r.y + r.h;
    for (uint32_t i = 1; i < count; ++i) {
        flux_rect b = flux_curve_seg_bounds(segs[i]);
        if (b.x < minx)
            minx = b.x;
        if (b.y < miny)
            miny = b.y;
        if (b.x + b.w > maxx)
            maxx = b.x + b.w;
        if (b.y + b.h > maxy)
            maxy = b.y + b.h;
    }
    flux_rect out = {minx, miny, maxx - minx, maxy - miny};
    return out;
}

/* ================================================================== */
/*  Ray / winding                                                     */
/* ================================================================== */

/* Parameters where the segment's y equals yv, over [0,1] (closed) or [0,1)
 * (half-open). Half-open is what winding needs: a vertex shared by two
 * consecutive segments is counted exactly once. */
static int seg_y_roots(flux_curve_seg s, double yv, bool half_open, double out_t[2]) {
    if (s.op == FLUX_CURVE_LINE) {
        double y0 = vy(s.p0), y2 = vy(s.p2);
        if (y0 == y2)
            return 0;
        double t = (yv - y0) / (y2 - y0);
        if (t < 0.0 || t > 1.0)
            return 0;
        if (half_open && t >= 1.0)
            return 0;
        out_t[0] = t;
        return 1;
    }
    double a = vy(s.p0) - 2.0 * vy(s.p1) + vy(s.p2);
    double b = 2.0 * (vy(s.p1) - vy(s.p0));
    double c = vy(s.p0) - yv;
    double roots[2];
    int n = solve_quadratic(a, b, c, roots);
    int count = 0;
    for (int i = 0; i < n && count < 2; ++i) {
        double t = roots[i];
        if (t < 0.0 || t > 1.0)
            continue;
        if (half_open && t >= 1.0)
            continue;
        out_t[count++] = t;
    }
    return count;
}

uint32_t flux_curve_seg_ray_x(flux_curve_seg s, float yv, float out_x[2]) {
    if (!out_x || !flux_curve_seg_is_finite(s) || !isfinite(yv))
        return 0;
    double t[2];
    int n = seg_y_roots(s, (double)yv, false, t);
    for (int i = 0; i < n; ++i) {
        flux_point p = flux_curve_quad_eval(s, (float)t[i]);
        out_x[i] = p.x;
    }
    if (n == 2 && out_x[1] < out_x[0]) {
        float tmp = out_x[0];
        out_x[0] = out_x[1];
        out_x[1] = tmp;
    }
    return (uint32_t)n;
}

int32_t flux_curve_cover_winding(flux_curve_cover cover, float x, float y) {
    if (!cover.segs || cover.count == 0)
        return 0;
    double xd = (double)x, yd = (double)y;
    int32_t w = 0;
    for (uint32_t i = 0; i < cover.count; ++i) {
        flux_curve_seg s = cover.segs[i];
        double t[2];
        int n = seg_y_roots(s, yd, true, t);
        for (int k = 0; k < n; ++k) {
            flux_point d = quad_deriv(s, t[k]);
            if (fabs((double)d.y) <= CURVE_EPS)
                continue; /* tangent to the ray: no transversal crossing */
            flux_point p = flux_curve_quad_eval(s, (float)t[k]);
            if ((double)p.x > xd)
                w += (d.y > 0.0f) ? 1 : -1;
        }
    }
    return w;
}

bool flux_curve_cover_contains(flux_curve_cover cover, float x, float y, flux_curve_fill rule) {
    int32_t w = flux_curve_cover_winding(cover, x, y);
    if (rule == FLUX_CURVE_FILL_EVENODD)
        return (w & 1) != 0;
    return w != 0;
}

float flux_curve_cover_coverage(flux_curve_cover cover, flux_curve_fill rule, float x, float y,
                                uint32_t n) {
    if (!cover.segs || cover.count == 0 || n == 0)
        return 0.0f;
    if (n > 64)
        n = 64;
    /* Winding is O(count) per sample, so a 64x64 grid over a long strip is
     * expensive; this is a reference, not a hot path. */
    uint32_t inside = 0;
    const float step = 1.0f / (float)n;
    const float half = step * 0.5f;
    for (uint32_t j = 0; j < n; ++j) {
        float sy = y + (float)j * step + half;
        for (uint32_t i = 0; i < n; ++i) {
            float sx = x + (float)i * step + half;
            int32_t w = flux_curve_cover_winding(cover, sx, sy);
            bool in = (rule == FLUX_CURVE_FILL_EVENODD) ? ((w & 1) != 0) : (w != 0);
            if (in)
                inside++;
        }
    }
    return (float)inside / (float)(n * n);
}
