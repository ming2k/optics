/* version.c — library version information. */

#include <flux-text/text.h>

#define FLUX_TEXT_STR2(x) #x
#define FLUX_TEXT_STR(x) FLUX_TEXT_STR2(x)

void flux_text_version(int *major, int *minor, int *patch) {
    if (major)
        *major = FLUX_TEXT_VERSION_MAJOR;
    if (minor)
        *minor = FLUX_TEXT_VERSION_MINOR;
    if (patch)
        *patch = FLUX_TEXT_VERSION_PATCH;
}

uint32_t flux_text_version_number(void) {
    return FLUX_TEXT_VERSION_NUMBER;
}

bool flux_text_version_check(int major, int minor, int patch) {
    if (major != FLUX_TEXT_VERSION_MAJOR)
        return false;
    if (minor > FLUX_TEXT_VERSION_MINOR)
        return false;
    if (minor == FLUX_TEXT_VERSION_MINOR && patch > FLUX_TEXT_VERSION_PATCH)
        return false;
    return true;
}

const char *flux_text_version_string(void) {
    return FLUX_TEXT_STR(FLUX_TEXT_VERSION_MAJOR) "." FLUX_TEXT_STR(
        FLUX_TEXT_VERSION_MINOR) "." FLUX_TEXT_STR(FLUX_TEXT_VERSION_PATCH);
}
