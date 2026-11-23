/*
 * window/theme.h — system colour-scheme query + live watching.
 *
 * Reads the user's colour-scheme preference at startup, and (when the
 * platform supports it) watches for live changes.
 *
 * Startup query is always available. Live watching is callback-based: the
 * platform watches the OS setting on its own (on Linux, a dedicated thread
 * pumps the org.freedesktop.portal.Settings D-Bus signal SettingChanged)
 * and delivers changes on the window main thread — the thread running
 * optics_window_run — via the backend's wakeup seam. No fd, poll mask, or pump
 * call is exposed here; each backend wires delivery into its own event
 * loop (Wayland: poll; Win32: thread messages; Cocoa: CFRunLoop).
 *
 * When live watching is unavailable, optics_window_color_scheme_watch returns
 * non-zero and callers fall back to the startup-only behaviour (no live
 * updates; restart the app to pick up a change).
 */
#ifndef OPTICS_WINDOW_THEME_H
#define OPTICS_WINDOW_THEME_H

#include <optics/window/loop.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum optics_window_color_scheme : uint32_t {
    OPTICS_WINDOW_COLOR_SCHEME_NO_PREFERENCE = 0,
    OPTICS_WINDOW_COLOR_SCHEME_PREFER_DARK = 1,
    OPTICS_WINDOW_COLOR_SCHEME_PREFER_LIGHT = 2,
} optics_window_color_scheme;

/* Query the system colour scheme once at call time. Returns
 * OPTICS_WINDOW_COLOR_SCHEME_PREFER_DARK when no preference is discoverable
 * (safe default for a UI library). */
OPTICS_WINDOW_API optics_window_color_scheme optics_window_query_system_color_scheme(void);

/* Convenience: returns true when the system prefers dark OR no preference. */
OPTICS_WINDOW_API bool optics_window_system_prefers_dark(void);

/* ================================================================== */
/*  Live watching (optional; platform support varies)                 */
/* ================================================================== */

/* Callback invoked whenever the system colour scheme changes.
 *
 * Threading guarantee: `cb` always runs on the window main thread (the
 * thread executing optics_window_run), serialized with the event loop — it is
 * safe to touch ui or any other main-thread-affine state from it.
 * Delivery requires a running optics_window_run loop: changes detected while
 * no loop is active are dropped. */
typedef void (*optics_window_color_scheme_changed_fn)(optics_window_color_scheme new_scheme,
                                                      void *user);

/* Begin watching the system colour scheme. Returns:
 *    0  watching started
 *   -1  watching unavailable (no platform support, or the OS settings
 *       source is unreachable). Caller should fall back to startup-only
 *       query.
 *
 * `cb` is invoked on the first detected change after this call. The current
 * value is NOT reported at startup — call optics_window_query_system_color_scheme
 * to seed it.
 *
 * One watcher per process: calling this again while watching replaces the
 * callback and user pointer (and returns 0). Must be called from the window
 * main thread.
 *
 * This slot belongs to the HOST. The platform backend keeps its own
 * internal registration (to re-apply the ui theme live) through a
 * separate reserved slot, so a host watch/unwatch never disturbs the
 * backend's, and backend teardown never cancels the host's. */
OPTICS_WINDOW_API int optics_window_color_scheme_watch(optics_window_color_scheme_changed_fn cb,
                                                       void *user);

/* Stop watching and release platform resources. Blocks until the
 * platform's watcher thread (if any) has exited; `cb` is guaranteed not
 * to run after this returns. Safe to call when not watching. Must be
 * called from the window main thread. */
OPTICS_WINDOW_API void optics_window_color_scheme_unwatch(void);

#ifdef __cplusplus
}
#endif

#endif /* OPTICS_WINDOW_THEME_H */
