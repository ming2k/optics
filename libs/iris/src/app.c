/* app.c — iris_app_run dispatcher.
 *
 * Today there is exactly one backend (Linux/Wayland); iris_app_run is a
 * thin forwarder. The indirection exists so future backends (Win32, Cocoa)
 * can be selected at link time without changing the public API.
 *
 * If IRIS_BUILD_NO_BACKEND is defined at compile time, iris_app_run
 * returns a non-zero code immediately. This lets platform-less CI builds
 * link against libiris for header / bindgen purposes.
 */

#include <iris/app.h>

#ifndef IRIS_BUILD_NO_BACKEND
int iris_app_run_wayland(const iris_app_config *cfg);
void iris_request_animation_frame_wayland(void);
#endif

IRIS_API void iris_request_animation_frame(void) {
#ifndef IRIS_BUILD_NO_BACKEND
    iris_request_animation_frame_wayland();
#endif
}

IRIS_API int iris_app_run(const iris_app_config *cfg) {
    /* Both callbacks are optional: a pure-lens app leaves paint NULL, a
     * pure-canvas demo leaves build NULL. Rejecting an empty config here
     * would force every minimal example to register a no-op closure. */
    if (!cfg) {
        return 1;
    }
#ifdef IRIS_BUILD_NO_BACKEND
    return 2;
#else
    return iris_app_run_wayland(cfg);
#endif
}
