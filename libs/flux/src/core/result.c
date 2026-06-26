#include <flux/core.h>
#include <string.h>
#include <threads.h>

const char *flux_result_string(flux_result r) {
    switch (r) {
    case FLUX_OK:
        return "FLUX_OK";
    case FLUX_ERROR_INVALID_ARGUMENT:
        return "FLUX_ERROR_INVALID_ARGUMENT";
    case FLUX_ERROR_OUT_OF_MEMORY:
        return "FLUX_ERROR_OUT_OF_MEMORY";
    case FLUX_ERROR_OUT_OF_RANGE:
        return "FLUX_ERROR_OUT_OF_RANGE";
    case FLUX_ERROR_INVALID_STATE:
        return "FLUX_ERROR_INVALID_STATE";
    case FLUX_ERROR_UNSUPPORTED:
        return "FLUX_ERROR_UNSUPPORTED";
    case FLUX_ERROR_BACKEND_FAILURE:
        return "FLUX_ERROR_BACKEND_FAILURE";
    case FLUX_ERROR_DEVICE_LOST:
        return "FLUX_ERROR_DEVICE_LOST";
    case FLUX_ERROR_SURFACE_LOST:
        return "FLUX_ERROR_SURFACE_LOST";
    case FLUX_ERROR_TIMEOUT:
        return "FLUX_ERROR_TIMEOUT";
    }
    return "FLUX_ERROR_UNKNOWN";
}

static thread_local flux_error_info g_last_error;

void flux_get_last_error(flux_error_info *out) {
    if (!out)
        return;
    *out = g_last_error;
}

/* Internal — called by implementations on failure. Not exported. */
void flux_set_last_error(flux_result code, const char *function, const char *file, int line,
                         const char *message, int32_t backend_code) {
    g_last_error.code = code;
    g_last_error.function = function;
    g_last_error.file = file;
    g_last_error.line = line;
    g_last_error.message = message;
    g_last_error.backend_code = backend_code;
}
