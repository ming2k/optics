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
#ifndef OPTICS_WINDOW_STATE_H
#define OPTICS_WINDOW_STATE_H

#include <stdbool.h>
#include <stdint.h>

#include <optics/window/loop.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================== */
/*  Window state                                                       */
/* ================================================================== */

/* Minimize (iconify) the active window. */
OPTICS_WINDOW_API void optics_window_minimize(void);

/* Ask the compositor to maximise the active window. */
OPTICS_WINDOW_API void optics_window_maximize(void);

/* Ask the compositor to un-maximise the active window. */
OPTICS_WINDOW_API void optics_window_unmaximize(void);

/* Enter fullscreen on the output the active window is currently on. */
OPTICS_WINDOW_API void optics_window_fullscreen(void);

/* Leave fullscreen. Restores the prior floating geometry. */
OPTICS_WINDOW_API void optics_window_unfullscreen(void);

/* Restore the window from minimised / maximised state (when allowed).
 * Equivalent to calling unmaximize + unfullscreen; provided as a
 * convenience for window menu items. */
OPTICS_WINDOW_API void optics_window_restore(void);

/* Request keyboard focus (raise / activate). Subject to compositor
 * policy; most compositors restrict focus stealing without a user
 * activation token. */
OPTICS_WINDOW_API void optics_window_focus(void);

/* Request the compositor to close the active window. iris_app_run
 * returns as if the user had clicked the close button. */
OPTICS_WINDOW_API void optics_window_close(void);

/* Request the compositor / window manager to start an interactive move
 * of the active window (e.g. initiated by a pointer drag on a custom
 * title bar or tab bar in client-side decoration mode).
 *
 * On Wayland, this issues an xdg_toplevel.move request using the seat and
 * serial of the current pointer interaction. On backends without
 * interactive move support, this call is a safe no-op. */
OPTICS_WINDOW_API void optics_window_start_move(void);

/* ================================================================== */
/*  Size hints                                                         */
/* ================================================================== */

/* Configure the minimum and maximum logical window size the compositor
 * should respect. Pass 0 to clear the respective bound. Compositors
 * combine these with their own constraints (display size, layout, …). */
typedef struct optics_window_set_min_size_opts {
    int32_t width;
    int32_t height;
} optics_window_set_min_size_opts;

typedef struct optics_window_set_max_size_opts {
    int32_t width;
    int32_t height;
} optics_window_set_max_size_opts;

OPTICS_WINDOW_API void optics_window_set_min_size(const optics_window_set_min_size_opts *opts);
OPTICS_WINDOW_API void optics_window_set_max_size(const optics_window_set_max_size_opts *opts);

/* ================================================================== */
/*  Queries                                                            */
/* ================================================================== */

/* Snapshot of the active window's logical geometry. Out params are
 * optional (pass NULL to skip). Returns false when no app is active. */
OPTICS_WINDOW_API bool optics_window_get_geometry(int32_t *out_width, int32_t *out_height);

/* Request a window activation token (xdg-activation-v1 on Wayland) to pass
 * to an external or child process for seamless focus handoff.
 * Returns 0 on success with NUL-terminated token written into out_buf,
 * or -1 if unsupported, inactive, or creation fails. */
typedef struct optics_window_create_activation_token_opts {
    const char *app_id;
    char *out_buf;
    size_t out_cap;
} optics_window_create_activation_token_opts;

OPTICS_WINDOW_API int
optics_window_create_activation_token(const optics_window_create_activation_token_opts *opts);

#ifdef __cplusplus
}
#endif

#endif /* IRIS_WINDOW_H */
