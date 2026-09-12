/*
 * Version accessors across every library in the stack.
 *
 * All libraries share one versioning contract (docs/reference/api.md
 * "Library versioning"): four accessors under a per-library prefix,
 * all derived from that library's *_VERSION_* macros, all kept in
 * lockstep with meson.project_version() by
 * tools/check-version-lockstep.sh. This test pins the runtime side:
 *
 *   - version() round-trips the header macros exactly (a stale
 *     hard-coded string like the scene-graph "0.0.29" incident fails
 *     here, because the string must match the compile-time macros);
 *   - version_number() packs major/minor/patch in the documented bit
 *     layout and is identical across all libraries (one scheme);
 *   - version_check() accepts the current version and rejects any
 *     newer major/minor/patch combination (the consumer's gate).
 *
 * CPU-only, no device: this is the consumer-facing compatibility
 * surface, exercised in the cheapest possible way.
 */
#include "test_helpers.h"
#include <anim/anim.h>
#include <flux-scene-graph/scene-graph.h>
#include <flux-text/text.h>
#include <flux/flux.h>
#include <iris/app.h>
#include <lens/lens.h>
#include <prism/prism.h>

#include <string.h>

/* One library's four accessors against its header macros: the runtime
 * must agree with the compile-time macros exactly (a stale hard-coded
 * string like the scene-graph "0.0.29" incident fails here), the
 * packed number must use the documented bit layout, and version_check
 * must accept current + older and reject anything newer. */
#define CHECK_VERSION_SUITE(prefix, PREFIX)                                                        \
    do {                                                                                           \
        int M = 0, m = 0, p = 0;                                                                   \
        prefix##_version(&M, &m, &p);                                                              \
        EXPECT(M == PREFIX##_VERSION_MAJOR);                                                       \
        EXPECT(m == PREFIX##_VERSION_MINOR);                                                       \
        EXPECT(p == PREFIX##_VERSION_PATCH);                                                       \
        EXPECT(prefix##_version_number() == PREFIX##_VERSION_NUMBER);                              \
        EXPECT(prefix##_version_number() ==                                                        \
               ((uint32_t)PREFIX##_VERSION_MAJOR << 16 | (uint32_t)PREFIX##_VERSION_MINOR << 8 |   \
                (uint32_t)PREFIX##_VERSION_PATCH));                                                \
        EXPECT(prefix##_version_check(PREFIX##_VERSION_MAJOR, PREFIX##_VERSION_MINOR,              \
                                      PREFIX##_VERSION_PATCH));                                    \
        EXPECT(!prefix##_version_check(PREFIX##_VERSION_MAJOR + 1, 0, 0));                         \
        EXPECT(!prefix##_version_check(PREFIX##_VERSION_MAJOR, PREFIX##_VERSION_MINOR + 1, 0));    \
        EXPECT(!prefix##_version_check(PREFIX##_VERSION_MAJOR, PREFIX##_VERSION_MINOR,             \
                                       PREFIX##_VERSION_PATCH + 1));                               \
        /* Older-or-equal passes: same major, lower minor. */                                      \
        EXPECT(prefix##_version_check(PREFIX##_VERSION_MAJOR, 0, 0));                              \
        const char *s = prefix##_version_string();                                                 \
        EXPECT(s != NULL && s[0] != '\0');                                                         \
        /* The string must be exactly "M.m.p" — catches hard-coded                               \
         * literals that lag the macros (scene-graph shipped "0.0.29"                              \
         * against 0.0.36 for four releases). */                                                   \
        char expect[32];                                                                           \
        snprintf(expect, sizeof expect, "%d.%d.%d", PREFIX##_VERSION_MAJOR,                        \
                 PREFIX##_VERSION_MINOR, PREFIX##_VERSION_PATCH);                                  \
        EXPECT(strcmp(s, expect) == 0);                                                            \
    } while (0)

int main(void) {
    {
        int M = 0, m = 0, p = 0;
        flux_version(&M, &m, &p);
        EXPECT(M == FLUX_VERSION_MAJOR);
        EXPECT(m == FLUX_VERSION_MINOR);
        EXPECT(p == FLUX_VERSION_PATCH);
        EXPECT(flux_version_number() == FLUX_VERSION_NUMBER);
        EXPECT(flux_version_check(FLUX_VERSION_MAJOR, FLUX_VERSION_MINOR, FLUX_VERSION_PATCH));
        EXPECT(!flux_version_check(FLUX_VERSION_MAJOR + 1, 0, 0));
        const char *s = flux_version_string();
        char expect[32];
        snprintf(expect, sizeof expect, "%d.%d.%d", FLUX_VERSION_MAJOR, FLUX_VERSION_MINOR,
                 FLUX_VERSION_PATCH);
        EXPECT(strcmp(s, expect) == 0);
    }
    CHECK_VERSION_SUITE(lens, LENS);
    CHECK_VERSION_SUITE(iris, IRIS);
    CHECK_VERSION_SUITE(prism, PRISM);
    CHECK_VERSION_SUITE(anim, ANIM);
    CHECK_VERSION_SUITE(flux_sg, FLUX_SG);
    CHECK_VERSION_SUITE(flux_text, FLUX_TEXT);

    /* One packed-number scheme across the whole stack: a consumer can
     * compare every library's version_number() with the same code. */
    EXPECT(FLUX_VERSION_NUMBER ==
           ((uint32_t)FLUX_VERSION_MAJOR << 16 | (uint32_t)FLUX_VERSION_MINOR << 8 |
            (uint32_t)FLUX_VERSION_PATCH));
    EXPECT(FLUX_VERSION_NUMBER == LENS_VERSION_NUMBER);
    EXPECT(FLUX_VERSION_NUMBER == IRIS_VERSION_NUMBER);
    EXPECT(FLUX_VERSION_NUMBER == PRISM_VERSION_NUMBER);
    EXPECT(FLUX_VERSION_NUMBER == ANIM_VERSION_NUMBER);
    EXPECT(FLUX_VERSION_NUMBER == FLUX_SG_VERSION_NUMBER);
    EXPECT(FLUX_VERSION_NUMBER == FLUX_TEXT_VERSION_NUMBER);

    TEST_SUMMARY();
}
