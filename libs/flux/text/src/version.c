/* version.c — library version string. */

#include <flux-text/text.h>

#define FLUX_TEXT_STR2(x) #x
#define FLUX_TEXT_STR(x) FLUX_TEXT_STR2(x)

const char *flux_text_version_string(void) {
    return FLUX_TEXT_STR(FLUX_TEXT_VERSION_MAJOR) "." FLUX_TEXT_STR(
        FLUX_TEXT_VERSION_MINOR) "." FLUX_TEXT_STR(FLUX_TEXT_VERSION_PATCH);
}
