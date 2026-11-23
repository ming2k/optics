/*
 * app/app.h — the application shell: ui running in a native window.
 *
 * window owns the window, the event pump and the frame cadence and depends on
 * gfx only. ui is a headless UI engine and depends on no windowing system.
 * app is the one place the two meet: it implements window's UI-host seam
 * (<optics/window/host.h>) over a ui context, once, for every platform backend.
 *
 *     optics_app_run(&(optics_app_opts){
 *         .title = "Hello",
 *         .build = build,          // immediate-mode ui widgets, every frame
 *     });
 *
 * What app does on the application's behalf:
 *   - creates the ui context on window's device and releases it in order;
 *   - folds optics_window_input into optics_ui_input and runs optics_ui_begin / build /
 * optics_ui_end;
 *   - publishes ui snapshots to window's presentation pipeline;
 *   - maps the OS colour scheme and accessibility preferences onto the theme;
 *   - wires the ui clipboard and drag source to the window's;
 *   - exposes the ui semantic tree to the platform accessibility bridge.
 *
 * Window, cursor, file-dialog and capability APIs are window's and are used
 * directly (<optics/window.h>).
 */
#ifndef OPTICS_APP_H
#define OPTICS_APP_H

#include <stdbool.h>
#include <stdint.h>

#include <optics/gfx/core.h>
#include <optics/ui.h>
#include <optics/window.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(_WIN32) && !defined(OPTICS_APP_STATIC)
#ifdef OPTICS_APP_BUILDING
#define OPTICS_APP_API __declspec(dllexport)
#else
#define OPTICS_APP_API __declspec(dllimport)
#endif
#elif defined(__GNUC__) || defined(__clang__)
#define OPTICS_APP_API __attribute__((visibility("default")))
#else
#define OPTICS_APP_API
#endif

/* Per-frame build callback. `ui` is live inside an open optics_ui_begin/optics_ui_end
 * pair; `in` is the input ui is consuming this frame (read-only — filter
 * events in `on_event`, before they are folded). */
typedef void (*optics_app_build_fn)(optics_ui *ui, const optics_ui_input *in, void *user);

/* Application-resource lifecycle. `start` runs once the device and the ui
 * context exist and before the first frame; returning false aborts the run.
 * `stop` runs after the frame loop while both are still alive, and only when
 * `start` was absent or returned true. */
typedef bool (*optics_app_start_fn)(optics_ui *ui, optics_gfx_device *device, void *user);
typedef void (*optics_app_stop_fn)(optics_ui *ui, optics_gfx_device *device, void *user);

typedef struct optics_app_opts {
    const char *title;                /* window title (UTF-8, optional)        */
    const char *app_id;               /* desktop application id (optional)     */
    int32_t width;                    /* initial logical width (0 = default)   */
    int32_t height;                   /* initial logical height (0 = default)  */
    bool dark;                        /* force dark; false = follow the system */
    bool log_raw;                     /* log folded input to stderr            */
    bool no_clear;                    /* keep content rendered in `prepare`    */
    optics_window_event_fn on_event;  /* pre-fold event interception (optional) */
    optics_app_start_fn start;        /* resource setup (optional)             */
    optics_app_stop_fn stop;          /* resource teardown (optional)          */
    optics_app_build_fn build;        /* per-frame UI build (optional)         */
    optics_window_record_fn record;   /* host canvas content under the UI (optional) */
    optics_window_prepare_fn prepare; /* pre-canvas GPU pass (optional)        */
    void *user;                       /* passed to every callback              */
} optics_app_opts;

/* Run the application until its window closes. Returns 0 on success, non-zero
 * on platform failure (no display, GPU initialisation failure, ...). Blocks
 * the calling thread; every callback runs on it. */
[[nodiscard]] OPTICS_APP_API int optics_app_run(const optics_app_opts *opts);

/* ================================================================== */
/*  Active Application Runtime Controls (ADR-0172)                    */
/* ================================================================== */

/* Callback posted to the application main thread via `optics_app_post`. */
typedef void (*optics_app_main_fn)(void *user);

/* Request the active application window to close cleanly. */
OPTICS_APP_API void optics_app_quit(void);

/* Request one more frame at the display's active animation cadence. */
OPTICS_APP_API void optics_app_request_frame(void);

/* Thread-safe: post `fn(user)` to run on the application main thread.
 * Returns 0 if queued, -1 if no application loop is active. */
OPTICS_APP_API int optics_app_post(optics_app_main_fn fn, void *user);

/* Query the active application window's logical size. Returns false outside
 * an active run. */
OPTICS_APP_API bool optics_app_get_geometry(int32_t *out_width, int32_t *out_height);

#ifdef __cplusplus
}
#endif

#endif /* OPTICS_APP_H */
