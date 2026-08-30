/* test_capability.c — iris_supports() is the feature-discovery contract.
 *
 * Pins three properties:
 *   1. The answer is backend-consistent: what iris_supports() reports
 *      matches the backend this test runs on (wayland on CI).
 *   2. Unknown/newer capability values return false instead of reading
 *      out of the table's bounds (forward compatibility).
 *   3. The backend name is one of the four documented spellings.
 */

#include <stdio.h>
#include <string.h>

#include <iris/iris.h>

static int fails = 0;

#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            fails++;                                                                               \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                        \
        }                                                                                          \
    } while (0)

int main(void) {
    const char *backend = iris_backend_name();
    CHECK(backend != NULL);
    CHECK(strcmp(backend, "wayland") == 0 || strcmp(backend, "win32") == 0 ||
          strcmp(backend, "cocoa") == 0 || strcmp(backend, "none") == 0);

    int is_wayland = strcmp(backend, "wayland") == 0;
    int is_win32 = strcmp(backend, "win32") == 0;

    /* Platform-designed capability answers are load-bearing: an app that
     * shows a "primary selection" affordance on Windows would be wrong. */
    CHECK(iris_supports(IRIS_CAP_PRIMARY_SELECTION) == is_wayland);
    CHECK(iris_supports(IRIS_CAP_TABLET) == is_wayland);
    CHECK(iris_supports(IRIS_CAP_DROP_TARGET) == is_wayland);
    CHECK(iris_supports(IRIS_CAP_DRAG_SOURCE) == is_wayland);
    CHECK(iris_supports(IRIS_CAP_FRACTIONAL_SCALE) == is_win32);

    /* The window/file-dialog capability is backend-level, not OS-level:
     * only the linkable shell (no backend) lacks them. */
    int has_backend = strcmp(backend, "none") != 0;
    CHECK(iris_supports(IRIS_CAP_WINDOW_CONTROL) == has_backend);
    CHECK(iris_supports(IRIS_CAP_FILE_DIALOG) == has_backend);

    /* Forward compatibility: values a newer libiris might define must not
     * index past the table. Exercise well beyond the current range. */
    for (int v = 100; v < 200; v++)
        CHECK(iris_supports((iris_capability)v) == 0);
    CHECK(iris_supports((iris_capability)-1) == 0);
    CHECK(iris_supports((iris_capability)-100000) == 0);

    printf("capability: %s checks on backend=%s\n", fails ? "FAILED" : "ok", backend);
    return fails ? 1 : 0;
}
