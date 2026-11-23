/* platform_internal.h — portable platform interface for window.
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
 *   int optics_window_app_run_<backend>(const optics_window_opts *cfg);
 *     Owns the whole app lifecycle: create the native window + gfx device
 *     + surface + canvas + ui context, honour cfg->{title,app_id,width,
 *     height,dark}, invoke cfg->start/build/paint/stop at the documented
 *     points (see window/app.h), run the event loop until the window closes,
 *     tear down in dependency order. Returns 0 on success, non-zero on
 *     failure. Blocks the calling thread; all host callbacks run on that
 *     thread.
 *
 *   void optics_window_request_animation_frame_<backend>(void);
 *     Implements the public optics_window_request_animation_frame(): set a flag so
 *     the loop keeps the active (~60 Hz) cadence for one more frame. No-op
 *     when no app is running.
 *
 * Public window/cursor APIs (window/window.h, window/cursor.h) are context-free
 * — optics_window_minimize(), optics_window_set_cursor(), etc. take no handle. Each
 * backend therefore keeps a single static "active platform" pointer (see
 * g_active_pl in app_wayland.c), published at the top of
 * optics_window_app_run_<backend>() and cleared before teardown, and implements
 * every function declared in window/window.h and window/cursor.h against it.
 * They are documented as thread-affine to optics_window_run and must be safe
 * no-ops when no app is active. optics_window_focus() may legitimately be a
 * permanent no-op on platforms where clients cannot raise themselves
 * (Wayland); implement the hook anyway.
 *
 * Wakeup seam (platform_wakeup.h):
 *   The backend registers a kick via optics_window_platform_wakeup_set_kick()
 *   before entering its loop, unregisters after, and calls
 *   optics_window_platform_wakeup_drain() whenever the kick fired. This is how
 *   cross-thread subsystems (the colour-scheme watcher, async clipboard
 *   reads) reach the main thread — and, through the public
 *   optics_window_post_to_main_thread() (window/app.h, implemented in app.c on top of
 *   the same seam), how hosts deliver work from arbitrary threads. See
 *   platform_wakeup.h for the per-platform recipe. Teardown order matters:
 *   unregister the kick BEFORE the final drain, so detached threads that
 *   post late fail cleanly instead of queueing jobs nothing would drain.
 *
 * Input mapping (platform_input.h):
 *   Native pointer button codes are translated to optics_window_pointer_button and
 *   then to OPTICS_WINDOW_BUTTON_* indices via optics_window_pointer_button_index(). Never
 *   include another platform's headers (linux/input-event-codes.h is
 *   Linux-only, windows.h Win32-only, …).
 *
 * Event seam (event_internal.h, ADR-0107):
 *   Every native event is reified into one optics_window_event and routed through
 *   optics_window__event_seam_emit() BEFORE it is applied to the backend
 *   accumulator; only survivors are folded into optics_window_input. A backend must
 *   not write optics_window_input directly, and must not apply an event the seam
 *   dropped. Host rewrites are read back out of the same optics_window_event. Emit
 *   order is arrival order; window stamps the monotonic sequence.
 *
 * Keyboard contract (all three backends MUST hold these; ui relies on
 * them):
 *   (a) Both edges are reported: every mappable key produces a
 *       optics_window_key_event with pressed=true on key-down and pressed=false on
 *       key-up. (Win32: WM_KEYDOWN/UP; Cocoa: keyDown:/keyUp:; Wayland:
 *       wl_keyboard.key state.)
 *   (b) Letters and digits are normalised to their UNSHIFTED ASCII code
 *       ('a', never 'A'; '1', never '!') — shift state travels in
 *       optics_window_input.mods only, so Ctrl/Shift chords compare equal across
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
 *   (c) optics_window_key_event.repeat marks synthesised auto-repeat presses:
 *       Wayland implements client-side repeat from the compositor's
 *       wl_keyboard.repeat_info (a timerfd in the event loop re-emits the
 *       held key, with its text, until key-up / focus loss / modifier
 *       change), Win32 reads lParam bit 30, Cocoa reads isARepeat. ui
 *       treats a repeat exactly like a press (no widget filters on the
 *       flag), so a backend without repeat support may leave it false.
 *   (d) Committed text still carries the SHIFTED characters (the xkb UTF-8
 *       string / WM_CHAR / insertText:) — only the key id is normalised.
 *   (e) Control characters never travel as committed text: the backends
 *       agree via the shared optics_window_cp_is_text predicate (platform_text.h),
 *       which rejects the C0 range AND U+007F DEL. DEL is the trap: the
 *       Delete keysym's xkb UTF-8 mapping and Win32's WM_CHAR for
 *       Ctrl+Backspace are both exactly 0x7f, so a plain ">= 0x20" test
 *       leaks an invisible "\x7f" into text_utf8 alongside the key event.
 *       The intent rides the optics_window_key_event (OPTICS_WINDOW_KEY_DELETE); editors
 *       would otherwise insert garbage on every Delete press.
 *
 * IME truncation contract: every string funneled into optics_window_input's
 * fixed-size buffers (text_utf8[256] / preedit_utf8[OPTICS_WINDOW_PREEDIT_MAX=256])
 * is clipped with the shared boundary-aware helpers in platform_text.h —
 * never a raw byte cap, which can split a multi-byte sequence and hand
 * ui invalid UTF-8. Backend staging buffers (the Wayland accumulator's
 * text/preedit, the IME commit slot) are sized to match and pinned by
 * static_assert against the optics_window_input members.
 *
 * ui contract (unchanged, platform-neutral): fold native input into one
 * optics_window_input per frame; wire optics_window_ui_services
 * clipboard{set_text,request_text,user} to the native clipboard; drive
 * optics_window_a11y_update(ui) once per frame after the UI frame when the a11y bridge is running
 * (Linux: also integrate the bridge's fd via src/a11y_internal.h; Win32/Cocoa ship their own
 * bridge).
 */
#ifndef OPTICS_WINDOW_PLATFORM_INTERNAL_H
#define OPTICS_WINDOW_PLATFORM_INTERNAL_H

#include "optics/window/loop.h"

#include "event_internal.h"

/* Record the host canvas callback into an immutable display list.
 * Implemented once in app.c so every backend uses the same recording path. */
optics_gfx_display_list *optics_window_record_host_canvas(const optics_window_opts *cfg,
                                                          float scale);

/* Wayland — implemented by app_wayland.c (Linux). */
int optics_window_run_wayland(const optics_window_opts *cfg);
void optics_window_request_animation_frame_wayland(void);
void optics_window_record_mark_static_wayland(void);
void optics_window_request_frame_skip_render_wayland(void);

/* Win32 — implemented by app_win32.c (ADR-0056). */
int optics_window_run_win32(const optics_window_opts *cfg);
void optics_window_request_animation_frame_win32(void);
void optics_window_record_mark_static_win32(void);
void optics_window_request_frame_skip_render_win32(void);

/* Cocoa — implemented by app_cocoa.m (ADR-0056). */
int optics_window_run_cocoa(const optics_window_opts *cfg);
void optics_window_request_animation_frame_cocoa(void);
void optics_window_record_mark_static_cocoa(void);
void optics_window_request_frame_skip_render_cocoa(void);

#endif /* OPTICS_WINDOW_PLATFORM_INTERNAL_H */
