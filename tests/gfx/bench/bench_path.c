#include "../../../libs/gfx/src/canvas/internal.h"
#include "bench_helpers.h"
#include <optics/gfx.h>
#include <stdio.h>
#include <string.h>

#define N BENCH_ITERATIONS

typedef struct {
    optics_gfx_arena *arena;
    uint32_t count;
} path_ctx;

static void bench_path_add_rect(void *p) {
    path_ctx *c = p;
    for (int i = 0; i < 100; ++i) {
        optics_gfx_path *path = nullptr;
        (void)optics_gfx_path_create(&path, c->arena);
        optics_gfx_path_add_rect(path, (optics_gfx_rect){0, 0, 100, 100});
    }
    optics_gfx_arena_reset(c->arena);
}

static void bench_path_add_circle(void *p) {
    path_ctx *c = p;
    for (int i = 0; i < 100; ++i) {
        optics_gfx_path *path = nullptr;
        (void)optics_gfx_path_create(&path, c->arena);
        optics_gfx_path_add_circle(path, 50.0f, 50.0f, 25.0f);
    }
    optics_gfx_arena_reset(c->arena);
}

static void bench_path_add_round_rect(void *p) {
    path_ctx *c = p;
    for (int i = 0; i < 100; ++i) {
        optics_gfx_path *path = nullptr;
        (void)optics_gfx_path_create(&path, c->arena);
        optics_gfx_path_add_rrect(path, (optics_gfx_rect){0, 0, 100, 100},
                                  optics_gfx_corner_radii_uniform(10.0f), 0.0f);
    }
    optics_gfx_arena_reset(c->arena);
}

static void bench_path_line_to_1000(void *p) {
    path_ctx *c = p;
    optics_gfx_path *path = nullptr;
    (void)optics_gfx_path_create(&path, c->arena);
    optics_gfx_path_move_to(path, 0, 0);
    for (int i = 0; i < 1000; ++i)
        optics_gfx_path_line_to(path, (float)i, (float)i);
    optics_gfx_arena_reset(c->arena);
}

static void bench_path_cubic_to_1000(void *p) {
    path_ctx *c = p;
    optics_gfx_path *path = nullptr;
    (void)optics_gfx_path_create(&path, c->arena);
    optics_gfx_path_move_to(path, 0, 0);
    for (int i = 0; i < 1000; ++i)
        optics_gfx_path_cubic_to(path, (float)i, 1.0f, (float)i + 0.5f, 2.0f, (float)i + 1.0f,
                                 0.0f);
    optics_gfx_arena_reset(c->arena);
}

int main(void) {
    fprintf(stdout, "=== path builder benchmarks ===\n");

    optics_gfx_arena arena;
    if (optics_gfx_arena_init(&arena, 1 << 22, nullptr) != OPTICS_GFX_OK)
        return 1;

    path_ctx ctx = {.arena = &arena};

    BENCH_RUN("path_add_rect x100", N, bench_path_add_rect, &ctx);
    BENCH_RUN("path_add_circle x100", N, bench_path_add_circle, &ctx);
    BENCH_RUN("path_add_round_rect x100", N, bench_path_add_round_rect, &ctx);
    BENCH_RUN("path_line_to x1000", N, bench_path_line_to_1000, &ctx);
    BENCH_RUN("path_cubic_to x1000", N, bench_path_cubic_to_1000, &ctx);

    optics_gfx_arena_deinit(&arena);

    fprintf(stdout, "\n");
    return 0;
}
