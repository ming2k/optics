/* test_helpers.h — minimal assertion harness for the CPU-only tests.
 * Same shape as tests/lens/test_helpers.h (each suite is standalone). */

#ifndef ANIM_TEST_HELPERS_H
#define ANIM_TEST_HELPERS_H

#include <math.h>
#include <stdio.h>

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

#endif /* ANIM_TEST_HELPERS_H */
