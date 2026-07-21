# ADR-0036: Lens input / clipboard / IME — host-supplied, size-guarded ABI

- Status: Accepted
- Date: 2026-07-21
- Scope: lens (L2 toolkit). Defines the input snapshot, clipboard, and
  IME contract and its ABI guard.

## Context

Lens is input-driven but owns no window system, clipboard, or IME. The
host (the application's platform layer, e.g. iris) fills an input
snapshot per frame, supplies a clipboard interface, and delivers IME
state. These structs are part of the public API and cross the library
boundary every frame, so their layout is an ABI surface that must
survive new fields.

Forces:

1. **Forward/backward ABI.** Apps compiled against an older `lens_input`
   must keep working when lens adds fields; apps compiled against a
   newer header must not overwrite memory when lens is older.
2. **Host separation.** Lens never links a clipboard or IME library; it
   calls host-supplied callbacks and consumes host-supplied state
   ([ADR-0029](0029-lens-interaction-model.md), [ADR-0033](0033-lens-text-seam.md)).
3. **Asynchronous clipboard.** Wayland's `wl_data_device` answers paste
   requests later; lens must queue delivered text for the focused widget
   to drain next frame.
4. **Full IME state.** text-input-v3 carries preedit with a cursor and
   an active clause, committed text, and a `delete_surrounding_text`
   request; all of it must be representable.

## Decision

1. **`lens_input` is a sized struct.** It opens with `uint32_t size`.
   Zero means "trust the full struct" (legacy); non-zero lets `lens_begin`
   copy `min(caller, lib)` bytes and re-stamp the library size, so older
   or newer binaries degrade cleanly. Same pattern as
   [ADR-0032](0032-lens-theme-tokens.md). Implemented in
   `libs/lens/src/core/context.c`.
2. **Cursor, mouse, wheel, mods, keys, text.** The base input fields
   cover the common event sources. Portable key sentinels
   (`LENS_KEY_TAB`, `_RETURN`, …) and modifier masks
   (`LENS_MOD_SHIFT`, …) keep the host's keycode mapping explicit.
3. **IME fields.** `preedit_utf8` + `preedit_cursor` + `preedit_sel_lo` /
   `_sel_hi` (active clause), committed `text_utf8`, and
   `ime_delete_before` / `ime_delete_after` (the
   `delete_surrounding_text` request, as byte counts in the host's last
   reported surrounding text). The host fills all of them per frame; a
   text widget consumes them during its IME update.
4. **Continuous scroll deltas.** `scroll_pixels_x` / `_y` are kept
   separate from wheel steps so widgets do not multiply finger motion by
   their line-scroll factor. Appended after the size guard is in place
   so older callers still link.
5. **Clipboard interface on `lens_desc`.** `lens_clipboard {
   request_text, set_text, user }` is optional. `lens_copy` /
   `lens_request_paste` call the callbacks; `lens_paste` queues delivered
   text into a staging buffer (`LENSI_PASTE_MAX`, 1024) for the focused
   text widget to drain via `lensi_take_paste` next frame.
6. **Caret rect host-readable.** The focused text widget sets the caret
   rect; the host reads `lens_caret_rect` to position the IME candidate
   window.
7. **IME is a no-op without a host.** Headless tests never supply IME
   state; the fields stay zero and widgets behave as if no composition
   is in progress.

References: `libs/lens/include/lens/lens.h` (Input snapshot, Host
clipboard interface, Clipboard and IME sections), `libs/lens/src/core/context.c`
(`lens_begin` size-aware copy), `libs/lens/src/input/clipboard.c`.

## Alternatives Considered

- **Event queue instead of a per-frame snapshot.** Reject: widgets
  return a synchronous `lens_response`; a snapshot is the natural shape.
- **Version integer instead of a byte `size`.** Reject: `size` is
  finer-grained and matches the Vulkan `pNext` idiom; a version requires
  a lookup table of layouts.
- **Lens links a clipboard/IME library.** Reject: breaks host separation
  and platform neutrality (Wayland vs Win32 vs Cocoa).
- **Synchronous paste (block on `lens_request_paste`).** Reject:
  Wayland's clipboard is inherently async.

## Consequences

Positive:

- The input ABI grows without breaking older binaries; new IME features
  land as appended fields.
- Lens has zero clipboard/IME dependencies; iris
  ([ADR-0044](0044-iris-backend-selection.md)) wires the platform
  transport.
- Async paste matches the Wayland contract.

Negative:

- Callers must set `input->size = sizeof(lens_input)` (or zero) to opt
  into the guard; forgetting falls back to the legacy full-copy path,
  which is safe but loses the guard.
- IME state is frame-coalesced; a host that delivers multiple
  compositions per frame must merge them.

## References

- [ADR-0024](0024-lens-foundations.md) — foundations.
- [ADR-0029](0029-lens-interaction-model.md) — interaction resolved from
  the snapshot.
- [ADR-0032](0032-lens-theme-tokens.md) — the same `size`-guard pattern,
  applied to theme.
- [ADR-0043](0043-iris-foundations.md), [ADR-0044](0044-iris-backend-selection.md)
  — the host that fills the snapshot.
