/* test_helpers.h — the minimal assertion harness for the CPU-only svg tests.
 *
 * Deliberately header-only and self-contained: every svg test is a
 * device-less binary, so it must not drag in flux's Vulkan-aware helper
 * library (tests/flux/test_helpers.c) just to count failures. */

#ifndef SVG_TEST_HELPERS_H
#define SVG_TEST_HELPERS_H

#include <math.h>
#include <stdio.h>
#include <string.h>

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
        if (!(fabs(_a - _b) <= (eps))) {                                                           \
            g_fails++;                                                                             \
            fprintf(stderr, "FAIL %s:%d: %s ~= %s  (%g vs %g)\n", __FILE__, __LINE__, #a, #b, _a,  \
                    _b);                                                                           \
        }                                                                                          \
    } while (0)

#define CHECK_STR(a, b)                                                                            \
    do {                                                                                           \
        g_checks++;                                                                                \
        const char *_a = (a), *_b = (b);                                                           \
        if (!_a || !_b || strcmp(_a, _b) != 0) {                                                   \
            g_fails++;                                                                             \
            fprintf(stderr, "FAIL %s:%d: \"%s\" != \"%s\"\n", __FILE__, __LINE__,                  \
                    _a ? _a : "(null)", _b ? _b : "(null)");                                       \
        }                                                                                          \
    } while (0)

#define TEST_REPORT()                                                                              \
    do {                                                                                           \
        fprintf(stderr, "%s: %d/%d passed\n", __FILE__, g_checks - g_fails, g_checks);             \
        return g_fails ? 1 : 0;                                                                    \
    } while (0)

#endif /* SVG_TEST_HELPERS_H */
