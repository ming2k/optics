/* test_version.c — iris_version_string matches the compiled version macros.
 *
 * Headless: pure string compare, no platform deps.
 */

#include "optics_test.h"
#include <optics/window/loop.h>
#include <stdio.h>
#include <string.h>

int main(void) {
    const char *v = optics_window_version_string();
    OT_EXPECT(v != nullptr);

    /* Exact string built from the macros, so a release bump cannot leave
     * this test holding a stale hardcoded literal. */
    char expect[32];
    snprintf(expect, sizeof expect, "%d.%d.%d", OPTICS_WINDOW_VERSION_MAJOR,
             OPTICS_WINDOW_VERSION_MINOR, OPTICS_WINDOW_VERSION_PATCH);
    OT_EXPECT_STR_EQ(v, expect);

    /* The string is X.Y.Z with all-numeric components. */
    int maj = 0, min = 0, pat = 0;
    int matched = sscanf(v, "%d.%d.%d", &maj, &min, &pat);
    OT_EXPECT(matched == 3);
    OT_EXPECT(maj == OPTICS_WINDOW_VERSION_MAJOR);
    OT_EXPECT(min == OPTICS_WINDOW_VERSION_MINOR);
    OT_EXPECT(pat == OPTICS_WINDOW_VERSION_PATCH);

    return OT_REPORT();
}
