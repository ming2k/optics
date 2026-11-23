/*
 * e2e_gfx_version — the smallest public-API contract: the umbrella version
 * and the gfx runtime version agree in every feature configuration.
 *
 * This scenario deliberately needs neither ui nor a canvas, so the e2e layer
 * remains populated even in a gfx-only distribution.
 */

#include "optics_test.h"

#include <optics/optics.h>

#include <string.h>

int main(void) {
    static_assert(OPTICS_VERSION_MAJOR == OPTICS_GFX_VERSION_MAJOR,
                  "umbrella and gfx major versions diverge");
    static_assert(OPTICS_VERSION_MINOR == OPTICS_GFX_VERSION_MINOR,
                  "umbrella and gfx minor versions diverge");
    static_assert(OPTICS_VERSION_PATCH == OPTICS_GFX_VERSION_PATCH,
                  "umbrella and gfx patch versions diverge");
    static_assert(OPTICS_VERSION_NUMBER == OPTICS_GFX_VERSION_NUMBER,
                  "umbrella and gfx packed versions diverge");

    OT_EXPECT(optics_gfx_version_number() == OPTICS_GFX_VERSION_NUMBER);
    OT_EXPECT(
        optics_gfx_version_check(OPTICS_VERSION_MAJOR, OPTICS_VERSION_MINOR, OPTICS_VERSION_PATCH));

    int major = -1, minor = -1, patch = -1;
    optics_gfx_version(&major, &minor, &patch);
    OT_EXPECT(major == OPTICS_VERSION_MAJOR);
    OT_EXPECT(minor == OPTICS_VERSION_MINOR);
    OT_EXPECT(patch == OPTICS_VERSION_PATCH);

    char expected[32];
    snprintf(expected, sizeof expected, "%d.%d.%d", OPTICS_VERSION_MAJOR, OPTICS_VERSION_MINOR,
             OPTICS_VERSION_PATCH);
    OT_EXPECT_STR_EQ(optics_gfx_version_string(), expected);

    return OT_REPORT();
}
