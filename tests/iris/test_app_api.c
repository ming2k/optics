/* test_app_api.c — argument validation on the public entry points.
 *
 * Headless: every call here must fail fast WITHOUT opening a window or
 * connecting to a compositor.
 *
 *   - iris_app_run(NULL) is rejected at the dispatcher before any backend
 *     work. (A non-NULL config always dispatches to the live backend and
 *     opens a window, so it is NOT exercised here.)
 *   - file and folder pickers reject a NULL buffer / zero capacity without
 *     contacting the desktop portal.
 */

#include "test_helpers.h"
#include <iris/iris.h>

static bool host_start(lens *ui, flux_device *device, void *user) {
    return ui != NULL && device != NULL && user != NULL;
}

static void host_stop(lens *ui, flux_device *device, void *user) {
    (void)ui;
    (void)device;
    (void)user;
}

int main(void) {
    /* Lifecycle callbacks are part of the public, designated-init API.
     * Do not run this config headlessly; assigning it catches declaration
     * and function-signature regressions at compile time. */
    int user = 1;
    iris_app_config lifecycle_config = {
        .start = host_start,
        .stop = host_stop,
        .user = &user,
    };
    CHECK(lifecycle_config.start != NULL);
    CHECK(lifecycle_config.stop != NULL);

    /* iris_app_run: NULL config is rejected before any backend work. */
    CHECK(iris_app_run(NULL) != 0);

    /* Safe outside a running app; the dispatcher/backend seam treats this
     * exactly like a request made after the window has closed. */
    iris_request_animation_frame();

    /* Pickers reject NULL buffers / zero capacity before contacting the
     * portal. A valid buffer would open a live desktop picker. */
    CHECK(iris_pick_file(NULL, NULL, 0) != 0);
    char buf[16];
    CHECK(iris_pick_file(NULL, buf, 0) != 0);
    CHECK(iris_pick_file(NULL, NULL, sizeof buf) != 0);
    CHECK(iris_pick_folder(NULL, NULL, 0) != 0);
    CHECK(iris_pick_folder(NULL, buf, 0) != 0);
    CHECK(iris_pick_folder(NULL, NULL, sizeof buf) != 0);

    return TEST_REPORT();
}
