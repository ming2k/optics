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

IRIS_API void iris_request_animation_frame(void) {
#if defined(IRIS_BACKEND_WAYLAND)
    iris_request_animation_frame_wayland();
#elif defined(IRIS_BACKEND_WIN32)
    iris_request_animation_frame_win32();
#elif defined(IRIS_BACKEND_COCOA)
    iris_request_animation_frame_cocoa();
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
