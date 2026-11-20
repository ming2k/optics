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

#include <optics/window/loop.h>

#include "event_internal.h"
#include "platform_internal.h"
#include "platform_wakeup.h"

OPTICS_WINDOW_API void optics_window_request_animation_frame(void) {
#if defined(OPTICS_WINDOW_BACKEND_WAYLAND)
    optics_window_request_animation_frame_wayland();
#elif defined(OPTICS_WINDOW_BACKEND_WIN32)
    optics_window_request_animation_frame_win32();
#elif defined(OPTICS_WINDOW_BACKEND_COCOA)
    optics_window_request_animation_frame_cocoa();
#endif
}

OPTICS_WINDOW_API void optics_window_record_mark_static(void) {
#if defined(OPTICS_WINDOW_BACKEND_WAYLAND)
    optics_window_record_mark_static_wayland();
#elif defined(OPTICS_WINDOW_BACKEND_WIN32)
    optics_window_record_mark_static_win32();
#elif defined(OPTICS_WINDOW_BACKEND_COCOA)
    optics_window_record_mark_static_cocoa();
#endif
}

optics_gfx_display_list *optics_window_record_host_canvas(const optics_window_opts *cfg,
                                                          float scale) {
    if (!cfg || !cfg->record)
        return nullptr;
    optics_gfx_encoder *enc = nullptr;
    if (optics_gfx_encoder_create(&(optics_gfx_encoder_desc){}, &enc) != OPTICS_GFX_OK)
        return nullptr;
    cfg->record(enc, scale, cfg->user);
    optics_gfx_display_list *list = nullptr;
    (void)optics_gfx_encoder_finish(enc, &list);
    optics_gfx_encoder_destroy(enc);
    return list;
}

OPTICS_WINDOW_API void optics_window_request_frame_skip_render(void) {
#if defined(OPTICS_WINDOW_BACKEND_WAYLAND)
    optics_window_request_frame_skip_render_wayland();
#elif defined(OPTICS_WINDOW_BACKEND_WIN32)
    optics_window_request_frame_skip_render_win32();
#elif defined(OPTICS_WINDOW_BACKEND_COCOA)
    optics_window_request_frame_skip_render_cocoa();
#endif
}

OPTICS_WINDOW_API int optics_window_post_to_main_thread(optics_window_main_thread_fn fn,
                                                        void *user) {
#if defined(OPTICS_WINDOW_BACKEND_WAYLAND) || defined(OPTICS_WINDOW_BACKEND_WIN32) ||              \
    defined(OPTICS_WINDOW_BACKEND_COCOA)
    /* The backend wakeup seam (platform_wakeup.h) IS the delivery
     * mechanism: each backend registers a thread-safe kick that makes its
     * event-loop wait return, and drains the shared FIFO on the loop
     * thread. See iris/app.h for the public contract. */
    return optics_window_platform_wakeup_post(fn, user);
#else
    (void)fn;
    (void)user;
    return -1; /* no backend: no loop to post to */
#endif
}

OPTICS_WINDOW_API int optics_window_run(const optics_window_opts *cfg) {
    /* Both callbacks are optional: a pure-lens app leaves paint NULL, a
     * pure-canvas demo leaves build NULL. Rejecting an empty config here
     * would force every minimal example to register a no-op closure. */
    if (!cfg) {
        return 1;
    }
#if defined(OPTICS_WINDOW_BACKEND_WAYLAND) || defined(OPTICS_WINDOW_BACKEND_WIN32) ||              \
    defined(OPTICS_WINDOW_BACKEND_COCOA)
    /* Install the pre-fold event seam (ADR-0107) for the whole run; the
     * active backend routes every reified event through it. */
    optics_window__event_seam_configure(cfg->on_event, cfg->user);
#if defined(OPTICS_WINDOW_BACKEND_WAYLAND)
    int rc = optics_window_run_wayland(cfg);
#elif defined(OPTICS_WINDOW_BACKEND_WIN32)
    int rc = optics_window_run_win32(cfg);
#else
    int rc = optics_window_run_cocoa(cfg);
#endif
    optics_window__event_seam_clear();
    return rc;
#else
    return 2; /* IRIS_BUILD_NO_BACKEND */
#endif
}

#if !defined(OPTICS_WINDOW_BACKEND_WAYLAND) && !defined(OPTICS_WINDOW_BACKEND_WIN32) &&            \
    !defined(OPTICS_WINDOW_BACKEND_COCOA)
#include <optics/window/dnd.h>

OPTICS_WINDOW_API int optics_window_dnd_start(const optics_window_dnd_source *source) {
    (void)source;
    return -1;
}

OPTICS_WINDOW_API bool optics_window_dnd_is_active(void) {
    return false;
}

OPTICS_WINDOW_API void optics_window_dnd_cancel(void) {}
#endif
