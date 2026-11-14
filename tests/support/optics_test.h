/*
 * optics_test.h — the one assertion and reporting harness for every C test.
 *
 * A test binary is one Meson test. It may contain several helper functions, but
 * it reports one process exit code: 0 = pass, 1 = failure, 77 = skipped
 * (Meson's protocol). Every layer (unit, integration, e2e) uses this header; a
 * suite-local copy of CHECK/EXPECT is test-infrastructure rot.
 *
 * Contracts:
 *   - Assertions never allocate and never abort. They count, print one line to
 *     stderr, and continue, so one run names every broken invariant.
 *   - OT_REQUIRE is the fatal form for a precondition that makes later checks
 *     meaningless (a failed create, a missing buffer). It reports and ends the
 *     current test binary; there is exactly one test case per process, so this
 *     is the case boundary.
 *   - OT_SKIP reports an honest Meson skip instead of a silent pass. A missing
 *     Vulkan ICD, validation layer, or platform facility is a skip, not a test
 *     that vacuously succeeded.
 *   - Diagnostics go to stderr. The harness never writes stdout; stdout belongs
 *     to the program under test (and to benchmarks).
 */

#ifndef OPTICS_TEST_H
#define OPTICS_TEST_H

#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Process-wide accounting for one test binary. */
void optics_test_check(void);
void optics_test_fail(const char *file, int line, const char *expression, const char *format, ...);
void optics_test_fail_near(const char *file, int line, const char *a_text, double a,
                           const char *b_text, double b, double epsilon);
void optics_test_fail_str(const char *file, int line, const char *actual_text, const char *actual,
                          const char *expected_text, const char *expected);
void optics_test_fail_mem(const char *file, int line, const char *actual_text, const void *actual,
                          const char *expected_text, const void *expected, size_t size,
                          size_t differ_at);
void optics_test_skip(const char *reason);
int optics_test_report(const char *file);
int optics_test_checks(void);
int optics_test_failures(void);
bool optics_test_skipped(void);

/* Count one check without asserting. Used by diagnostics that compose their
 * own failure message (for example, a multi-field leak report). */
#define OT_MANUAL_CHECK() optics_test_check()

/* Count one manual failure. Prefer OT_EXPECT; this exists for loops that build
 * a richer message than a single expression can carry. */
#define OT_MANUAL_FAIL() optics_test_fail(__FILE__, __LINE__, nullptr, nullptr)

#define OT_CHECKS() optics_test_checks()
#define OT_FAILURES() optics_test_failures()

#define OT_EXPECT(condition)                                                                       \
    do {                                                                                           \
        optics_test_check();                                                                       \
        if (!(condition))                                                                          \
            optics_test_fail(__FILE__, __LINE__, #condition, nullptr);                             \
    } while (0)

#define OT_EXPECT_NEAR(actual, expected, epsilon)                                                  \
    do {                                                                                           \
        optics_test_check();                                                                       \
        double _ot_a = (double)(actual);                                                           \
        double _ot_b = (double)(expected);                                                         \
        double _ot_e = (double)(epsilon);                                                          \
        if (!(fabs(_ot_a - _ot_b) <= _ot_e))                                                       \
            optics_test_fail_near(__FILE__, __LINE__, #actual, _ot_a, #expected, _ot_b, _ot_e);    \
    } while (0)

#define OT_EXPECT_STR_EQ(actual, expected)                                                         \
    do {                                                                                           \
        optics_test_check();                                                                       \
        const char *_ot_a = (actual);                                                              \
        const char *_ot_e = (expected);                                                            \
        if (_ot_a == nullptr || _ot_e == nullptr || strcmp(_ot_a, _ot_e) != 0)                     \
            optics_test_fail_str(__FILE__, __LINE__, #actual, _ot_a, #expected, _ot_e);            \
    } while (0)

#define OT_EXPECT_MEM_EQ(actual, expected, size)                                                   \
    do {                                                                                           \
        optics_test_check();                                                                       \
        const void *_ot_a = (actual);                                                              \
        const void *_ot_e = (expected);                                                            \
        size_t _ot_n = (size_t)(size);                                                             \
        size_t _ot_at = 0;                                                                         \
        bool _ot_equal = _ot_a != nullptr && _ot_e != nullptr;                                     \
        if (_ot_equal) {                                                                           \
            const uint8_t *_ot_ab = (const uint8_t *)_ot_a;                                        \
            const uint8_t *_ot_eb = (const uint8_t *)_ot_e;                                        \
            for (; _ot_at < _ot_n; ++_ot_at) {                                                     \
                if (_ot_ab[_ot_at] != _ot_eb[_ot_at]) {                                            \
                    _ot_equal = false;                                                             \
                    break;                                                                         \
                }                                                                                  \
            }                                                                                      \
        }                                                                                          \
        if (!_ot_equal)                                                                            \
            optics_test_fail_mem(__FILE__, __LINE__, #actual, _ot_a, #expected, _ot_e, _ot_n,      \
                                 _ot_at);                                                          \
    } while (0)

/* Fatal precondition: report and end this test process. */
#define OT_REQUIRE(condition)                                                                      \
    do {                                                                                           \
        bool _ot_require_ok = (condition);                                                         \
        OT_EXPECT(_ot_require_ok);                                                                 \
        if (!_ot_require_ok)                                                                       \
            exit(OT_REPORT());                                                                     \
    } while (0)

#define OT_SKIP(reason)                                                                            \
    do {                                                                                           \
        optics_test_skip(reason);                                                                  \
        exit(OT_REPORT());                                                                         \
    } while (0)

#define OT_SKIP_IF(condition, reason)                                                              \
    do {                                                                                           \
        if (condition) {                                                                           \
            optics_test_skip(reason);                                                              \
            exit(OT_REPORT());                                                                     \
        }                                                                                          \
    } while (0)

/* Report and return the process exit code. Every test main ends with this. */
#define OT_REPORT() optics_test_report(__FILE__)

#ifdef __cplusplus
}
#endif

#endif /* OPTICS_TEST_H */
