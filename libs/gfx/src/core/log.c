#include "internal.h"
#include <optics/gfx/core.h>

#include <stdarg.h>
#include <stdio.h>

void optics_gfx_console_logger(optics_gfx_log_level level, const char *file, int line,
                               const char *fmt, const char *msg, void *user) {
    (void)user;
    (void)fmt;

    const char *tag;
    switch (level) {
    case OPTICS_GFX_LOG_TRACE:
        tag = "TRACE";
        break;
    case OPTICS_GFX_LOG_DEBUG:
        tag = "DEBUG";
        break;
    case OPTICS_GFX_LOG_INFO:
        tag = "INFO";
        break;
    case OPTICS_GFX_LOG_WARN:
        tag = "WARN";
        break;
    case OPTICS_GFX_LOG_ERROR:
        tag = "ERROR";
        break;
    default:
        tag = "?";
        break;
    }

    /* All levels go to stderr — a library's diagnostics must not
     * pollute the consumer's stdout. */
    if (file) {
        fprintf(stderr, "[optics_gfx %s] %s:%d: %s\n", tag, file, line, msg ? msg : "");
    } else {
        fprintf(stderr, "[optics_gfx %s] %s\n", tag, msg ? msg : "");
    }
}

void optics_gfx_device_log(optics_gfx_device *d, optics_gfx_log_level level, const char *category,
                           const char *fmt, ...) {
    if (!d || !d->log || !fmt)
        return;

    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    d->log(level, category ? category : "optics_gfx", 0, "%s", buf, d->log_user);
}
