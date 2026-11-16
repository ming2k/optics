/*
 * aperture/aperture.h — the application shell: lens running in an iris window.
 *
 * iris owns the window, the event pump and the frame cadence and depends on
 * flux only. lens is a headless UI engine and depends on no windowing system.
 * aperture is the one place the two meet: it implements iris's UI-host seam
 * (<iris/host.h>) over a lens context, once, for every platform backend.
 *
 *     aperture_app_run(&(aperture_app_opts){
 *         .title = "Hello",
 *         .build = build,          // immediate-mode lens widgets, every frame
 *     });
 *
 * What aperture does on the application's behalf:
 *   - creates the lens context on iris's device and releases it in order;
 *   - folds iris_input into lens_input and runs lens_begin / build / lens_end;
 *   - publishes lens snapshots to iris's presentation pipeline;
 *   - maps the OS colour scheme and accessibility preferences onto the theme;
 *   - wires the lens clipboard and drag source to the window's;
 *   - exposes the lens semantic tree to the platform accessibility bridge.
 *
 * Window, cursor, file-dialog and capability APIs are iris's and are used
 * directly (<iris/iris.h>).
 */
#ifndef APERTURE_H
#define APERTURE_H

#include <stdbool.h>
#include <stdint.h>

#include <flux/core.h>
#include <iris/iris.h>
#include <lens/lens.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(_WIN32) && !defined(APERTURE_STATIC)
#ifdef APERTURE_BUILDING
#define APERTURE_API __declspec(dllexport)
#else
#define APERTURE_API __declspec(dllimport)
#endif
#elif defined(__GNUC__) || defined(__clang__)
#define APERTURE_API __attribute__((visibility("default")))
#else
#define APERTURE_API
#endif

/* Per-frame build callback. `ui` is live inside an open lens_begin/lens_end
 * pair; `in` is the input lens is consuming this frame (read-only — filter
 * events in `on_event`, before they are folded). */
typedef void (*aperture_build_fn)(lens *ui, const lens_input *in, void *user);

/* Application-resource lifecycle. `start` runs once the device and the lens
 * context exist and before the first frame; returning false aborts the run.
 * `stop` runs after the frame loop while both are still alive, and only when
 * `start` was absent or returned true. */
typedef bool (*aperture_start_fn)(lens *ui, flux_device *device, void *user);
typedef void (*aperture_stop_fn)(lens *ui, flux_device *device, void *user);

typedef struct aperture_app_opts {
    const char *title;       /* window title (UTF-8, optional)        */
    const char *app_id;      /* desktop application id (optional)     */
    int32_t width;           /* initial logical width (0 = default)   */
    int32_t height;          /* initial logical height (0 = default)  */
    bool dark;               /* force dark; false = follow the system */
    bool log_raw;            /* log folded input to stderr            */
    bool no_clear;           /* keep content rendered in `prepare`    */
    iris_event_fn on_event;  /* pre-fold event interception (optional) */
    aperture_start_fn start; /* resource setup (optional)             */
    aperture_stop_fn stop;   /* resource teardown (optional)          */
    aperture_build_fn build; /* per-frame UI build (optional)         */
    iris_record_fn record;   /* host canvas content under the UI (optional) */
    iris_prepare_fn prepare; /* pre-canvas GPU pass (optional)        */
    void *user;              /* passed to every callback              */
} aperture_app_opts;

/* Run the application until its window closes. Returns 0 on success, non-zero
 * on platform failure (no display, GPU initialisation failure, ...). Blocks
 * the calling thread; every callback runs on it. */
[[nodiscard]] APERTURE_API int aperture_app_run(const aperture_app_opts *opts);

#ifdef __cplusplus
}
#endif

#endif /* APERTURE_H */
