/* test_version.c — iris_version_string matches the compiled version macros.
 *
 * Headless: pure string compare, no platform deps.
 */

#include "test_helpers.h"
#include <iris/app.h>
#include <stdio.h>
#include <string.h>

int main(void) {
    const char *v = iris_version_string();
    CHECK(v != NULL);

    /* Exact string built from the macros, so a release bump cannot leave
     * this test holding a stale hardcoded literal. */
    char expect[32];
    snprintf(expect, sizeof expect, "%d.%d.%d", IRIS_VERSION_MAJOR, IRIS_VERSION_MINOR,
             IRIS_VERSION_PATCH);
    CHECK_STR_EQ(v, expect);

    /* The string is X.Y.Z with all-numeric components. */
    int maj = 0, min = 0, pat = 0;
    int matched = sscanf(v, "%d.%d.%d", &maj, &min, &pat);
    CHECK(matched == 3);
    CHECK(maj == IRIS_VERSION_MAJOR);
    CHECK(min == IRIS_VERSION_MINOR);
    CHECK(pat == IRIS_VERSION_PATCH);

    return TEST_REPORT();
}
