/*
 * window/a11y_prefs.h — system accessibility preference query + live watching.
 *
 * The a11y counterpart of theme.h: reads the OS accessibility preferences
 * at startup, and (when the platform supports it) watches for live
 * changes. Three preferences, one struct, one callback — an AT user
 * typically changes them together in the same settings pane, and the
 * delivery plumbing (portal signal / WM_SETTINGCHANGE / DistributedCenter
 * notification) is shared, so they are not split per-field.
 *
 *   reduced_motion  "minimize animation" — drives the UI layer's reduced-motion switch
 *   high_contrast   forced high-contrast theme — hosts re-theme; the window
 *                   backends apply a raised-contrast token set to ui
 *   text_scale      OS "make text bigger" factor — drives
 *                   the UI layer's text scale (a pure font multiplier, see
 *                   ui.h; 1.0 = default)
 *
 * Startup query is always available. Live watching is callback-based with
 * the same threading contract as theme.h: `cb` runs on the window main
 * thread (the thread executing optics_window_run), serialized with the event
 * loop, so it is safe to touch ui from it. When live watching is
 * unavailable, optics_window_a11y_prefs_watch returns non-zero and callers fall
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
#ifndef OPTICS_WINDOW_A11Y_PREFS_H
#define OPTICS_WINDOW_A11Y_PREFS_H

#include <optics/window/loop.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The system accessibility preferences window knows about. Value semantics:
 * read once at startup (optics_window_a11y_prefs_query), then re-read on every
 * callback from optics_window_a11y_prefs_watch. */
typedef struct optics_window_a11y_prefs {
    bool reduced_motion; /* user asked the OS to minimize animation */
    bool high_contrast;  /* user asked for a forced high-contrast theme */
    float text_scale;    /* "make text bigger"; 1.0 = default, > 1 bigger */
} optics_window_a11y_prefs;

/* Query the system accessibility preferences once at call time. Never
 * fails: unreadable fields fall back to the library defaults (false /
 * false / 1.0). Safe to call before optics_window_run. */
OPTICS_WINDOW_API optics_window_a11y_prefs optics_window_a11y_prefs_query(void);

/* Callback invoked whenever any accessibility preference changes.
 * Threading guarantee: `cb` always runs on the window main thread (the
 * thread executing optics_window_run), serialized with the event loop — the
 * same contract as optics_window_color_scheme_changed_fn. Delivery requires a
 * running optics_window_run loop; changes detected while no loop is active
 * are dropped (re-query on the next run).
 *
 * The callback receives the FULL preference set, not a delta: callers
 * apply it unconditionally, so a missed intermediate change cannot leave
 * the UI stuck at a stale half-applied state. */
typedef void (*optics_window_a11y_prefs_changed_fn)(const optics_window_a11y_prefs *prefs,
                                                    void *user);

/* Begin watching the accessibility preferences. Returns:
 *    0  watching started
 *   -1  watching unavailable (no platform support, or the settings source
 *       is unreachable). Caller falls back to optics_window_a11y_prefs_query.
 *
 * The current value is NOT reported at startup — seed with
 * optics_window_a11y_prefs_query. One watcher per process: a second call replaces
 * the registration. Hosts own this slot; the window backends keep their own
 * registration (to drive ui directly) through a separate reserved slot,
 * exactly like the theme watcher. */
OPTICS_WINDOW_API int optics_window_a11y_prefs_watch(optics_window_a11y_prefs_changed_fn cb,
                                                     void *user);

/* Stop watching and release platform resources. Safe to call when not
 * watching. Must be called from the window main thread. */
OPTICS_WINDOW_API void optics_window_a11y_prefs_unwatch(void);

#ifdef __cplusplus
}
#endif

#endif /* OPTICS_WINDOW_A11Y_PREFS_H */
