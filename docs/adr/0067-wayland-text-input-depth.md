# ADR-0067: Wayland text-input depth — key repeat, compose, per-widget IME sessions

- Status: Accepted
- Date: 2026-08-12

## Context

The Wayland backend's text-input-v3 wiring was structurally sound but
shallow. An audit (against HEAD 49b8460) found user-visible defects: no
key repeat at all (`repeat_info` discarded, no client-side timer); a
preedit abandoned by Alt-Tab kept rendering in the still-focused text
widget; the IME was enabled window-wide, so with ibus/fcitx active,
typing with no text field focused started invisible compositions and
swallowed keys; no xkb compose meant dead keys were broken for users
without an IM; `set_surrounding_text` was never sent, leaving the
correctly implemented `delete_surrounding_text` chain unreachable;
IME commits were truncated by `lens_input.text_utf8[32]`, sized for
key-at-a-time input, not full-sentence conversion; seat/output globals
leaked on removal; the DND drop read blocked the UI thread; and
mouse-initiated clipboard operations carried a stale serial.

Win32 and Cocoa get repeat, compose, and IM arbitration from the OS.
Wayland hands the client protocols and nothing else — this depth is the
backend's job, but it had not been built yet.

## Decision

1. **Client-side key repeat** (`libs/iris/src/app_wayland.c`): a
   `timerfd` (CLOCK_MONOTONIC) joins the event loop's poll set, armed
   from the compositor's `repeat_info` rate/delay. Each expiry re-emits
   the held key as `lens_key_event{pressed=true, repeat=true}` plus its
   UTF-8 text. Repeat follows the newest pressed key and is cancelled by
   its release, `kb_leave`, another press, a modifier change, or rate 0.
2. **xkb compose**: a compose table/state (locale from `LC_CTYPE`,
   `XKB_DEFAULT_LOCALE` honored) is fed on every non-repeat press;
   composed output replaces the raw UTF-8 fallback. Repeats bypass
   compose — a sequence completes on the original press.
3. **The IME session is per-widget, not per-window**: enabled exactly
   while the window has keyboard focus AND a lens text widget is focused,
   re-evaluated once per frame after `lens_end` (`im_frame_update`) —
   the wl_keyboard enter arrives before any frame renders, and lens-side
   focus changes produce no Wayland events, so the frame evaluation is
   the only authoritative signal. Transitions alone issue protocol
   requests; session end disables + commits and drops every piece of
   pending composition state.
4. **Surrounding text and content hints**: lens publishes the focused
   text widget's text/cursor/multiline through the new
   `lens_text_context_get` (`libs/lens/include/lens/lens.h`); the widget
   pushes it every frame alongside the caret rect, and the buffer is
   borrowed until the next `lens_begin`. iris reports it via
   `set_surrounding_text` only when content or cursor changed
   (`iris_text_memento_update` in `platform_text.c`) and sets the
   MULTILINE content hint for textareas. This makes the
   `delete_surrounding_text` chain (ADR-0036) reachable by real IMs.
5. **IME buffers sized for conversion, not keystrokes**:
   `lens_input.text_utf8` 32→256, `LENS_PREEDIT_MAX` 64→256. The
   ADR-0036 size guard keeps callers built against older headers
   working; truncation stays boundary-aware and tested.
6. **Preedit active clause**: iris stops discarding the text-input-v3
   selection range, and lens renders it — `lens_widget_content` gains an
   appended `preedit_clause` field the default skins draw as an
   emphasised underline (`libs/lens/src/widgets/textfield.c`,
   `textarea.c`, `skin/`). Caller-owned skins are unaffected unless they
   opt in.
7. **Primary selection and async drop**: `zwp_primary_selection_v1`
   mirrors copies onto the middle-click selection and pastes it on
   middle-press through the shared async reader; the DND drop read moved
   onto the same detached-thread + deadline + wakeup pattern as
   clipboard paste; pointer-button serials are tracked so
   mouse-initiated clipboard operations are legitimate.
8. **Lifecycle hygiene**: `reg_remove` destroys and recycles removed
   output/seat globals; keyboard-capability loss ends the IM session
   first; seat/pointer/keyboard/data-device objects use `_release` where
   the bound version provides it, guarded for v1 managers.

## Alternatives Considered

- **Window-wide IM enable (previous behavior).** Rejected: with a real
  IM running, keys are swallowed and candidate windows appear at stale
  positions whenever no text field is focused. GTK/Qt disable the IM on
  text blur; now iris does too.
- **Repeat driven by the frame loop instead of a timerfd.** Rejected:
  ties repeat cadence to frame pacing, burns frames for input events,
  and stalls when the host idles the loop. The timerfd wakes the loop
  only when a repeat is actually due.
- **Relying on the IM for compose.** Rejected: users without ibus/fcitx
  get nothing; client-side xkb compose is the desktop norm (GTK/Qt do
  the same).
- **Pointer/length fields in `lens_input` instead of larger arrays.**
  Rejected: churns the ADR-0036 size-guarded POD model for no real
  gain; fixed buffers with documented, boundary-aware truncation stay
  simple and testable.
- **Password `CONTENT_PURPOSE` in this round.** Deferred: it needs a
  cross-stack lens design (masking, a11y, all three backends), not a
  Wayland-local change.

## Consequences

- `lens_input` layout changes (the ADR-0036 size guard covers older
  callers; the monorepo rebuilds together). `lens_widget_content` grows
  one appended field.
- `lens_key_event.repeat` is now genuinely produced on Wayland;
  `platform_internal.h` documents each backend's reality (Win32 lParam
  bit 30, Cocoa `isARepeat`).
- Verified by compile, the full meson suite (including new lens/iris
  unit tests), and the ASan build. Compositor-facing paths (repeat
  timing, compose, text-input-v3, primary selection, DND) have no
  headless harness here — that coverage gap is accepted and stated.
- Follow-ons not built: password purpose, drag source, fractional-scale
  rendering into lens, touch widget API, virtual keyboard.

## References

- [ADR-0036 — lens input, clipboard, and IME seam](0036-lens-input-clipboard-ime.md)
- [ADR-0057 — Paste drain and caret rect for app-owned editing surfaces](0057-paste-drain-and-caret-rect-for-app-surfaces.md)
- wayland-protocols: text-input-unstable-v3, primary-selection-unstable-v1
