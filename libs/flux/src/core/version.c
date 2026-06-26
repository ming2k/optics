#include <flux/core.h>

void flux_version(int *major, int *minor, int *patch) {
    if (major)
        *major = FLUX_VERSION_MAJOR;
    if (minor)
        *minor = FLUX_VERSION_MINOR;
    if (patch)
        *patch = FLUX_VERSION_PATCH;
}

uint32_t flux_version_number(void) {
    return FLUX_VERSION_NUMBER;
}

bool flux_version_check(int major, int minor, int patch) {
    if (major != FLUX_VERSION_MAJOR)
        return false;
    if (minor > FLUX_VERSION_MINOR)
        return false;
    if (minor == FLUX_VERSION_MINOR && patch > FLUX_VERSION_PATCH)
        return false;
    return true;
}

#define FLUX_STR_(x) #x
#define FLUX_STR(x) FLUX_STR_(x)

const char *flux_version_string(void) {
    return FLUX_STR(FLUX_VERSION_MAJOR) "." FLUX_STR(FLUX_VERSION_MINOR) "." FLUX_STR(
        FLUX_VERSION_PATCH);
}
