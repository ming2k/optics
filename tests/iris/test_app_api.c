/* test_app_api.c — argument validation on the public entry points.
 *
 * Headless: every call here must fail fast WITHOUT opening a window or
 * connecting to a compositor.
 *
 *   - iris_app_run(NULL) is rejected at the dispatcher before any backend
 *     work. (A non-NULL config always dispatches to the live backend and
 *     opens a window, so it is NOT exercised here.)
 *   - iris_pick_file rejects a NULL buffer / zero capacity without forking
 *     the portal subprocess.
 */

#include "test_helpers.h"
#include <iris/iris.h>

int main(void) {
    /* iris_app_run: NULL config is rejected before any backend work. */
    CHECK(iris_app_run(NULL) != 0);

    /* iris_pick_file: NULL buffer / zero capacity are rejected without
     * forking the portal subprocess. (A non-zero cap with a real buffer
     * would fork gdbus and block on the desktop picker — never call that
     * from a unit test.) */
    CHECK(iris_pick_file(NULL, NULL, 0) != 0);
    char buf[16];
    CHECK(iris_pick_file(NULL, buf, 0) != 0);
    CHECK(iris_pick_file(NULL, NULL, sizeof buf) != 0);

    return TEST_REPORT();
}
