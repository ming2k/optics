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

CHECK_PROJECT_VERSION(FLUX);
CHECK_PROJECT_VERSION(OPTICS);

#if defined(OPTICS_HAVE_IRIS)
CHECK_PROJECT_VERSION(IRIS);
#endif
#if defined(OPTICS_HAVE_LENS)
CHECK_PROJECT_VERSION(LENS);
#endif

#if defined(OPTICS_HAVE_GLYPH)
CHECK_PROJECT_VERSION(GLYPH);
#endif
#if defined(OPTICS_HAVE_PRISM)
CHECK_PROJECT_VERSION(PRISM);
#endif
#if defined(OPTICS_HAVE_RETICLE)
CHECK_PROJECT_VERSION(RETICLE);
#endif
#if defined(OPTICS_HAVE_TRANSIT)
CHECK_PROJECT_VERSION(TRANSIT);
#endif
#if defined(OPTICS_HAVE_VISTA)
CHECK_PROJECT_VERSION(VISTA);
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
    CHECK_VERSION_SUITE(flux, FLUX);

    OT_EXPECT(OPTICS_VERSION_NUMBER ==
              ((uint32_t)OPTICS_VERSION_MAJOR << 16 | (uint32_t)OPTICS_VERSION_MINOR << 8 |
               (uint32_t)OPTICS_VERSION_PATCH));
    OT_EXPECT(FLUX_VERSION_NUMBER == OPTICS_VERSION_NUMBER);

#if defined(OPTICS_HAVE_LENS)
    CHECK_VERSION_SUITE(lens, LENS);
    OT_EXPECT(LENS_VERSION_NUMBER == OPTICS_VERSION_NUMBER);
#endif
#if defined(OPTICS_HAVE_IRIS)
    CHECK_VERSION_SUITE(iris, IRIS);
    OT_EXPECT(IRIS_VERSION_NUMBER == OPTICS_VERSION_NUMBER);
#endif

#if defined(OPTICS_HAVE_GLYPH)
    CHECK_VERSION_SUITE(glyph, GLYPH);
    OT_EXPECT(GLYPH_VERSION_NUMBER == OPTICS_VERSION_NUMBER);
#endif
#if defined(OPTICS_HAVE_PRISM)
    CHECK_VERSION_SUITE(prism, PRISM);
    OT_EXPECT(PRISM_VERSION_NUMBER == OPTICS_VERSION_NUMBER);
#endif
#if defined(OPTICS_HAVE_RETICLE)
    CHECK_VERSION_SUITE(reticle, RETICLE);
    OT_EXPECT(RETICLE_VERSION_NUMBER == OPTICS_VERSION_NUMBER);
#endif
#if defined(OPTICS_HAVE_TRANSIT)
    CHECK_VERSION_SUITE(transit, TRANSIT);
    OT_EXPECT(TRANSIT_VERSION_NUMBER == OPTICS_VERSION_NUMBER);
#endif
#if defined(OPTICS_HAVE_VISTA)
    CHECK_VERSION_SUITE(vista, VISTA);
    OT_EXPECT(VISTA_VERSION_NUMBER == OPTICS_VERSION_NUMBER);
#endif

    return OT_REPORT();
}
