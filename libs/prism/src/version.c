/*
 * prism/version.c — version string derived from PRISM_VERSION_* macros.
 */

#include <prism/types.h>

PRISM_API const char *prism_version_string(void) {
    return PRISM_STRINGIFY(PRISM_VERSION_MAJOR) "." PRISM_STRINGIFY(PRISM_VERSION_MINOR) "." PRISM_STRINGIFY(
        PRISM_VERSION_PATCH);
}
