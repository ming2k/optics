#include "../../../libs/flux/src/canvas/internal.h"
#include "bench_helpers.h"
#include <flux/flux.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define W 128u
#define H 128u

typedef struct {
    flux_point *pts;
    uint32_t count;
    flux_canvas_contour con;
    uint32_t w, h;
    float ox, oy;
    float *out_cov;
} cov_bench_ctx;

static void run_raster(void *p) {
    cov_bench_ctx *c = p;
    (void)canvas_coverage_rasterize(c->pts, &c->con, 1, FLUX_FILL_NON_ZERO, c->ox, c->oy, c->w,
                                    c->h, c->out_cov);
}

static void bench_polygon(uint32_t n, uint32_t iters, const char *label) {
    flux_point *pts = malloc((size_t)n * sizeof(flux_point));
    float *out_cov = malloc((size_t)W * H * sizeof(float));
    if (!pts || !out_cov) {
        free(pts);
        free(out_cov);
        return;
    }
    for (uint32_t i = 0; i < n; ++i) {
        double a = (double)i * 2.0 * 3.14159265358979 / (double)n;
        pts[i].x = (float)(50.0 * cos(a));
        pts[i].y = (float)(50.0 * sin(a));
    }
    cov_bench_ctx ctx = {
        .pts = pts,
        .count = n,
        .con = {.start = 0, .count = n, .closed = true},
        .w = W,
        .h = H,
        .ox = -64.0f,
        .oy = -64.0f,
        .out_cov = out_cov,
    };
    BENCH_RUN(label, iters, run_raster, &ctx);
    free(pts);
    free(out_cov);
}

static void bench_star(uint32_t n, uint32_t iters, const char *label) {
    flux_point *pts = malloc((size_t)n * sizeof(flux_point));
    const uint32_t sw = 512, sh = 512;
    float *out_cov = malloc((size_t)sw * sh * sizeof(float));
    if (!pts || !out_cov) {
        free(pts);
        free(out_cov);
        return;
    }
    for (uint32_t i = 0; i < n; ++i) {
        double a = (double)i * 2.0 * 3.14159265358979 / (double)n;
        double r = (i % 2 == 0) ? 200.0 : 50.0;
        pts[i].x = (float)(r * cos(a));
        pts[i].y = (float)(r * sin(a));
    }
    cov_bench_ctx ctx = {
        .pts = pts,
        .count = n,
        .con = {.start = 0, .count = n, .closed = true},
        .w = sw,
        .h = sh,
        .ox = -256.0f,
        .oy = -256.0f,
        .out_cov = out_cov,
    };
    BENCH_RUN(label, iters, run_raster, &ctx);
    free(pts);
    free(out_cov);
}

int main(void) {
    printf("Coverage rasterizer benchmarks (W=%u, H=%u):\n", W, H);
    bench_polygon(64, 50, "polygon 64 vertices");
    bench_polygon(256, 20, "polygon 256 vertices");
    bench_polygon(512, 10, "polygon 512 vertices");
    bench_polygon(1024, 5, "polygon 1024 vertices");
    printf("\nComplex star (W=512, H=512):\n");
    bench_star(256, 10, "star 256 vertices");
    bench_star(512, 5, "star 512 vertices");
    bench_star(1024, 3, "star 1024 vertices");
    return 0;
}
