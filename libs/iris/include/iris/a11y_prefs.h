/*
 * iris/a11y_prefs.h — system accessibility preference query + live watching.
 *
 * The a11y counterpart of theme.h: reads the OS accessibility preferences
 * at startup, and (when the platform supports it) watches for live
 * changes. Three preferences, one struct, one callback — an AT user
 * typically changes them together in the same settings pane, and the
 * delivery plumbing (portal signal / WM_SETTINGCHANGE / DistributedCenter
 * notification) is shared, so they are not split per-field.
 *
 *   reduced_motion  "minimize animation" — drives lens_set_reduced_motion
 *   high_contrast   forced high-contrast theme — hosts re-theme; the iris
 *                   backends apply a raised-contrast token set to lens
 *   text_scale      OS "make text bigger" factor — drives
 *                   lens_set_text_scale (a pure font multiplier, see
 *                   lens.h; 1.0 = default)
 *
 * Startup query is always available. Live watching is callback-based with
 * the same threading contract as theme.h: `cb` runs on the iris main
 * thread (the thread executing iris_app_run), serialized with the event
 * loop, so it is safe to touch lens from it. When live watching is
 * unavailable, iris_a11y_prefs_watch returns non-zero and callers fall
 * back to the startup-only query.
 *
 * Sources per platform (ADR-0075):
 *   Linux   xdg-desktop-portal Settings namespace "org.freedesktop.appearance":
 *           keys color-scheme (contrast), and the GNOME/KDE text-scaling /
 *           reduce-animation keys under "org.gnome.desktop.interface"
 *           (text-scaling-factor, enable-animations) via the same portal —
 *           one SettingChanged subscription covers all of them.
 *   Windows SPI_GETCLIENTAREAANIMATION (reduced motion),
 *           SPI_GETHIGHCONTRASTW (high contrast),
 *           SystemParametersInfo(SPI_GETNONCLIENTMETRICS / registry
 *           FontSize) — delivered by WM_SETTINGCHANGE.
 *   macOS    NSWorkspace.accessibilityDisplayShouldReduceMotion,
 *           accessibilityDisplayIsHighContrastEnabled,
 *           NSDistributedNotificationCenter notifications.
 *
 * All accessors are honest about absence: text_scale reports 1.0 and the
 * bools report false when a preference cannot be read — the caller's UI
 * then simply runs at library defaults, which is the correct accessible
 * baseline, never a crash or a stall.
 */
#ifndef IRIS_A11Y_PREFS_H
#define IRIS_A11Y_PREFS_H

#include <iris/app.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The system accessibility preferences iris knows about. Value semantics:
 * read once at startup (iris_a11y_prefs_query), then re-read on every
 * callback from iris_a11y_prefs_watch. */
typedef struct iris_a11y_prefs {
    bool reduced_motion; /* user asked the OS to minimize animation */
    bool high_contrast;  /* user asked for a forced high-contrast theme */
    float text_scale;    /* "make text bigger"; 1.0 = default, > 1 bigger */
} iris_a11y_prefs;

/* Query the system accessibility preferences once at call time. Never
 * fails: unreadable fields fall back to the library defaults (false /
 * false / 1.0). Safe to call before iris_app_run. */
IRIS_API iris_a11y_prefs iris_a11y_prefs_query(void);

/* Callback invoked whenever any accessibility preference changes.
 * Threading guarantee: `cb` always runs on the iris main thread (the
 * thread executing iris_app_run), serialized with the event loop — the
 * same contract as iris_color_scheme_changed_fn. Delivery requires a
 * running iris_app_run loop; changes detected while no loop is active
 * are dropped (re-query on the next run).
 *
 * The callback receives the FULL preference set, not a delta: callers
 * apply it unconditionally, so a missed intermediate change cannot leave
 * the UI stuck at a stale half-applied state. */
typedef void (*iris_a11y_prefs_changed_fn)(const iris_a11y_prefs *prefs, void *user);

/* Begin watching the accessibility preferences. Returns:
 *    0  watching started
 *   -1  watching unavailable (no platform support, or the settings source
 *       is unreachable). Caller falls back to iris_a11y_prefs_query.
 *
 * The current value is NOT reported at startup — seed with
 * iris_a11y_prefs_query. One watcher per process: a second call replaces
 * the registration. Hosts own this slot; the iris backends keep their own
 * registration (to drive lens directly) through a separate reserved slot,
 * exactly like the theme watcher. */
IRIS_API int iris_a11y_prefs_watch(iris_a11y_prefs_changed_fn cb, void *user);

/* Stop watching and release platform resources. Safe to call when not
 * watching. Must be called from the iris main thread. */
IRIS_API void iris_a11y_prefs_unwatch(void);

#ifdef __cplusplus
}
#endif

#endif /* IRIS_A11Y_PREFS_H */
