/* version.c — library version information. */

#include <glyph/glyph.h>

#define FLUX_TEXT_STR2(x) #x
#define FLUX_TEXT_STR(x) FLUX_TEXT_STR2(x)

void glyph_version(int *major, int *minor, int *patch) {
    if (major)
        *major = GLYPH_VERSION_MAJOR;
    if (minor)
        *minor = GLYPH_VERSION_MINOR;
    if (patch)
        *patch = GLYPH_VERSION_PATCH;
}

uint32_t glyph_version_number(void) {
    return GLYPH_VERSION_NUMBER;
}

bool glyph_version_check(int major, int minor, int patch) {
    if (major != GLYPH_VERSION_MAJOR)
        return false;
    if (minor > GLYPH_VERSION_MINOR)
        return false;
    if (minor == GLYPH_VERSION_MINOR && patch > GLYPH_VERSION_PATCH)
        return false;
    return true;
}

const char *glyph_version_string(void) {
    return FLUX_TEXT_STR(GLYPH_VERSION_MAJOR) "." FLUX_TEXT_STR(
        GLYPH_VERSION_MINOR) "." FLUX_TEXT_STR(GLYPH_VERSION_PATCH);
}
