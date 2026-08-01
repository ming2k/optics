/* test_version.c — iris_version_string matches the compiled version macros.
 *
 * Headless: pure string compare, no platform deps.
 */

#include "test_helpers.h"
#include <iris/app.h>
#include <string.h>

int main(void) {
    const char *v = iris_version_string();
    CHECK(v != NULL);
    CHECK_STR_EQ(v, "0.0.8");

    /* The string is X.Y.Z with all-numeric components. */
    int maj = 0, min = 0, pat = 0;
    int matched = sscanf(v, "%d.%d.%d", &maj, &min, &pat);
    CHECK(matched == 3);
    CHECK(maj == IRIS_VERSION_MAJOR);
    CHECK(min == IRIS_VERSION_MINOR);
    CHECK(pat == IRIS_VERSION_PATCH);

    return TEST_REPORT();
}
