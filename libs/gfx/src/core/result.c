#include <optics/gfx/core.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* thread_local is a C23 keyword. */

const char *optics_gfx_result_string(optics_gfx_result r) {
    switch (r) {
    case OPTICS_GFX_OK:
        return "OPTICS_GFX_OK";
    case OPTICS_GFX_ERROR_INVALID_ARGUMENT:
        return "OPTICS_GFX_ERROR_INVALID_ARGUMENT";
    case OPTICS_GFX_ERROR_OUT_OF_MEMORY:
        return "OPTICS_GFX_ERROR_OUT_OF_MEMORY";
    case OPTICS_GFX_ERROR_OUT_OF_RANGE:
        return "OPTICS_GFX_ERROR_OUT_OF_RANGE";
    case OPTICS_GFX_ERROR_INVALID_STATE:
        return "OPTICS_GFX_ERROR_INVALID_STATE";
    case OPTICS_GFX_ERROR_UNSUPPORTED:
        return "OPTICS_GFX_ERROR_UNSUPPORTED";
    case OPTICS_GFX_ERROR_BACKEND_FAILURE:
        return "OPTICS_GFX_ERROR_BACKEND_FAILURE";
    case OPTICS_GFX_ERROR_DEVICE_LOST:
        return "OPTICS_GFX_ERROR_DEVICE_LOST";
    case OPTICS_GFX_ERROR_SURFACE_LOST:
        return "OPTICS_GFX_ERROR_SURFACE_LOST";
    case OPTICS_GFX_ERROR_TIMEOUT:
        return "OPTICS_GFX_ERROR_TIMEOUT";
    case OPTICS_GFX_ERROR_BUSY:
        return "OPTICS_GFX_ERROR_BUSY";
    }
    return "OPTICS_GFX_ERROR_UNKNOWN";
}

static thread_local optics_gfx_error_info g_last_error;
/* Backing store for formatted messages (OPTICS_GFX_FAILF). The error slot borrows
 * this buffer, so its contents stay valid exactly as the public contract
 * states: until the next non-OK return on this thread. */
static thread_local char g_error_message[512];

void optics_gfx_get_last_error(optics_gfx_error_info *out) {
    if (!out)
        return;
    *out = g_last_error;
}

/* Internal — called by implementations on failure. Not exported. */
void optics_gfx_set_last_error(optics_gfx_result code, const char *function, const char *file,
                               int line, const char *message, int32_t backend_code) {
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
void optics_gfx_set_last_errorf(optics_gfx_result code, const char *function, const char *file,
                                int line, int32_t backend_code, const char *fmt, ...) {
    if (fmt) {
        va_list ap;
        va_start(ap, fmt);
        // NOLINTNEXTLINE(clang-analyzer-valist.Uninitialized): va_start initializes ap
        vsnprintf(g_error_message, sizeof(g_error_message), fmt, ap);
        va_end(ap);
    } else {
        g_error_message[0] = '\0';
    }
    optics_gfx_set_last_error(code, function, file, line, fmt ? g_error_message : nullptr,
                              backend_code);
}

/* Public seam for a consumer library's own validation failures (see core.h).
 * Delegates to the same slot so a caller's `optics_gfx_get_last_error` cannot tell
 * gfx-native and consumer-detected failures apart — which is the point: the
 * binding reads one contract either way. */
void optics_gfx_set_diagnostic(const optics_gfx_error_info *info) {
    if (!info)
        return;
    optics_gfx_set_last_error(info->code, info->function, info->file, info->line, info->message,
                              info->backend_code);
}

const char *optics_gfx_diagnostic_format(const char *fmt, ...) {
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
