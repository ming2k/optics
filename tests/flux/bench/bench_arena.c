#include "bench_helpers.h"
#include <flux/flux.h>
#include <stdio.h>
#include <string.h>

#define N BENCH_ITERATIONS

typedef struct {
    flux_arena *arena;
    size_t alloc_size;
} arena_alloc_ctx;

static void bench_arena_alloc(void *p) {
    arena_alloc_ctx *c = p;
    for (int i = 0; i < 1000; ++i) {
        flux_arena_alloc(c->arena, c->alloc_size);
    }
    flux_arena_reset(c->arena);
}

static void bench_arena_alloc_aligned(void *p) {
    arena_alloc_ctx *c = p;
    for (int i = 0; i < 1000; ++i) {
        flux_arena_alloc_aligned(c->arena, c->alloc_size, 64);
    }
    flux_arena_reset(c->arena);
}

typedef struct {
    flux_arena *arena;
} arena_reset_ctx;

static void bench_arena_reset(void *p) {
    arena_reset_ctx *c = p;
    flux_arena_reset(c->arena);
    for (int i = 0; i < 1000; ++i)
        flux_arena_alloc(c->arena, 32);
}

int main(void) {
    fprintf(stdout, "=== arena benchmarks ===\n");

    flux_arena arena;
    if (flux_arena_init(&arena, 1 << 20, nullptr) != FLUX_OK)
        return 1;

    arena_alloc_ctx ctx8 = {&arena, 8};
    arena_alloc_ctx ctx64 = {&arena, 64};
    arena_alloc_ctx ctx256 = {&arena, 256};

    BENCH_RUN("arena_alloc(8)", N, bench_arena_alloc, &ctx8);
    BENCH_RUN("arena_alloc(64)", N, bench_arena_alloc, &ctx64);
    BENCH_RUN("arena_alloc(256)", N, bench_arena_alloc, &ctx256);

    BENCH_RUN("arena_alloc_aligned(8, 64)", N, bench_arena_alloc_aligned, &ctx8);
    BENCH_RUN("arena_alloc_aligned(64, 64)", N, bench_arena_alloc_aligned, &ctx64);

    arena_reset_ctx rctx = {&arena};
    BENCH_RUN("arena_reset (after 1000 x 32B)", N, bench_arena_reset, &rctx);

    flux_arena_destroy(&arena);

    fprintf(stdout, "\n");
    return 0;
}
