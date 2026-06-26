#include "../../../libs/flux/src/canvas/internal.h"
#include "bench_helpers.h"
#include <flux/flux.h>
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

typedef struct {
    flux_arena *arena;
    flux_point *pts;
    flux_canvas_contour *cons;
    uint32_t cap;
    float radius;
    float pixel_scale;
    uint32_t result;
} flatten_ctx;

static void bench_flatten_circle(void *p) {
    flatten_ctx *c = p;
    flux_path *path = nullptr;
    (void)flux_path_create(&path, c->arena);
    flux_path_add_circle(path, 0.0f, 0.0f, c->radius);
    flatten_multi r = flatten_path_to_contours(path, c->pixel_scale, c->pts, c->cap, c->cons,
                                               FLUX_CANVAS_MAX_CONTOURS);
    c->result = r.point_count;
    flux_arena_reset(c->arena);
}

static void bench_flatten_rect(void *p) {
    flatten_ctx *c = p;
    flux_path *path = nullptr;
    (void)flux_path_create(&path, c->arena);
    flux_path_add_rect(path, (flux_rect){0, 0, 100, 100});
    flatten_multi r = flatten_path_to_contours(path, c->pixel_scale, c->pts, c->cap, c->cons,
                                               FLUX_CANVAS_MAX_CONTOURS);
    c->result = r.point_count;
    flux_arena_reset(c->arena);
}

static void bench_flatten_round_rect(void *p) {
    flatten_ctx *c = p;
    flux_path *path = nullptr;
    (void)flux_path_create(&path, c->arena);
    flux_path_add_round_rect(path, (flux_rect){0, 0, 100, 100}, 10.0f);
    flatten_multi r = flatten_path_to_contours(path, c->pixel_scale, c->pts, c->cap, c->cons,
                                               FLUX_CANVAS_MAX_CONTOURS);
    c->result = r.point_count;
    flux_arena_reset(c->arena);
}

int main(void) {
    fprintf(stdout, "=== flatten benchmarks ===\n");

    flux_arena arena;
    if (flux_arena_init(&arena, 1 << 20, nullptr) != FLUX_OK)
        return 1;

    flux_point *pts = malloc(FLUX_CANVAS_PATH_SCRATCH_CAP * sizeof(flux_point));
    flux_canvas_contour *cons = malloc(FLUX_CANVAS_MAX_CONTOURS * sizeof(flux_canvas_contour));

    flatten_ctx ctx = {
        .arena = &arena,
        .pts = pts,
        .cons = cons,
        .cap = FLUX_CANVAS_PATH_SCRATCH_CAP,
    };

    ctx.radius = 50.0f;
    ctx.pixel_scale = 1.0f;
    BENCH_RUN("flatten_circle(r=50, scale=1)", N, bench_flatten_circle, &ctx);

    ctx.radius = 50.0f;
    ctx.pixel_scale = 10.0f;
    BENCH_RUN("flatten_circle(r=50, scale=10)", N, bench_flatten_circle, &ctx);

    ctx.radius = 200.0f;
    ctx.pixel_scale = 1.0f;
    BENCH_RUN("flatten_circle(r=200, scale=1)", N, bench_flatten_circle, &ctx);

    ctx.pixel_scale = 1.0f;
    BENCH_RUN("flatten_rect(100x100)", N, bench_flatten_rect, &ctx);

    ctx.pixel_scale = 1.0f;
    BENCH_RUN("flatten_round_rect(100x100, r=10)", N, bench_flatten_round_rect, &ctx);

    ctx.pixel_scale = 10.0f;
    BENCH_RUN("flatten_round_rect(100x100, r=10, scale=10)", N, bench_flatten_round_rect, &ctx);

    free(pts);
    free(cons);
    flux_arena_destroy(&arena);

    fprintf(stdout, "\n");
    return 0;
}
