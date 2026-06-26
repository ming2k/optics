/* test_helpers.h — minimal assertion harness for the iris C tests.
 *
 * Mirrors lens's libs/lens/tests/test_helpers.h so the whole stack shares
 * one test style: CHECK accumulates failures, TEST_REPORT() prints a
 * summary and returns the process exit code.
 */
#ifndef IRIS_TEST_HELPERS_H
#define IRIS_TEST_HELPERS_H

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

#define CHECK_STR_EQ(actual, expected)                                                             \
    do {                                                                                           \
        g_checks++;                                                                                \
        const char *_a = (actual), *_e = (expected);                                               \
        if ((_a) == NULL || (_e) == NULL || strcmp(_a, _e) != 0) {                                 \
            g_fails++;                                                                             \
            fprintf(stderr, "FAIL %s:%d: %s == %s  (\"%s\" vs \"%s\")\n", __FILE__, __LINE__,      \
                    #actual, #expected, _a ? _a : "(null)", _e ? _e : "(null)");                   \
        }                                                                                          \
    } while (0)

#define TEST_REPORT()                                                                              \
    (g_fails ? (fprintf(stderr, "%d/%d checks failed\n", g_fails, g_checks), 1)                    \
             : (printf("ok (%d checks)\n", g_checks), 0))

#endif /* IRIS_TEST_HELPERS_H */
