/* test_helpers.h — minimal assertion harness for the CPU-only tests. */

#ifndef LENS_TEST_HELPERS_H
#define LENS_TEST_HELPERS_H

#include <math.h>
#include <stdio.h>

#include <lens/lens.h>

static inline flux_result test_snapshot_render(lens *ui, flux_canvas *canvas) {
    if (!canvas)
        return FLUX_ERROR_INVALID_ARGUMENT;
    lens_scene_snapshot *snapshot = nullptr;
    flux_result result = lens_snapshot_create(ui, &snapshot);
    if (result == FLUX_OK)
        result = lens_snapshot_submit(snapshot, canvas);
    lens_snapshot_release(snapshot);
    return result;
}

static int g_checks = 0;
static int g_fails = 0;

#define CHECK(cond)                                                                                \
    do {                                                                                           \
        g_checks++;                                                                                \
        if (!(cond)) {                                                                             \
            g_fails++;                                                                             \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                        \
        }                                                                                          \
    } while (0)

#define CHECK_NEAR(a, b, eps)                                                                      \
    do {                                                                                           \
        g_checks++;                                                                                \
        double _a = (double)(a), _b = (double)(b);                                                 \
        if (fabs(_a - _b) > (eps)) {                                                               \
            g_fails++;                                                                             \
            fprintf(stderr, "FAIL %s:%d: %s ~= %s  (%g vs %g)\n", __FILE__, __LINE__, #a, #b, _a,  \
                    _b);                                                                           \
        }                                                                                          \
    } while (0)

#define TEST_REPORT()                                                                              \
    (g_fails ? (fprintf(stderr, "%d/%d checks failed\n", g_fails, g_checks), 1)                    \
             : (printf("ok (%d checks)\n", g_checks), 0))

/* Headless tests explicitly present each completed frame. Tests of delayed
 * presentation use lens_end and activate a chosen snapshot themselves. */
static inline void test_end(lens *ui) {
    lens_end(ui);
    if (lens_overflowed(ui))
        return;
    lens_scene_snapshot *snapshot = nullptr;
    flux_result result = lens_snapshot_create(ui, &snapshot);
    CHECK(result == FLUX_OK);
    if (result == FLUX_OK)
        CHECK(lens_snapshot_activate(ui, snapshot) == FLUX_OK);
    lens_snapshot_release(snapshot);
}

#endif /* LENS_TEST_HELPERS_H */
