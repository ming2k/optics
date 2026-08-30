/*
 * iris/capability.h — feature discovery (the query the API owed its hosts).
 *
 * Problem this header solves: iris spans three backends with uneven
 * coverage, and before this existed the *degradation policy differed per
 * subsystem* — window/cursor calls were silent no-ops, theme/a11y/watch
 * returned -1, and the file dialogs behaved differently per backend
 * without any of it being written down. A host could not ask "does this
 * build do X?"; it had to try and observe silence, which made
 * cross-platform divergence undetectable by construction.
 *
 * The contract now is two-layered:
 *
 *   1. iris_supports(cap) answers "does this build implement the
 *      capability at all?" — statically, before any app runs, on any
 *      thread. It never fails and never depends on runtime state.
 *      Unknown values (forward-compat from a newer libiris) return false.
 *
 *   2. When a capability is unsupported, the functions it covers degrade
 *      by ONE uniform policy, documented per capability below (see each
 *      iris_capability value). No silent-success, no per-backend
 *      divergence: where the answer is "not available", you either get
 *      the documented inert return value or an explicit negative result.
 *
 * Adding a capability: append to the enum (never insert — the values are
 * part of the ABI), document its degradation contract inline, and wire
 * irisi_supports() in capability.c per backend.
 */
#ifndef IRIS_CAPABILITY_H
#define IRIS_CAPABILITY_H

#include <iris/app.h> /* IRIS_API */

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================== */
/*  Capabilities                                                      */
/* ================================================================== */

typedef enum iris_capability {
    /* Window control (window.h). Degradation: functions are no-ops and
     * iris_window_get_* report zeros; the window itself keeps running.
     * Backends: Wayland (compositor-limited subsets), Win32, Cocoa. */
    IRIS_CAP_WINDOW_CONTROL = 0,

    /* Live theme watching (theme.h). Degradation: iris_color_scheme_watch
     * returns -1 and never invokes the callback; the startup query still
     * works. Backends: Wayland (portal/libsystemd), Win32, Cocoa. */
    IRIS_CAP_THEME_WATCH = 1,

    /* Assistive-technology bridge (a11y.h). Degradation: all iris_a11y_*
     * entry points return -1 and the app is simply not exposed to AT.
     * Backends: Wayland (AT-SPI); Win32/Cocoa pending (ADR-0056 D5). */
    IRIS_CAP_A11Y = 2,

    /* File dialogs (file_dialog.h). Degradation: iris_pick_* return 0
     * (documented "no selection") and iris_file_uri_to_path still works.
     * Backends: Wayland (portal), Win32 (IFileOpen/SaveDialog),
     * Cocoa (NSOpen/SavePanel). */
    IRIS_CAP_FILE_DIALOG = 3,

    /* Clipboard read (paste) + primary selection. Degradation: paste
     * requests never deliver; lens sees an empty clipboard. Primary
     * selection is Wayland-only by platform design (X11 heritage). */
    IRIS_CAP_CLIPBOARD = 4,
    IRIS_CAP_PRIMARY_SELECTION = 5,

    /* Pointer-attached pen/eraser input. Degradation: pen fields stay
     * zero and pen contact synthesizes mouse input. Wayland-only. */
    IRIS_CAP_TABLET = 6,

    /* Drag-and-drop *target* (receiving drops). Degradation: drops never arrive. */
    IRIS_CAP_DROP_TARGET = 7,

    /* Server-side window decorations. Degradation on Wayland without
     * xdg-decoration: the window has no title bar (the app draws its
     * own or runs chromeless); iris reports the condition once on
     * stderr at startup. Win32/Cocoa: always native. */
    IRIS_CAP_DECORATIONS = 8,

    /* Fractional (non-integer) UI scale factors flowing into lens.
     * Degradation: fractional scales are rounded to the nearest integer
     * buffer scale (ADR-0067 follow-on). Win32 reports true per-monitor
     * DPI today; Wayland/Cocoa currently quantize. */
    IRIS_CAP_FRACTIONAL_SCALE = 9,

    /* Drag-and-drop *source* (initiating drags, ADR-0086).
     * Degradation: drag initiation returns -1. */
    IRIS_CAP_DRAG_SOURCE = 10,

    /* Append only. Never repurpose. Unknown values return false from
     * iris_supports() (forward compatibility with a newer libiris). */
} iris_capability;

/* Does this build implement `cap`? Static per-build answer: callable
 * before iris_app_run, from any thread, with no window. Always safe to
 * call with unknown/newer values (returns false). */
IRIS_API int iris_supports(iris_capability cap);

/* Backend this build was configured with (ADR-0044). One of
 * "wayland", "win32", "cocoa", or "none" (linkable shell). Useful for
 * diagnostics; do not branch app behaviour on it — branch on
 * iris_supports() so capability, not platform, drives the UI. */
IRIS_API const char *iris_backend_name(void);

#ifdef __cplusplus
}
#endif

#endif /* IRIS_CAPABILITY_H */
