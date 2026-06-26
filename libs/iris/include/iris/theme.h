/*
 * iris/theme.h — system colour-scheme query + live watching.
 *
 * Reads the user's colour-scheme preference at startup, and (when libsystemd
 * is available at build time) watches for live changes via the
 * org.freedesktop.portal.Settings D-Bus signal SettingChanged.
 *
 * Startup query is always available. Live watching requires:
 *   - libsystemd (sd-bus) at build time
 *   - an xdg-desktop-portal backend at runtime
 *
 * When live watching is unavailable, iris_watch_system_color_scheme
 * returns non-zero and callers fall back to the startup-only behaviour
 * (no live updates; restart the app to pick up a change).
 */
#ifndef IRIS_THEME_H
#define IRIS_THEME_H

#include <iris/app.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum iris_color_scheme {
    IRIS_COLOR_SCHEME_NO_PREFERENCE = 0,
    IRIS_COLOR_SCHEME_PREFER_DARK = 1,
    IRIS_COLOR_SCHEME_PREFER_LIGHT = 2,
} iris_color_scheme;

/* Query the system colour scheme once at call time. Returns
 * IRIS_COLOR_SCHEME_PREFER_DARK when no preference is discoverable
 * (safe default for a UI library). */
IRIS_API iris_color_scheme iris_query_system_color_scheme(void);

/* Convenience: returns true when the system prefers dark OR no preference. */
IRIS_API bool iris_system_prefers_dark(void);

/* ================================================================== */
/*  Live watching (optional; requires libsystemd at build time)       */
/* ================================================================== */

/* Callback invoked whenever the system colour scheme changes. Runs on the
 * thread that pumps the watcher (see iris_pump_color_scheme_watcher). */
typedef void (*iris_color_scheme_changed_fn)(iris_color_scheme new_scheme, void *user);

/* Begin watching the system colour scheme. Returns:
 *    0  watching started; pump the fd returned by
 *       iris_color_scheme_watcher_fd and call
 *       iris_pump_color_scheme_watcher when readable
 *   -1  watching unavailable (libsystemd not linked, or D-Bus / portal
 *       unreachable). Caller should fall back to startup-only query.
 *
 * `cb` is invoked on the first detected change after this call. The current
 * value is NOT reported at startup — call iris_query_system_color_scheme
 * to seed it. */
IRIS_API int iris_watch_system_color_scheme(iris_color_scheme_changed_fn cb, void *user);

/* The fd to poll(2) for readability, or -1 when not watching. When the fd
 * is readable, call iris_pump_color_scheme_watcher to drain and dispatch. */
IRIS_API int iris_color_scheme_watcher_fd(void);

/* The poll(2) event mask to wait on for the watcher fd, as reported by the
 * underlying D-Bus connection (sd_bus_get_events). This is NOT a fixed POLLIN:
 * an sd-bus socket is level-triggered and may report POLLIN at the kernel even
 * when sd-bus has no work, so polling a hard-coded POLLIN spins the event loop.
 * Callers must use this mask for the fd's `events` field. Returns 0 when not
 * watching (caller should then skip the fd). */
IRIS_API short iris_color_scheme_watcher_poll_events(void);

/* Drain pending D-Bus messages and dispatch the callback registered via
 * iris_watch_system_color_scheme if the colour scheme changed. Safe to
 * call spuriously (no-op when nothing is pending). */
IRIS_API void iris_pump_color_scheme_watcher(void);

/* Stop watching and release D-Bus resources. Safe to call when not watching. */
IRIS_API void iris_stop_color_scheme_watcher(void);

#ifdef __cplusplus
}
#endif

#endif /* IRIS_THEME_H */
