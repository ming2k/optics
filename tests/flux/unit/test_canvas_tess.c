/*
 * Tessellator orientation regression.
 *
 * `flux_canvas_fill_path` runs every closed contour through
 * `ear_clip_contour`, which expects math-CCW input. The reversal
 * step in `flux_canvas_fill_path` consults `signed_area` to decide
 * whether to flip the contour; the sign of that comparison must
 * match the ear-clip's cross-product convention or every closed
 * input bails out after one pass and the caller sees a single
 * degenerate triangle instead of (n - 2).
 *
 * The test compiles geometry_tess.c into the test binary, stubs
 * the symbols `flux_canvas_fill_path` reaches into (the flattener
 * and the renderer submit), constructs a stack-resident fake
 * `flux_canvas`, and asserts the full triangulation lands in
 * the captured submit. This drives the actual reversal sign in
 * `fill_path` — a regression that flips it back fails here.
 */
#include "../../../libs/flux/src/canvas/internal.h"
#include "test_helpers.h"
#include <flux/flux.h>

#include <string.h>

/* --------------------------------------------------------------- */
/*  Test seam: scripted flatten output + captured submit           */
/* --------------------------------------------------------------- */

static flux_point g_polygon[64];
static uint32_t g_polygon_n = 0;

/* Multi-contour script: when g_script_contour_n > 0 the flatten stub
 * replays these instead of the single g_polygon. Sized to fill the
 * whole scratch so overflow regressions can be scripted. */
static flux_point g_script_pts[FLUX_CANVAS_PATH_SCRATCH_CAP];
static uint32_t g_script_pts_n = 0;
static flux_canvas_contour g_script_cons[4];
static uint32_t g_script_contour_n = 0;

static uint32_t g_submitted_vertex_count = 0;
static flux_canvas_vertex g_submitted_verts[FLUX_CANVAS_PATH_SCRATCH_CAP * 3];

flatten_multi flatten_path_to_contours(const flux_path *p, float pixel_scale, flux_point *out_pts,
                                       uint32_t pts_cap, flux_canvas_contour *out_cons,
                                       uint32_t cons_cap) {
    (void)p;
    (void)pixel_scale;
    if (g_script_contour_n > 0) {
        if (g_script_pts_n > pts_cap || g_script_contour_n > cons_cap) {
            return (flatten_multi){0};
        }
        for (uint32_t i = 0; i < g_script_pts_n; ++i)
            out_pts[i] = g_script_pts[i];
        for (uint32_t i = 0; i < g_script_contour_n; ++i)
            out_cons[i] = g_script_cons[i];
        return (flatten_multi){.point_count = g_script_pts_n, .contour_count = g_script_contour_n};
    }
    if (g_polygon_n == 0 || g_polygon_n > pts_cap || cons_cap < 1) {
        return (flatten_multi){0};
    }
    for (uint32_t i = 0; i < g_polygon_n; ++i)
        out_pts[i] = g_polygon[i];
    out_cons[0] = (flux_canvas_contour){
        .start = 0,
        .count = g_polygon_n,
        .closed = true,
        .first_x = g_polygon[0].x,
        .first_y = g_polygon[0].y,
    };
    return (flatten_multi){.point_count = g_polygon_n, .contour_count = 1};
}

void submit_triangles(flux_canvas *c, const flux_paint *paint, const flux_canvas_vertex *verts,
                      uint32_t vertex_count) {
    (void)c;
    (void)paint;
    g_submitted_vertex_count = vertex_count;
    if (vertex_count <= sizeof(g_submitted_verts) / sizeof(g_submitted_verts[0])) {
        memcpy(g_submitted_verts, verts, vertex_count * sizeof(verts[0]));
    }
}

/* The stencil fallback's submit seam. The fake canvas carries no
 * stencil attachment, so fill_path never reaches it here; the stub
 * only satisfies the link. */
void submit_triangles_id(flux_canvas *c, const flux_paint *paint, canvas_pipe_id id,
                         const flux_canvas_vertex *verts, uint32_t vertex_count) {
    (void)id;
    submit_triangles(c, paint, verts, vertex_count);
}

/* Stub for the pixel-scale helper that lives in geometry_flatten.c —
 * the test scripts the flatten output directly, so the actual value
 * is unused. Any positive constant works. */
float flux_canvas_mat3x2_pixel_scale(flux_mat3x2 m) {
    (void)m;
    return 1.0f;
}

/* Stubs for the internal allocator + error sink (hidden in
 * libflux.so, same reasoning as the flatten stubs above). The fake
 * canvas has no device, so the real implementations would route to
 * libc anyway. */
void flux_set_last_error(flux_result code, const char *function, const char *file, int line,
                         const char *message, int32_t backend_code) {
    (void)code;
    (void)function;
    (void)file;
    (void)line;
    (void)message;
    (void)backend_code;
}

void *flux_internal_alloc(flux_device *d, size_t bytes) {
    (void)d;
    return calloc(1, bytes);
}

void flux_internal_free(flux_device *d, void *ptr) {
    (void)d;
    free(ptr);
}

void emit_tri(flux_canvas_vertex *verts, uint32_t *count, uint32_t cap, flux_mat3x2 tx,
              flux_color color, flux_point a, flux_point b, flux_point e) {
    (void)tx;
    (void)color;
    if (*count + 3 > cap)
        return;
    verts[*count].pos[0] = a.x;
    verts[*count].pos[1] = a.y;
    verts[*count + 1].pos[0] = b.x;
    verts[*count + 1].pos[1] = b.y;
    verts[*count + 2].pos[0] = e.x;
    verts[*count + 2].pos[1] = e.y;
    *count += 3;
}

/* --------------------------------------------------------------- */
/*  Stack-resident fake canvas                                     */
/* --------------------------------------------------------------- */

static flux_point fk_pts[FLUX_CANVAS_PATH_SCRATCH_CAP];
static flux_canvas_vertex fk_vrts[FLUX_CANVAS_PATH_SCRATCH_CAP * 3];
static flux_canvas_contour fk_cons[FLUX_CANVAS_MAX_CONTOURS];
static uint32_t fk_prev[FLUX_CANVAS_PATH_SCRATCH_CAP];
static uint32_t fk_next[FLUX_CANVAS_PATH_SCRATCH_CAP];

static void reset_capture(void) {
    g_submitted_vertex_count = 0;
}

static flux_canvas make_fake_canvas(void) {
    flux_canvas c = {0};
    c.recording = true;
    c.scratch_pts = fk_pts;
    c.scratch_verts = fk_vrts;
    c.scratch_contours = fk_cons;
    c.scratch_lnk_prev = fk_prev;
    c.scratch_lnk_next = fk_next;
    c.states[0].transform = flux_mat3x2_identity();
    c.state_top = 0;
    return c;
}

static void run_fill(flux_point *poly, uint32_t n) {
    g_script_contour_n = 0;
    g_polygon_n = n;
    for (uint32_t i = 0; i < n; ++i)
        g_polygon[i] = poly[i];
    reset_capture();

    flux_canvas c = make_fake_canvas();
    flux_path dummy = {0}; /* opaque to our scripted flatten */
    dummy.count = 1;       /* defeat the empty-path early-return */
    flux_paint paint = flux_paint_default();
    flux_canvas_fill_path(&c, &dummy, &paint);
}

static void run_fill_scripted(void) {
    reset_capture();
    flux_canvas c = make_fake_canvas();
    flux_path dummy = {0};
    dummy.count = 1;
    flux_paint paint = flux_paint_default();
    flux_canvas_fill_path(&c, &dummy, &paint);
}

/* Append a circle contour to the multi-contour script. `ccw` selects
 * the solid winding (per this file's signed_area convention); false
 * gives the hole winding. */
static void script_circle(float cx, float cy, float r, uint32_t n, bool ccw) {
    uint32_t start = g_script_pts_n;
    for (uint32_t i = 0; i < n; ++i) {
        float a = (float)i / (float)n * 6.28318530718f;
        if (!ccw)
            a = -a;
        g_script_pts[g_script_pts_n++] = (flux_point){cx + r * cosf(a), cy + r * sinf(a)};
    }
    g_script_cons[g_script_contour_n++] = (flux_canvas_contour){
        .start = start,
        .count = n,
        .closed = true,
        .first_x = g_script_pts[start].x,
        .first_y = g_script_pts[start].y,
    };
}

/* --------------------------------------------------------------- */
/*  Cases                                                          */
/* --------------------------------------------------------------- */

int main(void) {
    /* --- square, screen-CW (math-CCW) — fill_path leaves untouched --- */
    {
        flux_point p[] = {
            {0.0f, 0.0f},
            {10.0f, 0.0f},
            {10.0f, 10.0f},
            {0.0f, 10.0f},
        };
        run_fill(p, 4);
        EXPECT(g_submitted_vertex_count == 3 * (4 - 2));
    }

    /* --- same square, opposite winding — fill_path reverses --- */
    {
        flux_point p[] = {
            {0.0f, 0.0f},
            {0.0f, 10.0f},
            {10.0f, 10.0f},
            {10.0f, 0.0f},
        };
        run_fill(p, 4);
        EXPECT(g_submitted_vertex_count == 3 * (4 - 2));
    }

    /* --- concave L (six vertices, four triangles) --- */
    {
        flux_point p[] = {
            {0.0f, 0.0f},   {20.0f, 0.0f},  {20.0f, 10.0f},
            {10.0f, 10.0f}, {10.0f, 20.0f}, {0.0f, 20.0f},
        };
        run_fill(p, 6);
        EXPECT(g_submitted_vertex_count == 3 * (6 - 2));
    }

    /* --- round-rect-shaped arc, exercises a denser path --- */
    {
        flux_point p[64];
        uint32_t n = 0;
        p[n++] = (flux_point){10.0f, 0.0f};
        p[n++] = (flux_point){90.0f, 0.0f};
        for (uint32_t i = 1; i <= 16; ++i) {
            float a = (float)i / 16.0f * 1.57079632679f;
            p[n++] = (flux_point){90.0f + 10.0f * sinf(a), 10.0f - 10.0f * cosf(a)};
        }
        p[n++] = (flux_point){100.0f, 90.0f};
        p[n++] = (flux_point){0.0f, 90.0f};
        p[n++] = (flux_point){0.0f, 10.0f};
        run_fill(p, n);
        EXPECT(g_submitted_vertex_count == 3 * (n - 2));
    }

    /* --- donut: small hole-in-solid via bridge merge --- */
    {
        g_script_pts_n = 0;
        g_script_contour_n = 0;
        script_circle(0, 0, 100.0f, 32, true); /* outer, solid  */
        script_circle(0, 0, 40.0f, 16, false); /* inner, hole   */
        run_fill_scripted();
        /* outer(32) + hole(16) + 2 bridge points = 50 merged vertices
         * → 48 triangles when the ear clip completes. */
        EXPECT(g_submitted_vertex_count == 3 * (32 + 16 + 2 - 2));
    }

    /* --- regression: hole listed BEFORE its outer, near-full scratch.
     * The in-place bridge merge grows the outer's span by hole+2
     * points; with the outer starting deep in the scratch array the
     * merged polygon must not write past the scratch end (previously
     * an out-of-bounds write — caught by ASan builds). --- */
    {
        g_script_pts_n = 0;
        g_script_contour_n = 0;
        script_circle(0, 0, 50.0f, 1000, false); /* hole first      */
        script_circle(0, 0, 100.0f, 1040, true); /* outer at  1000  */
        run_fill_scripted();
        /* 1040 + 1000 + 2 = 2042 merged vertices → 2040 triangles. */
        EXPECT(g_submitted_vertex_count == 3 * (1040 + 1000 + 2 - 2));
    }

    /* --- signed_area sign convention is what the reversal expects --- */
    {
        flux_point ccw[] = {
            /* screen y-down natural order */
            {0.0f, 0.0f},
            {1.0f, 0.0f},
            {1.0f, 1.0f},
            {0.0f, 1.0f},
        };
        EXPECT(signed_area(ccw, 4) < 0.0f);

        flux_point cw[] = {
            {0.0f, 0.0f},
            {0.0f, 1.0f},
            {1.0f, 1.0f},
            {1.0f, 0.0f},
        };
        EXPECT(signed_area(cw, 4) > 0.0f);
    }

    TEST_SUMMARY();
}
