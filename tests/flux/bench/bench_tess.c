#include "../../../libs/flux/src/canvas/internal.h"
#include "bench_helpers.h"
#include <flux/flux.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#define N BENCH_ITERATIONS

void flux_set_last_error(flux_result code, const char *function, const char *file, int line,
                         const char *message, int32_t backend_code) {
    (void)code;
    (void)function;
    (void)file;
    (void)line;
    (void)message;
    (void)backend_code;
}

/* Internal allocator stubs — hidden in libflux.so, same reasoning as
 * flux_set_last_error above. */
void *flux_internal_alloc(flux_device *d, size_t bytes) {
    (void)d;
    return calloc(1, bytes);
}

void flux_internal_free(flux_device *d, void *ptr) {
    (void)d;
    free(ptr);
}

static uint32_t g_emit_tri_count;

void emit_tri(flux_canvas_vertex *verts, uint32_t *count, uint32_t cap, flux_mat3x2 tx,
              flux_color color, flux_point a, flux_point b, flux_point e) {
    (void)tx;
    (void)color;
    (void)a;
    (void)b;
    (void)e;
    if (*count + 3 > cap)
        return;
    *count += 3;
    g_emit_tri_count += 3;
}

void submit_triangles(flux_canvas *c, const flux_paint *paint, const flux_canvas_vertex *verts,
                      uint32_t vertex_count) {
    (void)c;
    (void)paint;
    (void)verts;
    (void)vertex_count;
}

void submit_triangles_id(flux_canvas *c, const flux_paint *paint, canvas_pipe_id id,
                         const flux_canvas_vertex *verts, uint32_t vertex_count) {
    (void)c;
    (void)paint;
    (void)id;
    (void)verts;
    (void)vertex_count;
}

/* Referenced by the tess cache (compiled in with geometry_tess.c) but
 * never reached here: the bench drives ear_clip_contour directly. */
void push_vertex(flux_canvas_vertex *v, flux_point p, flux_mat3x2 tx, flux_color c) {
    (void)v;
    (void)p;
    (void)tx;
    (void)c;
}

float flux_canvas_mat3x2_pixel_scale(flux_mat3x2 m) {
    (void)m;
    return 1.0f;
}

flatten_multi flatten_path_to_contours(const flux_path *p, float pixel_scale, flux_point *out_pts,
                                       uint32_t pts_cap, flux_canvas_contour *out_cons,
                                       uint32_t cons_cap) {
    (void)p;
    (void)pixel_scale;
    (void)out_pts;
    (void)pts_cap;
    (void)out_cons;
    (void)cons_cap;
    return (flatten_multi){0};
}

static uint32_t *g_prev;
static uint32_t *g_next;

typedef struct {
    flux_point *pts;
    uint32_t n;
    flux_canvas_vertex *verts;
    uint32_t *v_count;
    uint32_t verts_cap;
    flux_mat3x2 tx;
    flux_color color;
    uint32_t result;
} tess_ctx;

static void bench_tess_ear_clip(void *p) {
    tess_ctx *c = p;
    *c->v_count = 0;
    for (uint32_t i = 0; i < c->n; ++i) {
        g_prev[i] = (i == 0) ? c->n - 1 : i - 1;
        g_next[i] = (i + 1 == c->n) ? 0 : i + 1;
    }
    ear_clip_contour(c->verts, c->v_count, c->verts_cap, c->tx, c->color, c->pts, g_prev, g_next,
                     c->n);
    c->result = *c->v_count;
}

static void make_regular_polygon(flux_point *pts, uint32_t n, float cx, float cy, float r) {
    for (uint32_t i = 0; i < n; ++i) {
        float a = (float)i / (float)n * 2.0f * 3.14159265359f;
        pts[i].x = cx + r * cosf(a);
        pts[i].y = cy + r * sinf(a);
    }
}

static void make_concave_l(flux_point *pts, uint32_t *n) {
    *n = 6;
    pts[0] = (flux_point){0, 0};
    pts[1] = (flux_point){20, 0};
    pts[2] = (flux_point){20, 10};
    pts[3] = (flux_point){10, 10};
    pts[4] = (flux_point){10, 20};
    pts[5] = (flux_point){0, 20};
}

static void make_star(flux_point *pts, uint32_t n, float cx, float cy, float r_outer,
                      float r_inner) {
    for (uint32_t i = 0; i < n; ++i) {
        float a_outer = (float)(i * 2) / (float)(n * 2) * 2.0f * 3.14159265359f - 1.57079632679f;
        pts[i * 2] = (flux_point){cx + r_outer * cosf(a_outer), cy + r_outer * sinf(a_outer)};
        float a_inner =
            (float)(i * 2 + 1) / (float)(n * 2) * 2.0f * 3.14159265359f - 1.57079632679f;
        pts[i * 2 + 1] = (flux_point){cx + r_inner * cosf(a_inner), cy + r_inner * sinf(a_inner)};
    }
}

int main(void) {
    fprintf(stdout, "=== tessellator benchmarks ===\n");

    uint32_t max_pts = 256;
    flux_point *pts = malloc(max_pts * sizeof(flux_point));
    flux_canvas_vertex *verts =
        malloc(FLUX_CANVAS_PATH_SCRATCH_CAP * 3 * sizeof(flux_canvas_vertex));
    uint32_t v_count = 0;
    g_prev = malloc(max_pts * sizeof(uint32_t));
    g_next = malloc(max_pts * sizeof(uint32_t));

    tess_ctx ctx = {
        .pts = pts,
        .verts = verts,
        .v_count = &v_count,
        .verts_cap = FLUX_CANVAS_PATH_SCRATCH_CAP * 3,
        .tx = flux_mat3x2_identity(),
        .color = 0xFFFFFFFF,
    };

    ctx.n = 4;
    make_regular_polygon(pts, 4, 50, 50, 50);
    BENCH_RUN("tess_square(4 verts)", N, bench_tess_ear_clip, &ctx);

    ctx.n = 32;
    make_regular_polygon(pts, 32, 50, 50, 50);
    BENCH_RUN("tess_regular_32gon(32 verts)", N, bench_tess_ear_clip, &ctx);

    ctx.n = 64;
    make_regular_polygon(pts, 64, 50, 50, 50);
    BENCH_RUN("tess_regular_64gon(64 verts)", N, bench_tess_ear_clip, &ctx);

    ctx.n = 128;
    make_regular_polygon(pts, 128, 50, 50, 50);
    BENCH_RUN("tess_regular_128gon(128 verts)", N, bench_tess_ear_clip, &ctx);

    make_concave_l(pts, &ctx.n);
    BENCH_RUN("tess_concave_L(6 verts)", N, bench_tess_ear_clip, &ctx);

    ctx.n = 10;
    make_star(pts, 5, 50, 50, 50, 25);
    BENCH_RUN("tess_star_5point(10 verts)", N, bench_tess_ear_clip, &ctx);

    ctx.n = 20;
    make_star(pts, 10, 50, 50, 50, 25);
    BENCH_RUN("tess_star_10point(20 verts)", N, bench_tess_ear_clip, &ctx);

    free(pts);
    free(verts);
    free(g_prev);
    free(g_next);

    fprintf(stdout, "\n");
    return 0;
}
