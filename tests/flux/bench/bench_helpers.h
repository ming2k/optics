#ifndef FLUX_BENCH_HELPERS_H
#define FLUX_BENCH_HELPERS_H

#include <flux/flux.h>

#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#define BENCH_WARMUP 3
#define BENCH_ITERATIONS 100

typedef struct bench_sample {
    double ns_per_iter;
    uint64_t iterations;
} bench_sample;

static inline double bench_clock_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1e9 + (double)ts.tv_nsec;
}

static inline bench_sample bench_run(uint64_t iterations, void (*fn)(void *ctx), void *ctx) {
    for (int w = 0; w < BENCH_WARMUP; ++w)
        fn(ctx);

    double t0 = bench_clock_ns();
    for (uint64_t i = 0; i < iterations; ++i)
        fn(ctx);
    double t1 = bench_clock_ns();

    bench_sample s;
    s.ns_per_iter = (t1 - t0) / (double)iterations;
    s.iterations = iterations;
    return s;
}

static inline void bench_print(const char *name, bench_sample s) {
    if (s.ns_per_iter < 1000.0)
        fprintf(stdout, "  %-40s %8.2f ns/iter\n", name, s.ns_per_iter);
    else if (s.ns_per_iter < 1e6)
        fprintf(stdout, "  %-40s %8.2f us/iter\n", name, s.ns_per_iter / 1e3);
    else
        fprintf(stdout, "  %-40s %8.2f ms/iter\n", name, s.ns_per_iter / 1e6);
}

#define BENCH_RUN(name, iterations, fn, ctx)                                                       \
    do {                                                                                           \
        bench_sample _s = bench_run((iterations), (fn), (ctx));                                    \
        bench_print((name), _s);                                                                   \
    } while (0)

#endif
