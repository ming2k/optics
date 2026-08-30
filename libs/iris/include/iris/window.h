/*
 * iris/window.h — host-driven window state for the active app.
 *
 * iris owns the window; hosts request state changes through this API.
 * The window operated on is the one opened by the most recent
 * iris_app_run call on the calling thread ("the active window").
 *
 * All calls are thread-affine to iris_app_run and a no-op outside an
 * active app. Compositor policy is honoured: a tiling WM may ignore
 * minimize / maximize requests, and fullscreen may be deferred until
 * the user confirms. The window state observable to the host is
 * whatever the compositor actually configures.
 *
 * On backends without the underlying capability the calls degrade to
 * no-ops (no error is reported) — see ADR-0036 for the seam rationale.
 */
#ifndef IRIS_WINDOW_H
#define IRIS_WINDOW_H

#include <stdbool.h>
#include <stdint.h>

#include <iris/app.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================== */
/*  Window state                                                       */
/* ================================================================== */

/* Minimize (iconify) the active window. */
IRIS_API void iris_window_minimize(void);

/* Ask the compositor to maximise the active window. */
IRIS_API void iris_window_maximize(void);

/* Ask the compositor to un-maximise the active window. */
IRIS_API void iris_window_unmaximize(void);

/* Enter fullscreen on the output the active window is currently on. */
IRIS_API void iris_window_fullscreen(void);

/* Leave fullscreen. Restores the prior floating geometry. */
IRIS_API void iris_window_unfullscreen(void);

/* Restore the window from minimised / maximised state (when allowed).
 * Equivalent to calling unmaximize + unfullscreen; provided as a
 * convenience for window menu items. */
IRIS_API void iris_window_restore(void);

/* Request keyboard focus (raise / activate). Subject to compositor
 * policy; most compositors restrict focus stealing without a user
 * activation token. */
IRIS_API void iris_window_focus(void);

/* Request the compositor to close the active window. iris_app_run
 * returns as if the user had clicked the close button. */
IRIS_API void iris_window_close(void);

/* Request the compositor / window manager to start an interactive move
 * of the active window (e.g. initiated by a pointer drag on a custom
 * title bar or tab bar in client-side decoration mode).
 *
 * On Wayland, this issues an xdg_toplevel.move request using the seat and
 * serial of the current pointer interaction. On backends without
 * interactive move support, this call is a safe no-op. */
IRIS_API void iris_window_start_move(void);

/* ================================================================== */
/*  Size hints                                                         */
/* ================================================================== */

/* Configure the minimum and maximum logical window size the compositor
 * should respect. Pass 0 to clear the respective bound. Compositors
 * combine these with their own constraints (display size, layout, …). */
IRIS_API void iris_window_set_min_size(int32_t width, int32_t height);
IRIS_API void iris_window_set_max_size(int32_t width, int32_t height);

/* ================================================================== */
/*  Queries                                                            */
/* ================================================================== */

/* Snapshot of the active window's logical geometry. Out params are
 * optional (pass NULL to skip). Returns false when no app is active. */
IRIS_API bool iris_window_get_geometry(int32_t *out_width, int32_t *out_height);

#ifdef __cplusplus
}
#endif

#endif /* IRIS_WINDOW_H */
