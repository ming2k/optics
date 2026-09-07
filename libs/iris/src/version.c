/* version.c — library version accessors, all derived from the
 * IRIS_VERSION_* macros (single source of truth; no hardcoded
 * literals — a stale literal here is a bug that no test can catch,
 * which is exactly how flux-scene-graph shipped "0.0.29" for four
 * releases against 0.0.36 headers). */

#include <iris/app.h>

#define IRIS_STR2(x) #x
#define IRIS_STR(x) IRIS_STR2(x)

void iris_version(int *major, int *minor, int *patch) {
    if (major)
        *major = IRIS_VERSION_MAJOR;
    if (minor)
        *minor = IRIS_VERSION_MINOR;
    if (patch)
        *patch = IRIS_VERSION_PATCH;
}

uint32_t iris_version_number(void) {
    return IRIS_VERSION_NUMBER;
}

bool iris_version_check(int major, int minor, int patch) {
    if (major != IRIS_VERSION_MAJOR)
        return false;
    if (minor > IRIS_VERSION_MINOR)
        return false;
    if (minor == IRIS_VERSION_MINOR && patch > IRIS_VERSION_PATCH)
        return false;
    return true;
}

IRIS_API const char *iris_version_string(void) {
    return IRIS_STR(IRIS_VERSION_MAJOR) "." IRIS_STR(IRIS_VERSION_MINOR) "." IRIS_STR(
        IRIS_VERSION_PATCH);
}
