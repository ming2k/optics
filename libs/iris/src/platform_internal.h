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
 *   cross-thread subsystems (the colour-scheme watcher, async clipboard
 *   reads) reach the main thread — and, through the public
 *   iris_post_to_main_thread() (iris/app.h, implemented in app.c on top of
 *   the same seam), how hosts deliver work from arbitrary threads. See
 *   platform_wakeup.h for the per-platform recipe. Teardown order matters:
 *   unregister the kick BEFORE the final drain, so detached threads that
 *   post late fail cleanly instead of queueing jobs nothing would drain.
 *
 * Input mapping (platform_input.h):
 *   Native pointer button codes are translated to iris_pointer_button and
 *   then to LENS_MOUSE_* indices via iris_pointer_button_to_lens(). Never
 *   include another platform's headers (linux/input-event-codes.h is
 *   Linux-only, windows.h Win32-only, …).
 *
 * Keyboard contract (all three backends MUST hold these; lens relies on
 * them):
 *   (a) Both edges are reported: every mappable key produces a
 *       lens_key_event with pressed=true on key-down and pressed=false on
 *       key-up. (Win32: WM_KEYDOWN/UP; Cocoa: keyDown:/keyUp:; Wayland:
 *       wl_keyboard.key state.)
 *   (b) Letters and digits are normalised to their UNSHIFTED ASCII code
 *       ('a', never 'A'; '1', never '!') — shift state travels in
 *       lens_input.mods only, so Ctrl/Shift chords compare equal across
 *       platforms. Wayland derives the id from the level-0 keysym of the
 *       key's active layout (xkb_keymap_key_get_syms_by_level), Win32 from
 *       the virtual-key code, Cocoa from charactersIgnoringModifiers.
 *       Deriving press and release from the same unshifted source also
 *       guarantees the two edges always carry the same key id.
 *       Scope: ALL printable ASCII (0x20–0x7E) produces a key event on
 *       every backend — letters, digits, punctuation and space alike —
 *       so host shortcuts can match characters directly (e.g. 'z', ',',
 *       ' '). The split is therefore: printable-ASCII key events carry
 *       the ASCII codepoint as the key id; NON-ASCII printable input
 *       (accented letters, CJK, other scripts) never produces a key event
 *       on any backend — it arrives as committed text only (IME or
 *       layout), which is the channel editors must consume for content.
 *       Shortcut matching should never rely on non-ASCII key events.
 *   (c) lens_key_event.repeat marks synthesised auto-repeat presses:
 *       Wayland implements client-side repeat from the compositor's
 *       wl_keyboard.repeat_info (a timerfd in the event loop re-emits the
 *       held key, with its text, until key-up / focus loss / modifier
 *       change), Win32 reads lParam bit 30, Cocoa reads isARepeat. lens
 *       treats a repeat exactly like a press (no widget filters on the
 *       flag), so a backend without repeat support may leave it false.
 *   (d) Committed text still carries the SHIFTED characters (the xkb UTF-8
 *       string / WM_CHAR / insertText:) — only the key id is normalised.
 *   (e) Control characters never travel as committed text: the backends
 *       agree via the shared iris_cp_is_text predicate (platform_text.h),
 *       which rejects the C0 range AND U+007F DEL. DEL is the trap: the
 *       Delete keysym's xkb UTF-8 mapping and Win32's WM_CHAR for
 *       Ctrl+Backspace are both exactly 0x7f, so a plain ">= 0x20" test
 *       leaks an invisible "\x7f" into text_utf8 alongside the key event.
 *       The intent rides the lens_key_event (LENS_KEY_DELETE); editors
 *       would otherwise insert garbage on every Delete press.
 *
 * IME truncation contract: every string funneled into lens_input's
 * fixed-size buffers (text_utf8[256] / preedit_utf8[LENS_PREEDIT_MAX=256])
 * is clipped with the shared boundary-aware helpers in platform_text.h —
 * never a raw byte cap, which can split a multi-byte sequence and hand
 * lens invalid UTF-8. Backend staging buffers (the Wayland accumulator's
 * text/preedit, the IME commit slot) are sized to match and pinned by
 * static_assert against the lens_input members.
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
void iris_paint_mark_static_wayland(void);

/* Win32 — implemented by app_win32.c (ADR-0056). */
int iris_app_run_win32(const iris_app_config *cfg);
void iris_request_animation_frame_win32(void);
void iris_paint_mark_static_win32(void);

/* Cocoa — implemented by app_cocoa.m (ADR-0056). */
int iris_app_run_cocoa(const iris_app_config *cfg);
void iris_request_animation_frame_cocoa(void);
void iris_paint_mark_static_cocoa(void);

#endif /* IRIS_PLATFORM_INTERNAL_H */
