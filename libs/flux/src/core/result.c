#include <flux/core.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* thread_local is a C23 keyword; no <threads.h> needed (MinGW/MSVC lack it). */

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
/* Backing store for formatted messages (FLUX_FAILF). The error slot borrows
 * this buffer, so its contents stay valid exactly as the public contract
 * states: until the next non-OK return on this thread. */
static thread_local char g_error_message[512];

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

/* Compose a diagnostic into this thread's error storage, then publish it.
 * The caller's format arguments are stack temporaries; the rendered text
 * outlives them. */
void flux_set_last_errorf(flux_result code, const char *function, const char *file, int line,
                          int32_t backend_code, const char *fmt, ...) {
    if (fmt) {
        va_list ap;
        va_start(ap, fmt);
        // NOLINTNEXTLINE(clang-analyzer-valist.Uninitialized): va_start initializes ap
        vsnprintf(g_error_message, sizeof(g_error_message), fmt, ap);
        va_end(ap);
    } else {
        g_error_message[0] = '\0';
    }
    flux_set_last_error(code, function, file, line, fmt ? g_error_message : nullptr, backend_code);
}

/* Public seam for a consumer library's own validation failures (see core.h).
 * Delegates to the same slot so a caller's `flux_get_last_error` cannot tell
 * flux-native and consumer-detected failures apart — which is the point: the
 * binding reads one contract either way. */
void flux_set_diagnostic(flux_result code, const char *function, const char *file, int line,
                         const char *message) {
    flux_set_last_error(code, function, file, line, message, 0);
}

const char *flux_diagnostic_format(const char *fmt, ...) {
    if (!fmt) {
        g_error_message[0] = '\0';
        return g_error_message;
    }
    va_list ap;
    va_start(ap, fmt);
    // NOLINTNEXTLINE(clang-analyzer-valist.Uninitialized): va_start initializes ap
    vsnprintf(g_error_message, sizeof(g_error_message), fmt, ap);
    va_end(ap);
    return g_error_message;
}
