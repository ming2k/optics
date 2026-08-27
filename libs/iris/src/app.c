/* app.c — iris_app_run dispatcher.
 *
 * Exactly one platform backend is compiled into libiris, selected at meson
 * configure time (ADR-0044): IRIS_BACKEND_WAYLAND, IRIS_BACKEND_WIN32, or
 * IRIS_BACKEND_COCOA. iris_app_run forwards to that backend; the public
 * headers never change per platform.
 *
 * If IRIS_BUILD_NO_BACKEND is defined at compile time, iris_app_run
 * returns a non-zero code immediately. This lets platform-less CI builds
 * link against libiris for header / bindgen purposes.
 */

#include <iris/app.h>

#include "platform_internal.h"
#include "platform_wakeup.h"

IRIS_API void iris_request_animation_frame(void) {
#if defined(IRIS_BACKEND_WAYLAND)
    iris_request_animation_frame_wayland();
#elif defined(IRIS_BACKEND_WIN32)
    iris_request_animation_frame_win32();
#elif defined(IRIS_BACKEND_COCOA)
    iris_request_animation_frame_cocoa();
#endif
}

IRIS_API void iris_paint_mark_static(void) {
#if defined(IRIS_BACKEND_WAYLAND)
    iris_paint_mark_static_wayland();
#elif defined(IRIS_BACKEND_WIN32)
    iris_paint_mark_static_win32();
#elif defined(IRIS_BACKEND_COCOA)
    iris_paint_mark_static_cocoa();
#endif
}

IRIS_API void iris_request_frame_skip_render(void) {
#if defined(IRIS_BACKEND_WAYLAND)
    iris_request_frame_skip_render_wayland();
#elif defined(IRIS_BACKEND_WIN32)
    iris_request_frame_skip_render_win32();
#elif defined(IRIS_BACKEND_COCOA)
    iris_request_frame_skip_render_cocoa();
#endif
}

IRIS_API int iris_post_to_main_thread(iris_main_thread_fn fn, void *user) {
#if defined(IRIS_BACKEND_WAYLAND) || defined(IRIS_BACKEND_WIN32) || defined(IRIS_BACKEND_COCOA)
    /* The backend wakeup seam (platform_wakeup.h) IS the delivery
     * mechanism: each backend registers a thread-safe kick that makes its
     * event-loop wait return, and drains the shared FIFO on the loop
     * thread. See iris/app.h for the public contract. */
    return iris_platform_wakeup_post(fn, user);
#else
    (void)fn;
    (void)user;
    return -1; /* no backend: no loop to post to */
#endif
}

IRIS_API int iris_app_run(const iris_app_config *cfg) {
    /* Both callbacks are optional: a pure-lens app leaves paint NULL, a
     * pure-canvas demo leaves build NULL. Rejecting an empty config here
     * would force every minimal example to register a no-op closure. */
    if (!cfg) {
        return 1;
    }
#if defined(IRIS_BACKEND_WAYLAND)
    return iris_app_run_wayland(cfg);
#elif defined(IRIS_BACKEND_WIN32)
    return iris_app_run_win32(cfg);
#elif defined(IRIS_BACKEND_COCOA)
    return iris_app_run_cocoa(cfg);
#else
    return 2; /* IRIS_BUILD_NO_BACKEND */
#endif
}
