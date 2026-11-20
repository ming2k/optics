#include "../../../libs/gfx/src/canvas/internal.h"
#include "bench_helpers.h"
#include <optics/gfx.h>
#include <stdio.h>
#include <string.h>

#define N BENCH_ITERATIONS

void optics_gfx_set_last_error(optics_gfx_result code, const char *function, const char *file,
                               int line, const char *message, int32_t backend_code) {
    (void)code;
    (void)function;
    (void)file;
    (void)line;
    (void)message;
    (void)backend_code;
}

typedef struct {
    optics_gfx_arena *arena;
    optics_gfx_point *pts;
    optics_gfx_canvas_contour *cons;
    uint32_t cap;
    float radius;
    float pixel_scale;
    uint32_t result;
} flatten_ctx;

static void bench_flatten_circle(void *p) {
    flatten_ctx *c = p;
    optics_gfx_path *path = nullptr;
    (void)optics_gfx_path_create(&path, c->arena);
    optics_gfx_path_add_circle(path, 0.0f, 0.0f, c->radius);
    flatten_multi r = flatten_path_to_contours(path, c->pixel_scale, c->pts, c->cap, c->cons,
                                               OPTICS_GFX_CANVAS_MAX_CONTOURS);
    c->result = r.point_count;
    optics_gfx_arena_reset(c->arena);
}

static void bench_flatten_rect(void *p) {
    flatten_ctx *c = p;
    optics_gfx_path *path = nullptr;
    (void)optics_gfx_path_create(&path, c->arena);
    optics_gfx_path_add_rect(path, (optics_gfx_rect){0, 0, 100, 100});
    flatten_multi r = flatten_path_to_contours(path, c->pixel_scale, c->pts, c->cap, c->cons,
                                               OPTICS_GFX_CANVAS_MAX_CONTOURS);
    c->result = r.point_count;
    optics_gfx_arena_reset(c->arena);
}

static void bench_flatten_round_rect(void *p) {
    flatten_ctx *c = p;
    optics_gfx_path *path = nullptr;
    (void)optics_gfx_path_create(&path, c->arena);
    optics_gfx_path_add_round_rect(path, (optics_gfx_rect){0, 0, 100, 100}, 10.0f);
    flatten_multi r = flatten_path_to_contours(path, c->pixel_scale, c->pts, c->cap, c->cons,
                                               OPTICS_GFX_CANVAS_MAX_CONTOURS);
    c->result = r.point_count;
    optics_gfx_arena_reset(c->arena);
}

int main(void) {
    fprintf(stdout, "=== flatten benchmarks ===\n");

    optics_gfx_arena arena;
    if (optics_gfx_arena_init(&arena, 1 << 20, nullptr) != OPTICS_GFX_OK)
        return 1;

    optics_gfx_point *pts = malloc(OPTICS_GFX_CANVAS_PATH_SCRATCH_CAP * sizeof(optics_gfx_point));
    optics_gfx_canvas_contour *cons =
        malloc(OPTICS_GFX_CANVAS_MAX_CONTOURS * sizeof(optics_gfx_canvas_contour));

    flatten_ctx ctx = {
        .arena = &arena,
        .pts = pts,
        .cons = cons,
        .cap = OPTICS_GFX_CANVAS_PATH_SCRATCH_CAP,
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
    optics_gfx_arena_deinit(&arena);

    fprintf(stdout, "\n");
    return 0;
}
