/* theme_watch_stub.c — fallback when libsystemd is not available.
 *
 * Reports the live-watching feature as unavailable. Callers fall back to
 * the startup-only iris_query_system_color_scheme().
 */

#include "theme_watch_internal.h"

#ifndef IRIS_HAVE_PORTAL_WATCH

#include <iris/theme.h>

IRIS_API int iris_watch_system_color_scheme(iris_color_scheme_changed_fn cb, void *user) {
    (void)cb;
    (void)user;
    return -1;
}

IRIS_API int iris_color_scheme_watcher_fd(void) {
    return -1;
}

IRIS_API short iris_color_scheme_watcher_poll_events(void) {
    return 0;
}

IRIS_API void iris_pump_color_scheme_watcher(void) { /* no-op */ }

IRIS_API void iris_stop_color_scheme_watcher(void) { /* no-op */ }

#endif /* !IRIS_HAVE_PORTAL_WATCH */
