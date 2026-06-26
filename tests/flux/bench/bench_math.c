#include "bench_helpers.h"
#include <flux/flux.h>
#include <stdio.h>
#include <string.h>

#define N BENCH_ITERATIONS

typedef struct {
    flux_mat4 *a;
    flux_mat4 *b;
    flux_mat4 *out;
} mat4_ctx;

static void bench_mat4_multiply(void *p) {
    mat4_ctx *c = p;
    for (int i = 0; i < 100; ++i)
        c->out[i] = flux_mat4_multiply(c->a[i], c->b[i]);
}

static void bench_mat4_invert(void *p) {
    mat4_ctx *c = p;
    for (int i = 0; i < 100; ++i)
        c->out[i] = flux_mat4_invert(c->a[i]);
}

static void bench_mat4_transform_vec4(void *p) {
    mat4_ctx *c = p;
    for (int i = 0; i < 100; ++i) {
        flux_vec4 v = {1.0f, 2.0f, 3.0f, 1.0f};
        flux_mat4_transform_vec4(c->a[i], v);
    }
}

typedef struct {
    flux_mat3x2 *a;
    flux_mat3x2 *b;
    flux_mat3x2 *out;
} mat3x2_ctx;

static void bench_mat3x2_multiply(void *p) {
    mat3x2_ctx *c = p;
    for (int i = 0; i < 100; ++i)
        c->out[i] = flux_mat3x2_multiply(c->a[i], c->b[i]);
}

static void bench_mat3x2_transform_point(void *p) {
    mat3x2_ctx *c = p;
    for (int i = 0; i < 100; ++i) {
        flux_point pt = {100.0f, 200.0f};
        flux_mat3x2_transform_point(c->a[i], pt);
    }
}

typedef struct {
    flux_quat *a;
    flux_quat *b;
    flux_quat *out;
} quat_ctx;

static void bench_quat_slerp(void *p) {
    quat_ctx *c = p;
    for (int i = 0; i < 100; ++i)
        c->out[i] = flux_quat_slerp(c->a[i], c->b[i], 0.5f);
}

static void bench_quat_multiply(void *p) {
    quat_ctx *c = p;
    for (int i = 0; i < 100; ++i)
        c->out[i] = flux_quat_multiply(c->a[i], c->b[i]);
}

static void bench_quat_rotate(void *p) {
    quat_ctx *c = p;
    for (int i = 0; i < 100; ++i) {
        flux_vec3 v = {1.0f, 0.0f, 0.0f};
        flux_quat_rotate(c->a[i], v);
    }
}

typedef struct {
    flux_vec3 *a;
    flux_vec3 *b;
} vec3_ctx;

static void bench_vec3_dot(void *p) {
    volatile float sink;
    vec3_ctx *c = p;
    for (int i = 0; i < 100; ++i)
        sink = flux_vec3_dot(c->a[i], c->b[i]);
    (void)sink;
}

static void bench_vec3_cross(void *p) {
    vec3_ctx *c = p;
    for (int i = 0; i < 100; ++i) {
        flux_vec3 r = flux_vec3_cross(c->a[i], c->b[i]);
        (void)r;
    }
}

static void bench_vec3_normalize(void *p) {
    vec3_ctx *c = p;
    for (int i = 0; i < 100; ++i)
        flux_vec3_normalize(c->a[i]);
}

int main(void) {
    fprintf(stdout, "=== math benchmarks ===\n");

    mat4_ctx m4;
    m4.a = malloc(100 * sizeof(flux_mat4));
    m4.b = malloc(100 * sizeof(flux_mat4));
    m4.out = malloc(100 * sizeof(flux_mat4));
    for (int i = 0; i < 100; ++i) {
        m4.a[i] = flux_mat4_translate((float)i, 1.0f, 2.0f);
        m4.b[i] = flux_mat4_scale(2.0f, 3.0f, 4.0f);
    }

    BENCH_RUN("mat4_multiply", N, bench_mat4_multiply, &m4);
    BENCH_RUN("mat4_invert", N, bench_mat4_invert, &m4);
    BENCH_RUN("mat4_transform_vec4", N, bench_mat4_transform_vec4, &m4);

    free(m4.a);
    free(m4.b);
    free(m4.out);

    mat3x2_ctx m32;
    m32.a = malloc(100 * sizeof(flux_mat3x2));
    m32.b = malloc(100 * sizeof(flux_mat3x2));
    m32.out = malloc(100 * sizeof(flux_mat3x2));
    for (int i = 0; i < 100; ++i) {
        m32.a[i] = flux_mat3x2_translate((float)i, (float)i * 0.5f);
        m32.b[i] = flux_mat3x2_rotate((float)i * 0.01f);
    }

    BENCH_RUN("mat3x2_multiply", N, bench_mat3x2_multiply, &m32);
    BENCH_RUN("mat3x2_transform_point", N, bench_mat3x2_transform_point, &m32);

    free(m32.a);
    free(m32.b);
    free(m32.out);

    quat_ctx q;
    q.a = malloc(100 * sizeof(flux_quat));
    q.b = malloc(100 * sizeof(flux_quat));
    q.out = malloc(100 * sizeof(flux_quat));
    for (int i = 0; i < 100; ++i) {
        q.a[i] = flux_quat_axis_angle((flux_vec3){0, 1, 0}, (float)i * 0.01f);
        q.b[i] = flux_quat_axis_angle((flux_vec3){1, 0, 0}, (float)i * 0.02f);
    }

    BENCH_RUN("quat_slerp", N, bench_quat_slerp, &q);
    BENCH_RUN("quat_multiply", N, bench_quat_multiply, &q);
    BENCH_RUN("quat_rotate", N, bench_quat_rotate, &q);

    free(q.a);
    free(q.b);
    free(q.out);

    vec3_ctx v3;
    v3.a = malloc(100 * sizeof(flux_vec3));
    v3.b = malloc(100 * sizeof(flux_vec3));
    for (int i = 0; i < 100; ++i) {
        v3.a[i] = flux_vec3_make((float)i, 1.0f, 2.0f);
        v3.b[i] = flux_vec3_make(3.0f, 4.0f, (float)i * 0.1f);
    }

    BENCH_RUN("vec3_dot", N, bench_vec3_dot, &v3);
    BENCH_RUN("vec3_cross", N, bench_vec3_cross, &v3);
    BENCH_RUN("vec3_normalize", N, bench_vec3_normalize, &v3);

    free(v3.a);
    free(v3.b);

    fprintf(stdout, "\n");
    return 0;
}
