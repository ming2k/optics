/*
 * iris/app.h — cross-platform application entry point.
 *
 * iris_app_run() opens a window, creates a flux device + canvas + lens
 * context, runs the platform event loop, and drives an optional host
 * lifecycle around two per-frame callbacks:
 *
 *   1. start(ui, device) — creates host resources from Iris-owned contexts.
 *   2. build(ui, in) — runs inside an open lens_begin/end pair. The host
 *      builds its chrome (immediate-mode lens widgets) and reads `in`, the
 *      same lens_input snapshot lens is consuming this frame.
 *   3. paint(canvas) — runs inside an open flux_canvas_begin/end pair,
 *      *before* lens_render(). Anything the host draws here lands *under*
 *      lens's chrome. Returning without drawing is fine.
 *   4. stop(ui, device) — releases host resources before Iris tears down
 *      Lens, Flux, and the device.
 *
 * Today only the Linux/Wayland backend is built; it lives inside libiris
 * (src/app_wayland.c) and is dispatched from iris_app_run in src/app.c.
 * The same C signature is the contract future backends (Win32, Cocoa)
 * will satisfy. A backend is compiled in when its platform dependency is
 * satisfied; defining IRIS_BUILD_NO_BACKEND at compile time produces a
 * linkable libiris with iris_app_run returning a non-zero code, for
 * platform-less CI / bindgen builds.
 *
 * Concurrency: iris_app_run blocks the calling thread until the window
 * is closed. All callbacks run on the same thread.
 */
#ifndef IRIS_APP_H
#define IRIS_APP_H

#include <flux/canvas.h>
#include <flux/core.h>
#include <lens/lens.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================== */
/*  Visibility                                                        */
/* ================================================================== */

#if defined(_WIN32) && !defined(IRIS_STATIC)
#ifdef IRIS_BUILDING
#define IRIS_API __declspec(dllexport)
#else
#define IRIS_API __declspec(dllimport)
#endif
#elif defined(__GNUC__) || defined(__clang__)
#define IRIS_API __attribute__((visibility("default")))
#else
#define IRIS_API
#endif

#define IRIS_VERSION_MAJOR 0
#define IRIS_VERSION_MINOR 0
#define IRIS_VERSION_PATCH 3

IRIS_API const char *iris_version_string(void);

/* ================================================================== */
/*  Application                                                       */
/* ================================================================== */

/* Per-frame build callback. `ui` is live inside an open lens_begin/end
 * pair; `in` is the same lens_input snapshot lens is consuming this frame
 * (read-only — hosts that want to filter what lens sees must do so before
 * iris_app_run, by owning the event source). `user` is the pointer passed
 * in iris_app_config.
 *
 * This is where the host builds its chrome (immediate-mode lens widgets).
 * Drawing *canvas content* (e.g. a document surface the chrome sits over)
 * belongs in iris_paint_fn, which runs inside the canvas envelope. */
typedef void (*iris_build_fn)(lens *ui, const lens_input *in, void *user);

/* Per-frame paint callback. `canvas` is live inside an open
 * flux_canvas_begin/end pair, with the frame already cleared to the theme
 * background. `device` is the flux_device iris owns for this app —
 * hosts that need a device (e.g. to create a flux_text context) must
 * borrow this one rather than opening their own, since two flux_devices
 * in one process is unsupported and crashes. `scale` is the current
 * device-pixel ratio (Wayland `wl_surface.preferred_buffer_scale`); hosts
 * painting canvas content directly should wrap their draw in
 * `flux_canvas_save / flux_canvas_scale(canvas, scale, scale) / ...
 * / flux_canvas_restore` and call `flux_text_set_scale(text, scale)` so
 * glyphs rasterise crisply at the device resolution. lens applies the
 * same scale to its own chrome internally; hosts do not need to also
 * scale the chrome portion.
 *
 * Anything the host draws here lands *under* lens's chrome, because
 * iris calls lens_render(ui, canvas) after this returns.
 *
 * `user` is the same pointer passed to iris_build_fn. The host typically
 * captures it in the same closure context. */
typedef void (*iris_paint_fn)(flux_canvas *canvas, flux_device *device, float scale, void *user);

/* Application-resource lifecycle.
 *
 * `start` runs after iris has created its flux device, canvas, and lens
 * context, but before the first frame. Returning false aborts the run.
 * `stop` runs after the frame loop and before iris destroys any of those
 * objects, allowing hosts to release device-backed resources in dependency
 * order. `stop` is called only when `start` was absent or returned true. */
typedef bool (*iris_start_fn)(lens *ui, flux_device *device, void *user);
typedef void (*iris_stop_fn)(lens *ui, flux_device *device, void *user);

typedef struct iris_app_config {
    const char *title;   /* window title (UTF-8, optional)       */
    const char *app_id;  /* Wayland desktop app ID (optional)    */
    int32_t width;       /* initial logical width (0 = default)  */
    int32_t height;      /* initial logical height (0 = default) */
    bool dark;           /* force dark; false = follow system    */
    bool log_raw;        /* debug raw input events to stderr     */
    iris_start_fn start; /* resource setup before first frame (optional) */
    iris_stop_fn stop;   /* resource teardown before iris GPU teardown (optional) */
    iris_build_fn build; /* per-frame build callback (may be NULL) */
    iris_paint_fn paint; /* per-frame canvas paint (may be NULL)   */
    void *user;          /* opaque pointer passed to both callbacks */
} iris_app_config;

/* Request one more frame at the backend's active animation cadence.
 *
 * Call this from a build or paint callback while time-based host content is
 * still moving. Repeating the request every frame sustains the animation;
 * stopping lets the backend return to its low-power idle cadence. The call is
 * thread-affine to iris_app_run and is a no-op outside an active app. */
IRIS_API void iris_request_animation_frame(void);

/* Run the application until the window is closed. Returns 0 on success,
 * non-zero on platform failure (no display, GPU init failure, etc.).
 *
 * When cfg->dark is false, iris queries the system colour scheme at
 * startup (see theme.h) and applies it to the lens theme. */
IRIS_API int iris_app_run(const iris_app_config *cfg);

#ifdef __cplusplus
}
#endif

#endif /* IRIS_APP_H */
