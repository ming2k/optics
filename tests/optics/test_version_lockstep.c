/*
 * test_version_lockstep.c — verify every enabled library's runtime accessors
 * and compile-time macros agree with Meson's project version
 * (ADR-0016 / ADR-0106).
 *
 * Meson injects OPTICS_PROJECT_VERSION_{MAJOR,MINOR,PATCH} and one
 * OPTICS_HAVE_<LIB> define per optional library. The runtime half proves:
 *   - version(), version_number(), and version_string() agree with the macros;
 *   - version_string() is derived from the macros, not a stale literal;
 *   - the packed integer has major in bits 16..23, minor 8..15, patch 0..7;
 *   - version_check() accepts the current version and rejects any newer
 *     major/minor/patch combination.
 *
 * The Cargo workspace is checked by tools/check_versions.py (lint), which sees
 * surfaces this C binary cannot.
 */

#ifndef OPTICS_PROJECT_VERSION_MAJOR
#error "Meson must inject OPTICS_PROJECT_VERSION_MAJOR"
#endif

#include "optics_test.h"

#include <optics/optics.h>

#include <string.h>

#define CHECK_PROJECT_VERSION(PREFIX)                                                              \
    static_assert(PREFIX##_VERSION_MAJOR == OPTICS_PROJECT_VERSION_MAJOR,                          \
                  #PREFIX " major differs from meson.project_version()");                          \
    static_assert(PREFIX##_VERSION_MINOR == OPTICS_PROJECT_VERSION_MINOR,                          \
                  #PREFIX " minor differs from meson.project_version()");                          \
    static_assert(PREFIX##_VERSION_PATCH == OPTICS_PROJECT_VERSION_PATCH,                          \
                  #PREFIX " patch differs from meson.project_version()")

CHECK_PROJECT_VERSION(OPTICS_GFX);
CHECK_PROJECT_VERSION(OPTICS);
CHECK_PROJECT_VERSION(OPTICS_WINDOW);

#if defined(OPTICS_HAVE_UI)
CHECK_PROJECT_VERSION(OPTICS_UI);
#endif

#if defined(OPTICS_HAVE_TEXT)
CHECK_PROJECT_VERSION(OPTICS_TEXT);
#endif

#define CHECK_VERSION_SUITE(prefix, PREFIX)                                                        \
    do {                                                                                           \
        int M = 0, m = 0, p = 0;                                                                   \
        prefix##_version(&M, &m, &p);                                                              \
        OT_EXPECT(M == PREFIX##_VERSION_MAJOR);                                                    \
        OT_EXPECT(m == PREFIX##_VERSION_MINOR);                                                    \
        OT_EXPECT(p == PREFIX##_VERSION_PATCH);                                                    \
        OT_EXPECT(prefix##_version_number() == PREFIX##_VERSION_NUMBER);                           \
        OT_EXPECT(prefix##_version_number() ==                                                     \
                  ((uint32_t)PREFIX##_VERSION_MAJOR << 16 |                                        \
                   (uint32_t)PREFIX##_VERSION_MINOR << 8 | (uint32_t)PREFIX##_VERSION_PATCH));     \
        OT_EXPECT(prefix##_version_check(PREFIX##_VERSION_MAJOR, PREFIX##_VERSION_MINOR,           \
                                         PREFIX##_VERSION_PATCH));                                 \
        OT_EXPECT(!prefix##_version_check(PREFIX##_VERSION_MAJOR + 1, 0, 0));                      \
        OT_EXPECT(!prefix##_version_check(PREFIX##_VERSION_MAJOR, PREFIX##_VERSION_MINOR + 1, 0)); \
        OT_EXPECT(!prefix##_version_check(PREFIX##_VERSION_MAJOR, PREFIX##_VERSION_MINOR,          \
                                          PREFIX##_VERSION_PATCH + 1));                            \
        const char *s = prefix##_version_string();                                                 \
        char expect[32];                                                                           \
        snprintf(expect, sizeof expect, "%d.%d.%d", PREFIX##_VERSION_MAJOR,                        \
                 PREFIX##_VERSION_MINOR, PREFIX##_VERSION_PATCH);                                  \
        OT_EXPECT(strcmp(s, expect) == 0);                                                         \
    } while (0)

int main(void) {
    CHECK_VERSION_SUITE(optics_gfx, OPTICS_GFX);

    OT_EXPECT(OPTICS_VERSION_NUMBER ==
              ((uint32_t)OPTICS_VERSION_MAJOR << 16 | (uint32_t)OPTICS_VERSION_MINOR << 8 |
               (uint32_t)OPTICS_VERSION_PATCH));
    OT_EXPECT(OPTICS_GFX_VERSION_NUMBER == OPTICS_VERSION_NUMBER);

    CHECK_VERSION_SUITE(optics_window, OPTICS_WINDOW);
    OT_EXPECT(OPTICS_WINDOW_VERSION_NUMBER == OPTICS_VERSION_NUMBER);

#if defined(OPTICS_HAVE_UI)
    CHECK_VERSION_SUITE(optics_ui, OPTICS_UI);
    OT_EXPECT(OPTICS_UI_VERSION_NUMBER == OPTICS_VERSION_NUMBER);
#endif

#if defined(OPTICS_HAVE_TEXT)
    CHECK_VERSION_SUITE(optics_text, OPTICS_TEXT);
    OT_EXPECT(OPTICS_TEXT_VERSION_NUMBER == OPTICS_VERSION_NUMBER);
#endif

    return OT_REPORT();
}
