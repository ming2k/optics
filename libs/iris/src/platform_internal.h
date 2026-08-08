/* platform_internal.h — portable platform interface for iris.
 *
 * Defines the contract between the public API and the platform backend
 * that owns the window, GPU device, and event loop (ADR-0044: exactly one
 * backend is compiled in, selected at meson configure time; the public
 * headers never change per platform).
 *
 * ============================================================================
 *  Backend contract — what app_wayland.c / app_win32.c / app_cocoa.m must
 *  each implement
 * ============================================================================
 *
 * Entry points dispatched from app.c (one per backend; only the compiled
 * backend's symbol is referenced):
 *
 *   int iris_app_run_<backend>(const iris_app_config *cfg);
 *     Owns the whole app lifecycle: create the native window + flux device
 *     + surface + canvas + lens context, honour cfg->{title,app_id,width,
 *     height,dark}, invoke cfg->start/build/paint/stop at the documented
 *     points (see iris/app.h), run the event loop until the window closes,
 *     tear down in dependency order. Returns 0 on success, non-zero on
 *     failure. Blocks the calling thread; all host callbacks run on that
 *     thread.
 *
 *   void iris_request_animation_frame_<backend>(void);
 *     Implements the public iris_request_animation_frame(): set a flag so
 *     the loop keeps the active (~60 Hz) cadence for one more frame. No-op
 *     when no app is running.
 *
 * Public window/cursor APIs (iris/window.h, iris/cursor.h) are context-free
 * — iris_window_minimize(), iris_set_cursor(), etc. take no handle. Each
 * backend therefore keeps a single static "active platform" pointer (see
 * g_active_pl in app_wayland.c), published at the top of
 * iris_app_run_<backend>() and cleared before teardown, and implements
 * every function declared in iris/window.h and iris/cursor.h against it.
 * They are documented as thread-affine to iris_app_run and must be safe
 * no-ops when no app is active. iris_window_focus() may legitimately be a
 * permanent no-op on platforms where clients cannot raise themselves
 * (Wayland); implement the hook anyway.
 *
 * Wakeup seam (platform_wakeup.h):
 *   The backend registers a kick via iris_platform_wakeup_set_kick()
 *   before entering its loop, unregisters after, and calls
 *   iris_platform_wakeup_drain() whenever the kick fired. This is how
 *   cross-thread subsystems (the colour-scheme watcher) reach the main
 *   thread. See platform_wakeup.h for the per-platform recipe.
 *
 * Input mapping (platform_input.h):
 *   Native pointer button codes are translated to iris_pointer_button and
 *   then to LENS_MOUSE_* indices via iris_pointer_button_to_lens(). Never
 *   include another platform's headers (linux/input-event-codes.h is
 *   Linux-only, windows.h Win32-only, …).
 *
 * lens contract (unchanged, platform-neutral): fold native input into one
 * lens_input per frame; wire lens_clipboard{set_text,request_text,user} to
 * the native clipboard; drive iris_a11y_update(ui) once per frame after
 * lens_end when the a11y bridge is running (Linux: also integrate the
 * bridge's fd via src/a11y_internal.h; Win32/Cocoa ship their own bridge).
 */
#ifndef IRIS_PLATFORM_INTERNAL_H
#define IRIS_PLATFORM_INTERNAL_H

#include "iris/app.h"

/* Wayland — implemented by app_wayland.c (Linux). */
int iris_app_run_wayland(const iris_app_config *cfg);
void iris_request_animation_frame_wayland(void);

/* Win32 — implemented by app_win32.c (to be added). */
int iris_app_run_win32(const iris_app_config *cfg);
void iris_request_animation_frame_win32(void);

/* Cocoa — implemented by app_cocoa.m (to be added). */
int iris_app_run_cocoa(const iris_app_config *cfg);
void iris_request_animation_frame_cocoa(void);

#endif /* IRIS_PLATFORM_INTERNAL_H */
