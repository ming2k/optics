/* version.c — library version accessors, all derived from the
 * IRIS_VERSION_* macros (single source of truth; no hardcoded
 * literals — a stale literal here is a bug that no test can catch,
 * which is exactly how vista shipped "0.0.29" for four
 * releases against 0.0.36 headers). */

#include <optics/window/loop.h>

#define OPTICS_WINDOW_STR2(x) #x
#define OPTICS_WINDOW_STR(x) OPTICS_WINDOW_STR2(x)

void optics_window_version(int *major, int *minor, int *patch) {
    if (major)
        *major = OPTICS_WINDOW_VERSION_MAJOR;
    if (minor)
        *minor = OPTICS_WINDOW_VERSION_MINOR;
    if (patch)
        *patch = OPTICS_WINDOW_VERSION_PATCH;
}

uint32_t optics_window_version_number(void) {
    return OPTICS_WINDOW_VERSION_NUMBER;
}

bool optics_window_version_check(int major, int minor, int patch) {
    if (major != OPTICS_WINDOW_VERSION_MAJOR)
        return false;
    if (minor > OPTICS_WINDOW_VERSION_MINOR)
        return false;
    if (minor == OPTICS_WINDOW_VERSION_MINOR && patch > OPTICS_WINDOW_VERSION_PATCH)
        return false;
    return true;
}

OPTICS_WINDOW_API const char *optics_window_version_string(void) {
    return OPTICS_WINDOW_STR(OPTICS_WINDOW_VERSION_MAJOR) "." OPTICS_WINDOW_STR(
        OPTICS_WINDOW_VERSION_MINOR) "." OPTICS_WINDOW_STR(OPTICS_WINDOW_VERSION_PATCH);
}
