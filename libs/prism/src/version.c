/*
 * prism/version.c — version accessors, all derived from the
 * PRISM_VERSION_* macros (single source of truth; no hardcoded
 * literals — a stale literal here is a bug that no test can catch,
 * which is exactly how flux-scene-graph shipped "0.0.29" for four
 * releases against 0.0.36 headers).
 */

#include <prism/types.h>

void prism_version(int *major, int *minor, int *patch) {
    if (major)
        *major = PRISM_VERSION_MAJOR;
    if (minor)
        *minor = PRISM_VERSION_MINOR;
    if (patch)
        *patch = PRISM_VERSION_PATCH;
}

uint32_t prism_version_number(void) {
    return PRISM_VERSION_NUMBER;
}

bool prism_version_check(int major, int minor, int patch) {
    if (major != PRISM_VERSION_MAJOR)
        return false;
    if (minor > PRISM_VERSION_MINOR)
        return false;
    if (minor == PRISM_VERSION_MINOR && patch > PRISM_VERSION_PATCH)
        return false;
    return true;
}

PRISM_API const char *prism_version_string(void) {
    return PRISM_STRINGIFY(PRISM_VERSION_MAJOR) "." PRISM_STRINGIFY(
        PRISM_VERSION_MINOR) "." PRISM_STRINGIFY(PRISM_VERSION_PATCH);
}
