/* a11y_prefs_stub.c — fallback when no platform watcher backend exists:
 * the libsystemd-less Linux build (no portal pump to ride). The startup
 * query itself still works (a11y_prefs_linux.c shells gsettings/kreadconfig
 * and needs no D-Bus client library), so only watch reports unavailable. */

#include "a11y_prefs_internal.h"

#ifndef IRIS_HAVE_PORTAL_WATCH

IRIS_API int iris_a11y_prefs_watch(iris_a11y_prefs_changed_fn cb, void *user) {
    (void)cb;
    (void)user;
    return -1;
}

IRIS_API void iris_a11y_prefs_unwatch(void) { /* no-op */ }

int iris_a11y_prefs__watch_backend(iris_a11y_prefs_changed_fn cb, void *user) {
    (void)cb;
    (void)user;
    return -1;
}

void iris_a11y_prefs__unwatch_backend(void) { /* no-op */ }

#endif /* !IRIS_HAVE_PORTAL_WATCH */
