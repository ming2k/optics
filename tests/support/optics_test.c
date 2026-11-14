/*
 * optics_test.c — process accounting for the shared test harness.
 *
 * One static definition per test executable (each binary links this static
 * library once). No global heap use: counters and the skip reason are plain
 * process state.
 */
#include "optics_test.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static int g_checks;
static int g_failures;
static bool g_skipped;
static char g_skip_reason[512];

void optics_test_check(void) {
    g_checks++;
}

void optics_test_fail(const char *file, int line, const char *expression, const char *format, ...) {
    g_checks++;
    g_failures++;
    fprintf(stderr, "FAIL %s:%d", file, line);
    if (expression)
        fprintf(stderr, ": %s", expression);
    if (format) {
        fputc(':', stderr);
        va_list args;
        va_start(args, format);
        vfprintf(stderr, format, args);
        va_end(args);
    }
    fputc('\n', stderr);
}

void optics_test_fail_near(const char *file, int line, const char *a_text, double a,
                           const char *b_text, double b, double epsilon) {
    g_checks++;
    g_failures++;
    fprintf(stderr, "FAIL %s:%d: %s (%.9g) ~= %s (%.9g) epsilon=%.9g\n", file, line, a_text, a,
            b_text, b, epsilon);
}

void optics_test_fail_str(const char *file, int line, const char *actual_text, const char *actual,
                          const char *expected_text, const char *expected) {
    g_checks++;
    g_failures++;
    fprintf(stderr, "FAIL %s:%d: %s == %s (\"%s\" vs \"%s\")\n", file, line, actual_text,
            expected_text, actual ? actual : "(null)", expected ? expected : "(null)");
}

void optics_test_fail_mem(const char *file, int line, const char *actual_text, const void *actual,
                          const char *expected_text, const void *expected, size_t size,
                          size_t differ_at) {
    g_checks++;
    g_failures++;
    if (!actual || !expected) {
        fprintf(stderr, "FAIL %s:%d: %s == %s (null buffer, size=%zu)\n", file, line, actual_text,
                expected_text, size);
        return;
    }
    size_t at = differ_at < size ? differ_at : size - (size > 0 ? 1u : 0u);
    const uint8_t *a = (const uint8_t *)actual;
    const uint8_t *b = (const uint8_t *)expected;
    if (size == 0)
        fprintf(stderr, "FAIL %s:%d: %s == %s (zero-length buffers)\n", file, line, actual_text,
                expected_text);
    else
        fprintf(stderr, "FAIL %s:%d: %s == %s (first difference at byte %zu: 0x%02x vs 0x%02x)\n",
                file, line, actual_text, expected_text, at, a[at], b[at]);
}

void optics_test_skip(const char *reason) {
    g_skipped = true;
    snprintf(g_skip_reason, sizeof g_skip_reason, "%s", reason ? reason : "unspecified");
}

int optics_test_report(const char *file) {
    if (g_failures == 0 && g_skipped) {
        fprintf(stderr, "SKIP %s: %s\n", file, g_skip_reason);
        return 77;
    }
    fprintf(stderr, "%s: %d/%d checks passed%s\n", file, g_checks - g_failures, g_checks,
            g_skipped ? " (with skipped checks)" : "");
    return g_failures ? 1 : 0;
}

int optics_test_checks(void) {
    return g_checks;
}

int optics_test_failures(void) {
    return g_failures;
}

bool optics_test_skipped(void) {
    return g_skipped;
}
