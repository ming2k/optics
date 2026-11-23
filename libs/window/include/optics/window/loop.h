/*
 * window/app.h — cross-platform application entry point.
 *
 * optics_window_run() opens a window, creates a gfx device, surface and canvas,
 * runs the platform event loop and drives the frame cadence. window depends on
 * gfx only: what is drawn comes from two independent, optional sources.
 *
 *   - Host canvas content: `record(enc, scale)` runs on the UI thread and
 *     records into an immutable display list; `prepare(frame, ...)` runs a
 *     pre-canvas GPU pass. Both are plain gfx.
 *   - A UI layer: `ui` is an `optics_window_ui_host` (window/host.h) that builds a frame
 *     from the folded input and publishes immutable snapshots. Its visuals
 *     composite over the host canvas content. window never includes a UI
 *     header; `app` binds ui to this seam.
 *
 * Lifecycle: start(device) → frames → stop(device), around the UI host's own
 * start/stop/destroy.
 *
 * One platform backend is compiled in per build (ADR-0044): Wayland
 * (src/app_wayland.c), Win32 (src/app_win32.c), or Cocoa (src/app_cocoa.m) —
 * dispatched from optics_window_run in src/app.c. Defining OPTICS_WINDOW_BUILD_NO_BACKEND
 * produces a linkable liboptics_window whose optics_window_run returns non-zero, for
 * platform-less CI / bindgen builds.
 *
 * Concurrency: optics_window_run blocks the calling thread until the window is
 * closed. All callbacks run on that thread unless window/host.h says otherwise.
 */
#ifndef OPTICS_WINDOW_LOOP_H
#define OPTICS_WINDOW_LOOP_H

#include <optics/gfx/canvas.h>
#include <optics/gfx/core.h>
#include <optics/window/event.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================== */
/*  Visibility                                                        */
/* ================================================================== */

#if defined(_WIN32) && !defined(OPTICS_WINDOW_STATIC)
#ifdef OPTICS_WINDOW_BUILDING
#define OPTICS_WINDOW_API __declspec(dllexport)
#else
#define OPTICS_WINDOW_API __declspec(dllimport)
#endif
#elif defined(__GNUC__) || defined(__clang__)
#define OPTICS_WINDOW_API __attribute__((visibility("default")))
#else
#define OPTICS_WINDOW_API
#endif

#define OPTICS_WINDOW_VERSION_MAJOR 0
#define OPTICS_WINDOW_VERSION_MINOR 0
#define OPTICS_WINDOW_VERSION_PATCH 83

/* Packed integer version, monotonic — identical layout to
 * OPTICS_GFX_VERSION_NUMBER (major in bits 16..23, minor 8..15, patch 0..7).
 * The whole stack shares one versioning scheme so a consumer can check
 * every library it loads the same way. */
#define OPTICS_WINDOW_VERSION_NUMBER                                                               \
    (((uint32_t)OPTICS_WINDOW_VERSION_MAJOR << 16) |                                               \
     ((uint32_t)OPTICS_WINDOW_VERSION_MINOR << 8) | (uint32_t)OPTICS_WINDOW_VERSION_PATCH)

OPTICS_WINDOW_API void optics_window_version(int *major, int *minor, int *patch);
OPTICS_WINDOW_API uint32_t optics_window_version_number(void);
/* True when the linked library can stand in for the one compiled
 * against: same major, and its (minor, patch) is >= the requested one.
 * ABI is major-locked; API additions ride minors. */
OPTICS_WINDOW_API bool optics_window_version_check(int major, int minor, int patch);
OPTICS_WINDOW_API const char *optics_window_version_string(void);

/* ================================================================== */
/*  Application                                                       */
/* ================================================================== */

/* The UI layer plugged into the frame loop; defined in <optics/window/host.h>. */
typedef struct optics_window_ui_host optics_window_ui_host;

/* Per-frame host canvas recording callback (ADR-0127).
 *
 * Runs on the UI / main thread outside any open Vulkan frame. The host records
 * vector geometry, glyph runs, and brushes into `enc` using the immutable
 * display list recording model (ADR-0088). `scale` is the current device-pixel
 * ratio (Wayland `wl_surface.preferred_buffer_scale`); record in logical
 * coordinates and wrap the drawing in `optics_gfx_encoder_scale(enc, scale, scale)`
 * when the content must be crisp at device resolution. Glyph runs are recorded
 * with `glyph_record` at the same scale.
 *
 * The recorded commands are compiled into an immutable optics_gfx_display_list and
 * submitted under the UI layer's visuals. This is the only host canvas path:
 * there is no live-canvas callback, so recording never touches a swapchain
 * and every backend uses the same decoupled presentation pipeline
 * (ADR-0124/ADR-0127). */
typedef void (*optics_window_record_fn)(optics_gfx_encoder *enc, float scale, void *user);

/* Per-frame prepare callback (optional).
 *
 * Runs after window acquires the frame and before window begins the primary
 * canvas pass on the swapchain image. `frame` is live in
 * OPTICS_GFX_FRAME_STATE_RECORDING with no active render pass.
 *
 * This hook enables multi-pass workloads: offscreen render targets,
 * compute dispatch (e.g. blur filters, material optical materials), or
 * scene graph passes that produce images consumed downstream by
 * `optics_window_record_fn`. */
typedef void (*optics_window_prepare_fn)(optics_gfx_frame *frame, optics_gfx_canvas *canvas,
                                         optics_gfx_device *device, float scale, void *user);

/* Application-resource lifecycle.
 *
 * `start` runs after window has created its gfx device and canvas and after
 * the UI host started, but before the first frame. Returning false aborts
 * the run. `stop` runs after the frame loop and before window destroys any of
 * those objects, allowing hosts to release device-backed resources in
 * dependency order. `stop` is called only when `start` was absent or
 * returned true. */
typedef bool (*optics_window_start_fn)(optics_gfx_device *device, void *user);
typedef void (*optics_window_stop_fn)(optics_gfx_device *device, void *user);

/* Pre-fold event interception (optional, ADR-0107).
 *
 * window reifies each native event into an `optics_window_event` (window/event.h) in
 * arrival order. When `on_event` is set, window invokes it once per event,
 * synchronously on the optics_window_run thread, *before* folding survivors
 * into the frame's optics_window_input. The host may drop an event (return
 * OPTICS_WINDOW_EVENT_DROP) or rewrite its fields and deliver it (mutate `*ev`,
 * return OPTICS_WINDOW_EVENT_PASS). This is the only point at which a host may change
 * what the UI layer sees.
 *
 * `on_event == nullptr` (the default) is behaviourally identical to a
 * build without this seam and incurs no per-event cost. Events flagged
 * OPTICS_WINDOW_EVENT_F_UNDROPPABLE (CLOSE / RESIZE / SCALE / FOCUS) are still
 * delivered to the callback but cannot be dropped. */
typedef struct optics_window_opts {
    const char *title;                /* window title (UTF-8, optional)       */
    const char *app_id;               /* Wayland desktop app ID (optional)    */
    int32_t width;                    /* initial logical width (0 = default)  */
    int32_t height;                   /* initial logical height (0 = default) */
    bool dark;                        /* force dark; false = follow system    */
    const optics_window_ui_host *ui;  /* UI layer (window/host.h); nullptr = canvas only */
    bool log_raw;                     /* debug raw input events to stderr     */
    bool no_clear;                    /* skip canvas clear; preserves content rendered in prepare */
    optics_window_event_fn on_event;  /* pre-fold event interception (optional, ADR-0107) */
    optics_window_start_fn start;     /* resource setup before first frame (optional) */
    optics_window_stop_fn stop;       /* resource teardown before window GPU teardown (optional) */
    optics_window_record_fn record;   /* host canvas recorder, the only canvas path (optional) */
    optics_window_prepare_fn prepare; /* pre-canvas frame compute/render pass (optional) */
    void *user;                       /* opaque pointer passed to callbacks */
} optics_window_opts;

/* Request one more frame at the backend's active animation cadence.
 *
 * Call this from a UI-frame or record callback while time-based host content is
 * still moving. Repeating the request every frame sustains the animation;
 * stopping lets the backend return to its low-power idle cadence. The call is
 * thread-affine to optics_window_run and is a no-op outside an active app. */
OPTICS_WINDOW_API void optics_window_request_animation_frame(void);

/* Declare the current frame's recorded host canvas content static.
 *
 * Recorded host content is opaque to ui, so backends normally keep
 * presenting every scheduled frame — including a full clear to the
 * background. When the host knows its scene is pixel-identical to what is
 * already on screen (a paused animation, a settled audio visualizer), this
 * call lets the backend skip the entire record → clear → submit → present
 * cycle for this frame: no swapchain image is committed and the GPU does no
 * host work, which unloads the compositor exactly like a host without a
 * record callback.
 *
 * The declaration covers one frame only. Any user input, a resize or buffer
 * scale change, a following optics_window_request_animation_frame, or UI damage
 * forces the next frame to present again; hosts should keep
 * calling this every frame their scene is still static. Recording content
 * after declaring it static is contradictory — the recording is skipped along
 * with the pass. No-op without an active app or without a record callback. */
OPTICS_WINDOW_API void optics_window_record_mark_static(void);
/* Zero-render skip for hosts with a record callback: after a build in which
 * the host knows the canvas content is unchanged *and* it still wants the
 * active-rate cadence to continue, call this instead of (not in addition
 * to) optics_window_record_mark_static. The frame's begin_frame → clear → record →
 * snapshot publication → present sequence is skipped entirely — no swapchain image
 * is acquired and the previous buffer stays on screen — but the *scheduling*
 * branch sees an animation request as usual, so the loop keeps running at
 * the active rate. This is exactly the "present at the media cadence while
 * build continues at display cadence" shape: one render per stage tick,
 * zero-render static in-between frames, no idle-drop to ~4 Hz.
 *
 * Semantics per frame, mutually exclusive:
 *   record           → present this frame (default when neither is called)
 *   mark_static      → skip this frame AND decay toward idle pacing
 *   request+skip     → skip this frame AND keep active pacing
 *
 * Precedence note for the request+skip pattern: a fresh
 * optics_window_request_animation_frame does NOT veto a same-frame skip — the skip
 * wins for rendering, while the request re-arms the active deadline. Any
 * input, resize/scale change, or UI damage forces a real
 * present regardless of both declarations; those events invalidate what is
 * on screen and neither per-frame declaration can have accounted for them.
 * The flag is consumed once and must be re-issued every frame. No-op
 * without an active app or without a record callback. */
OPTICS_WINDOW_API void optics_window_request_frame_skip_render(void);

/* ================================================================== */
/*  Cross-thread delivery                                             */
/* ================================================================== */

/* Callback queued by optics_window_post_to_main_thread, run on the window main
 * thread (the thread executing optics_window_run). */
typedef void (*optics_window_main_thread_fn)(void *user);

/* Post fn(user) to the window main thread.
 *
 * Thread-safe: callable from ANY thread (worker threads, OS callbacks,
 * watcher pumps). The function runs on the main thread the next time the
 * event loop wakes, outside the UI layer's frame — so posted callbacks may
 * legally call main-thread-affine APIs. Posts made
 * while no optics_window_run loop is active are dropped and optics_window_run's
 * teardown drains or rejects everything still queued, so fn never runs
 * against torn-down host state.
 *
 * Ordering: callbacks posted from a single thread run in FIFO order within
 * the frame that drains them (posts from different threads are serialized
 * by arrival). Delivery is coalesced with the loop's normal wakeups: a post
 * does not by itself schedule a paint, it only runs the callback.
 *
 * Returns 0 when queued, -1 when no loop is currently registered (fn is
 * never run in that case — the caller retains ownership of user). */
OPTICS_WINDOW_API int optics_window_post_to_main_thread(optics_window_main_thread_fn fn,
                                                        void *user);

/* Run the application until the window is closed. Returns 0 on success,
 * non-zero on platform failure (no display, GPU init failure, etc.). */
[[nodiscard]] OPTICS_WINDOW_API int optics_window_run(const optics_window_opts *opts);

#ifdef __cplusplus
}
#endif

#endif /* OPTICS_WINDOW_LOOP_H */
