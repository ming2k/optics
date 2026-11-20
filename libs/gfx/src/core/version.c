#include <optics/gfx/core.h>

void optics_gfx_version(int *major, int *minor, int *patch) {
    if (major)
        *major = OPTICS_GFX_VERSION_MAJOR;
    if (minor)
        *minor = OPTICS_GFX_VERSION_MINOR;
    if (patch)
        *patch = OPTICS_GFX_VERSION_PATCH;
}

uint32_t optics_gfx_version_number(void) {
    return OPTICS_GFX_VERSION_NUMBER;
}

bool optics_gfx_version_check(int major, int minor, int patch) {
    if (major != OPTICS_GFX_VERSION_MAJOR)
        return false;
    if (minor > OPTICS_GFX_VERSION_MINOR)
        return false;
    if (minor == OPTICS_GFX_VERSION_MINOR && patch > OPTICS_GFX_VERSION_PATCH)
        return false;
    return true;
}

#define OPTICS_GFX_STR_(x) #x
#define OPTICS_GFX_STR(x) OPTICS_GFX_STR_(x)

const char *optics_gfx_version_string(void) {
    return OPTICS_GFX_STR(OPTICS_GFX_VERSION_MAJOR) "." OPTICS_GFX_STR(
        OPTICS_GFX_VERSION_MINOR) "." OPTICS_GFX_STR(OPTICS_GFX_VERSION_PATCH);
}
