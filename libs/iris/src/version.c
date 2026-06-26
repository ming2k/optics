/* version.c — library version string. */

#include <iris/app.h>

#define IRIS_STR2(x) #x
#define IRIS_STR(x) IRIS_STR2(x)

IRIS_API const char *iris_version_string(void) {
    return IRIS_STR(IRIS_VERSION_MAJOR) "." IRIS_STR(IRIS_VERSION_MINOR) "." IRIS_STR(
        IRIS_VERSION_PATCH);
}
