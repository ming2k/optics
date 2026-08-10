/* theme_watch_stub.c — fallback when libsystemd is not available.
 *
 * Reports the live-watching feature as unavailable. Callers fall back to
 * the startup-only iris_query_system_color_scheme().
 */

#include "theme_watch_internal.h"

#ifndef IRIS_HAVE_PORTAL_WATCH

#include <iris/theme.h>

IRIS_API int iris_color_scheme_watch(iris_color_scheme_changed_fn cb, void *user) {
    (void)cb;
    (void)user;
    return -1;
}

IRIS_API void iris_color_scheme_unwatch(void) { /* no-op */ }

int iris_theme__watch_backend(iris_color_scheme_changed_fn cb, void *user) {
    (void)cb;
    (void)user;
    return -1;
}

void iris_theme__unwatch_backend(void) { /* no-op */ }

#endif /* !IRIS_HAVE_PORTAL_WATCH */
