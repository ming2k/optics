/* theme_watch_stub.c — fallback when libsystemd is not available.
 *
 * Reports the live-watching feature as unavailable. Callers fall back to
 * the startup-only optics_window_query_system_color_scheme().
 */

#include "theme_watch_internal.h"

#ifndef OPTICS_WINDOW_HAVE_PORTAL_WATCH

#include <optics/window/theme.h>

OPTICS_WINDOW_API int optics_window_color_scheme_watch(optics_window_color_scheme_changed_fn cb,
                                                       void *user) {
    (void)cb;
    (void)user;
    return -1;
}

OPTICS_WINDOW_API void optics_window_color_scheme_unwatch(void) { /* no-op */ }

int optics_window_theme__watch_backend(optics_window_color_scheme_changed_fn cb, void *user) {
    (void)cb;
    (void)user;
    return -1;
}

void optics_window_theme__unwatch_backend(void) { /* no-op */ }

#endif /* !OPTICS_WINDOW_HAVE_PORTAL_WATCH */
