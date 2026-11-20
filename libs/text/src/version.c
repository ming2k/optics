/* version.c — library version information. */

#include <optics/text.h>

#define OPTICS_GFX_TEXT_STR2(x) #x
#define OPTICS_GFX_TEXT_STR(x) OPTICS_GFX_TEXT_STR2(x)

void optics_text_version(int *major, int *minor, int *patch) {
    if (major)
        *major = OPTICS_TEXT_VERSION_MAJOR;
    if (minor)
        *minor = OPTICS_TEXT_VERSION_MINOR;
    if (patch)
        *patch = OPTICS_TEXT_VERSION_PATCH;
}

uint32_t optics_text_version_number(void) {
    return OPTICS_TEXT_VERSION_NUMBER;
}

bool optics_text_version_check(int major, int minor, int patch) {
    if (major != OPTICS_TEXT_VERSION_MAJOR)
        return false;
    if (minor > OPTICS_TEXT_VERSION_MINOR)
        return false;
    if (minor == OPTICS_TEXT_VERSION_MINOR && patch > OPTICS_TEXT_VERSION_PATCH)
        return false;
    return true;
}

const char *optics_text_version_string(void) {
    return OPTICS_GFX_TEXT_STR(OPTICS_TEXT_VERSION_MAJOR) "." OPTICS_GFX_TEXT_STR(
        OPTICS_TEXT_VERSION_MINOR) "." OPTICS_GFX_TEXT_STR(OPTICS_TEXT_VERSION_PATCH);
}
