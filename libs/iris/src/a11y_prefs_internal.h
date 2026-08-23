/* a11y_prefs_internal.h — shared header for the accessibility-preference
 * watcher sources.
 *
 * Houses the backend-reserved watcher slot, mirroring
 * theme_watch_internal.h: the public iris_a11y_prefs_watch slot belongs to
 * the host; the platform backends (app_wayland.c / app_win32.c /
 * app_cocoa.m) register through iris_a11y_prefs__watch_backend so they can
 * drive lens directly without consuming the host's registration.
 *
 * Delivery contract: the callback runs on the iris main thread, serialized
 * with the event loop (same guarantee as iris_color_scheme_changed_fn).
 *
 * Platform notify seams (no fd to poll; the OS pushes):
 *   Win32   iris_a11y_prefs_win32__notify_setting_change() — from
 *           WM_SETTINGCHANGE in app_win32.c's WndProc.
 *   Cocoa   iris_a11y_prefs_cocoa__install() — registers the distributed
 *           notification observers on the app's main run loop.
 *   Linux   the shared portal pump in theme_watch_portal.c (no extra seam).
 */
#ifndef IRIS_A11Y_PREFS_INTERNAL_H
#define IRIS_A11Y_PREFS_INTERNAL_H

#include <iris/a11y_prefs.h>

/* Backend-reserved counterparts of iris_a11y_prefs_watch/unwatch. Only the
 * platform backends call these. Same return/delivery semantics as the
 * public API; independent of the host's public registration. */
int iris_a11y_prefs__watch_backend(iris_a11y_prefs_changed_fn cb, void *user);
void iris_a11y_prefs__unwatch_backend(void);

#if defined(_WIN32)
/* Called from app_win32.c's WndProc on WM_SETTINGCHANGE (main thread). */
void iris_a11y_prefs_win32__notify_setting_change(void);
#endif

#endif /* IRIS_A11Y_PREFS_INTERNAL_H */
