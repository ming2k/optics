/*
 * prism/version.c — version accessors, all derived from the
 * PRISM_VERSION_* macros (single source of truth; no hardcoded
 * literals — a stale literal here is a bug that no test can catch,
 * which is exactly how vista shipped "0.0.29" for four
 * releases against 0.0.36 headers).
 */

#include <optics/material/types.h>

void optics_material_version(int *major, int *minor, int *patch) {
    if (major)
        *major = OPTICS_MATERIAL_VERSION_MAJOR;
    if (minor)
        *minor = OPTICS_MATERIAL_VERSION_MINOR;
    if (patch)
        *patch = OPTICS_MATERIAL_VERSION_PATCH;
}

uint32_t optics_material_version_number(void) {
    return OPTICS_MATERIAL_VERSION_NUMBER;
}

bool optics_material_version_check(int major, int minor, int patch) {
    if (major != OPTICS_MATERIAL_VERSION_MAJOR)
        return false;
    if (minor > OPTICS_MATERIAL_VERSION_MINOR)
        return false;
    if (minor == OPTICS_MATERIAL_VERSION_MINOR && patch > OPTICS_MATERIAL_VERSION_PATCH)
        return false;
    return true;
}

OPTICS_MATERIAL_API const char *optics_material_version_string(void) {
    return OPTICS_MATERIAL_STRINGIFY(OPTICS_MATERIAL_VERSION_MAJOR) "." OPTICS_MATERIAL_STRINGIFY(
        OPTICS_MATERIAL_VERSION_MINOR) "." OPTICS_MATERIAL_STRINGIFY(OPTICS_MATERIAL_VERSION_PATCH);
}
