#include "internal.h"
#include <flux/core.h>

#include <stdarg.h>
#include <stdio.h>

void flux_console_logger(flux_log_level level, const char *file, int line, const char *fmt,
                         const char *msg, void *user) {
    (void)user;
    (void)fmt;

    const char *tag;
    switch (level) {
    case FLUX_LOG_TRACE:
        tag = "TRACE";
        break;
    case FLUX_LOG_DEBUG:
        tag = "DEBUG";
        break;
    case FLUX_LOG_INFO:
        tag = "INFO";
        break;
    case FLUX_LOG_WARN:
        tag = "WARN";
        break;
    case FLUX_LOG_ERROR:
        tag = "ERROR";
        break;
    default:
        tag = "?";
        break;
    }

    /* All levels go to stderr — a library's diagnostics must not
     * pollute the consumer's stdout. */
    if (file) {
        fprintf(stderr, "[flux %s] %s:%d: %s\n", tag, file, line, msg ? msg : "");
    } else {
        fprintf(stderr, "[flux %s] %s\n", tag, msg ? msg : "");
    }
}

void flux_device_log(flux_device *d, flux_log_level level, const char *category, const char *fmt,
                     ...) {
    if (!d || !d->log || !fmt)
        return;

    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    d->log(level, category ? category : "flux", 0, "%s", buf, d->log_user);
}
