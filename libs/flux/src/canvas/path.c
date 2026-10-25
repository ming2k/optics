/*
 * Path builder. Arena-allocated, dynamically growing.
 * Curves stored as control points; flattening happens at fill time.
 */
#include "internal.h"

#include <math.h>
#include <string.h>

#define FLUX_PATH_INITIAL_CAP 64

static bool path_grow(flux_path *p, flux_arena *arena) {
    uint32_t new_cap = p->capacity ? p->capacity * 2 : FLUX_PATH_INITIAL_CAP;
    flux_path_segment *new_seg = flux_arena_alloc_aligned(
        arena, new_cap * sizeof(flux_path_segment), alignof(flux_path_segment));
    if (!new_seg)
        return false;
    if (p->count > 0)
        memcpy(new_seg, p->segments, p->count * sizeof(flux_path_segment));
    p->segments = new_seg;
    p->capacity = new_cap;
    return true;
}

static void push_segment(flux_path *p, flux_arena *arena, uint32_t op, const float *pts,
                         uint32_t n) {
    if (!p)
        return;
    if (p->count >= p->capacity) {
        if (!path_grow(p, arena)) {
            if (p->dropped == 0) {
                FLUX_FAIL(FLUX_ERROR_OUT_OF_MEMORY, "flux_path arena exhausted; segments dropped");
            }
            p->dropped++;
            return;
        }
    }
    flux_path_segment *s = &p->segments[p->count++];
    s->op = op;
    memset(s->pts, 0, sizeof(s->pts));
    if (pts && n > 0)
        memcpy(s->pts, pts, n * sizeof(float));
}

uint32_t flux_path_dropped_count(const flux_path *p) {
    return p ? p->dropped : 0;
}

void flux_path_reset(flux_path *p) {
    if (!p)
        return;
    p->count = 0;
    p->capacity = 0;
    p->segments = nullptr;
    p->dropped = 0;
    p->cursor_x = 0.0f;
    p->cursor_y = 0.0f;
}

flux_result flux_path_create(flux_path **out, flux_arena *arena) {
    if (!out)
        return FLUX_ERROR_INVALID_ARGUMENT;
    *out = nullptr;
    if (!arena) {
        FLUX_FAIL(FLUX_ERROR_INVALID_ARGUMENT, "flux_path_create requires a non-NULL arena");
        return FLUX_ERROR_INVALID_ARGUMENT;
    }
    flux_path *p = flux_arena_alloc_aligned(arena, sizeof(*p), alignof(flux_path));
    if (!p) {
        FLUX_FAIL(FLUX_ERROR_OUT_OF_MEMORY, "arena exhausted while allocating flux_path");
        return FLUX_ERROR_OUT_OF_MEMORY;
    }
    memset(p, 0, sizeof(*p));
    p->arena = arena;
    *out = p;
    return FLUX_OK;
}

void flux_path_move_to(flux_path *p, float x, float y) {
    if (!p)
        return;
    float pts[2] = {x, y};
    push_segment(p, p->arena, FLUX_PATH_MOVE, pts, 2);
    p->cursor_x = x;
    p->cursor_y = y;
}

void flux_path_line_to(flux_path *p, float x, float y) {
    if (!p)
        return;
    float pts[2] = {x, y};
    push_segment(p, p->arena, FLUX_PATH_LINE, pts, 2);
    p->cursor_x = x;
    p->cursor_y = y;
}

void flux_path_quad_to(flux_path *p, float cx, float cy, float x, float y) {
    if (!p)
        return;
    float pts[4] = {cx, cy, x, y};
    push_segment(p, p->arena, FLUX_PATH_QUAD, pts, 4);
    p->cursor_x = x;
    p->cursor_y = y;
}

void flux_path_cubic_to(flux_path *p, float c1x, float c1y, float c2x, float c2y, float x,
                        float y) {
    if (!p)
        return;
    float pts[6] = {c1x, c1y, c2x, c2y, x, y};
    push_segment(p, p->arena, FLUX_PATH_CUBIC, pts, 6);
    p->cursor_x = x;
    p->cursor_y = y;
}

void flux_path_close(flux_path *p) {
    if (!p)
        return;
    push_segment(p, p->arena, FLUX_PATH_CLOSE, nullptr, 0);
}

void flux_path_add_rect(flux_path *p, flux_rect r) {
    if (!p)
        return;
    flux_path_move_to(p, r.x, r.y);
    flux_path_line_to(p, r.x + r.w, r.y);
    flux_path_line_to(p, r.x + r.w, r.y + r.h);
    flux_path_line_to(p, r.x, r.y + r.h);
    flux_path_close(p);
}

void flux_path_add_round_rect(flux_path *p, flux_rect r, float radius) {
    if (!p)
        return;
    if (radius <= 0.0f) {
        flux_path_add_rect(p, r);
        return;
    }
    float rr = radius;
    if (rr > r.w * 0.5f)
        rr = r.w * 0.5f;
    if (rr > r.h * 0.5f)
        rr = r.h * 0.5f;

    if (rr >= r.w * 0.5f - 1e-4f && rr >= r.h * 0.5f - 1e-4f) {
        flux_path_add_circle(p, r.x + r.w * 0.5f, r.y + r.h * 0.5f, rr);
        return;
    }

    const float k = 0.5522847498f * rr;
    float x0 = r.x, y0 = r.y;
    float x1 = r.x + r.w, y1 = r.y + r.h;

    flux_path_move_to(p, x0 + rr, y0);
    flux_path_line_to(p, x1 - rr, y0);
    flux_path_cubic_to(p, x1 - rr + k, y0, x1, y0 + rr - k, x1, y0 + rr);
    flux_path_line_to(p, x1, y1 - rr);
    flux_path_cubic_to(p, x1, y1 - rr + k, x1 - rr + k, y1, x1 - rr, y1);
    flux_path_line_to(p, x0 + rr, y1);
    flux_path_cubic_to(p, x0 + rr - k, y1, x0, y1 - rr + k, x0, y1 - rr);
    flux_path_line_to(p, x0, y0 + rr);
    flux_path_cubic_to(p, x0, y0 + rr - k, x0 + rr - k, y0, x0 + rr, y0);
    flux_path_close(p);
}

/* ------------------------------------------------------------------ */
/*  Squircle corner fit (ADR-0118 INV-SDF-06)                         */
/*                                                                    */
/*  flux_sdf_squircle (include/flux/sdf.h) defines the squircle corner as */
/*  a blend of a circular arc (curvature 0) and an L4 superellipse      */
/*  (curvature 1). Its zero set is a corner arc that a single cubic     */
/*  cannot carry: one cubic per corner is 3.1% of the radius off at     */
/*  curvature 1. Each corner is therefore emitted as two cubics fitted  */
/*  to that arc.                                                       */
/*                                                                    */
/*  The fit is Schneider's algorithm (least squares alternated with     */
/*  geometric reparameterisation) on the arc rho(theta) that solves     */
/*  (1-c)(|q|-r) + c(L4(q)-r)/grad = 0. Two properties make it          */
/*  trustworthy rather than merely plausible:                           */
/*    - at curvature 0 it reproduces the classically known two-cubic     */
/*      circle constant 0.265216 to five decimals (0.2652022), which     */
/*      validates the algorithm;                                        */
/*    - its worst deviation across the whole family is 0.33% of the      */
/*      corner radius, i.e. sub-pixel at any normal radius.             */
/*  test_canvas_squircle asserts the fitted path lies on the field's     */
/*  zero set, so the two cannot drift apart.                            */
/*                                                                    */
/*  Columns: curvature, control offset at the edge end, and at the       */
/*  diagonal end, both in units of the corner radius. The diagonal       */
/*  radius is exactly linear in curvature (verified: identical           */
/*  increments to seven decimals, ending at 2^(1/4)) and so is computed  */
/*  rather than tabulated.                                              */
/* ------------------------------------------------------------------ */

typedef struct squircle_fit_node {
    float curvature;
    float d_edge;
    float d_mid;
} squircle_fit_node;

static const squircle_fit_node SQUIRCLE_FIT[] = {
    {0.000f, 0.2652022f, 0.2652022f}, /* 0.095% of r */
    {0.125f, 0.2523099f, 0.2638708f}, /* 0.138% */
    {0.250f, 0.2404602f, 0.2629363f}, /* 0.146% */
    {0.375f, 0.2296833f, 0.2626447f}, /* 0.146% */
    {0.500f, 0.2200156f, 0.2631321f}, /* 0.143% */
    {0.625f, 0.2115071f, 0.2644686f}, /* 0.152% */
    {0.750f, 0.2042209f, 0.2666914f}, /* 0.190% */
    {0.875f, 0.1982264f, 0.2698249f}, /* 0.247% */
    {1.000f, 0.1935914f, 0.2738925f}, /* 0.330% */
};

#define SQUIRCLE_FIT_COUNT (sizeof(SQUIRCLE_FIT) / sizeof(SQUIRCLE_FIT[0]))

static void squircle_offsets(float c, float *d_edge, float *d_mid) {
    if (!(c > 0.0f)) {
        *d_edge = SQUIRCLE_FIT[0].d_edge;
        *d_mid = SQUIRCLE_FIT[0].d_mid;
        return;
    }
    if (c >= 1.0f) {
        *d_edge = SQUIRCLE_FIT[SQUIRCLE_FIT_COUNT - 1].d_edge;
        *d_mid = SQUIRCLE_FIT[SQUIRCLE_FIT_COUNT - 1].d_mid;
        return;
    }
    uint32_t i = 0;
    while (i + 2 < SQUIRCLE_FIT_COUNT && c > SQUIRCLE_FIT[i + 1].curvature)
        ++i;
    float c0 = SQUIRCLE_FIT[i].curvature;
    float c1 = SQUIRCLE_FIT[i + 1].curvature;
    float t = (c - c0) / (c1 - c0);
    *d_edge = SQUIRCLE_FIT[i].d_edge + t * (SQUIRCLE_FIT[i + 1].d_edge - SQUIRCLE_FIT[i].d_edge);
    *d_mid = SQUIRCLE_FIT[i].d_mid + t * (SQUIRCLE_FIT[i + 1].d_mid - SQUIRCLE_FIT[i].d_mid);
}

/* One squircle corner, traversed from `a_dir` to `b_dir` (perpendicular unit
 * vectors pointing from the corner centre at the two edge endpoints), as two
 * cubics whose union approximates the field's corner arc. */
static void squircle_corner(flux_path *p, flux_point centre, flux_point a_dir, flux_point b_dir,
                            float radius, float d_edge, float d_mid, float k45) {
    flux_point A = {centre.x + a_dir.x * radius, centre.y + a_dir.y * radius};
    flux_point B = {centre.x + b_dir.x * radius, centre.y + b_dir.y * radius};

    /* Diagonal point: along the bisector at the blend arc's diagonal radius. */
    flux_point bis = {a_dir.x + b_dir.x, a_dir.y + b_dir.y};
    float bl = sqrtf(bis.x * bis.x + bis.y * bis.y);
    flux_point u = {bis.x / bl, bis.y / bl};
    flux_point D = {centre.x + u.x * radius * k45, centre.y + u.y * radius * k45};

    /* Traversal tangents: the radial direction rotated +90 degrees. */
    flux_point tA = {-a_dir.y, a_dir.x};
    flux_point tB = {-b_dir.y, b_dir.x};
    flux_point tD = {-u.y, u.x};

    float e = d_edge * radius, m = d_mid * radius;
    flux_path_cubic_to(p, A.x + tA.x * e, A.y + tA.y * e, D.x - tD.x * m, D.y - tD.y * m, D.x, D.y);
    flux_path_cubic_to(p, D.x + tD.x * m, D.y + tD.y * m, B.x - tB.x * e, B.y - tB.y * e, B.x, B.y);
}

void flux_path_add_squircle(flux_path *p, flux_rect r, float radius, float curvature) {
    if (!p)
        return;
    if (radius <= 0.0f) {
        flux_path_add_rect(p, r);
        return;
    }
    float c = curvature;
    if (c < 0.0f)
        c = 0.0f;
    if (c > 1.0f)
        c = 1.0f;

    /* curvature 0 is a rounded rectangle: same corner radius, circular arc. */
    if (c <= 0.001f) {
        flux_path_add_round_rect(p, r, radius);
        return;
    }

    float max_r = fminf(r.w * 0.5f, r.h * 0.5f);
    float rr = fminf(radius, max_r);
    if (rr <= 0.0f) {
        flux_path_add_rect(p, r);
        return;
    }

    float d_edge, d_mid;
    squircle_offsets(c, &d_edge, &d_mid);
    /* Diagonal radius of the blend arc: 1 at curvature 0 (circular) and
     * 2^(1/4) at curvature 1 (L4 corner). Exactly linear in curvature. */
    float k45 = 1.0f + (1.189207115f - 1.0f) * c;

    float x0 = r.x, y0 = r.y;
    float x1 = r.x + r.w, y1 = r.y + r.h;

    flux_path_move_to(p, x0 + rr, y0);
    flux_path_line_to(p, x1 - rr, y0);
    squircle_corner(p, (flux_point){x1 - rr, y0 + rr}, (flux_point){0.0f, -1.0f},
                    (flux_point){1.0f, 0.0f}, rr, d_edge, d_mid, k45);
    flux_path_line_to(p, x1, y1 - rr);
    squircle_corner(p, (flux_point){x1 - rr, y1 - rr}, (flux_point){1.0f, 0.0f},
                    (flux_point){0.0f, 1.0f}, rr, d_edge, d_mid, k45);
    flux_path_line_to(p, x0 + rr, y1);
    squircle_corner(p, (flux_point){x0 + rr, y1 - rr}, (flux_point){0.0f, 1.0f},
                    (flux_point){-1.0f, 0.0f}, rr, d_edge, d_mid, k45);
    flux_path_line_to(p, x0, y0 + rr);
    squircle_corner(p, (flux_point){x0 + rr, y0 + rr}, (flux_point){-1.0f, 0.0f},
                    (flux_point){0.0f, -1.0f}, rr, d_edge, d_mid, k45);
    flux_path_close(p);
}

void flux_path_add_circle(flux_path *p, float cx, float cy, float radius) {
    if (!p)
        return;
    const float k = 0.5522847498f * radius;
    flux_path_move_to(p, cx + radius, cy);
    flux_path_cubic_to(p, cx + radius, cy + k, cx + k, cy + radius, cx, cy + radius);
    flux_path_cubic_to(p, cx - k, cy + radius, cx - radius, cy + k, cx - radius, cy);
    flux_path_cubic_to(p, cx - radius, cy - k, cx - k, cy - radius, cx, cy - radius);
    flux_path_cubic_to(p, cx + k, cy - radius, cx + radius, cy - k, cx + radius, cy);
    flux_path_close(p);
}
