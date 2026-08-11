# ADR-0064: Host-controlled caret and selection for lens_textfield

- Status: Accepted
- Date: 2026-08-12
- Scope: lens (L2 toolkit). Adds two public functions for moving a text
  field's caret/selection from host code. Complements
  [ADR-0036](0036-lens-input-clipboard-ime.md) (IME seam) and
  [ADR-0057](0057-paste-drain-and-caret-rect-for-app-surfaces.md)
  (app-owned editing surfaces).

## Context

A lens text field owns its caret/selection in retained widget state
(`lens_textfield_state` in `libs/lens/src/widgets/textfield.c`), and the
only accommodation for hosts that rewrite the edit buffer
programmatically is clamp-on-shrink: offsets beyond the new length are
clamped at the next build. Anything else strands the caret — append a
Tab completion to "git ch" → "git checkout" and the caret stays at byte
6, so the next typed character lands mid-string; pre-fill a value and
the caret sits at 0 instead of the end.

Hosts that need this today have two sanctioned routes and both are the
wrong size for the job: the ADR-0036 IME seam is for composing text, not
for placing the caret, and the ADR-0057 app-owned-surface pattern means
re-implementing editing, selection, and IME for a field that lens
already renders perfectly well. What these hosts actually need is the
same reach-into-retained-state channel that `lens_scroll_to` and
`lens_collapsing_set_open` already provide for scroll areas and
collapsing sections.

## Decision

Two public functions, implemented in
`libs/lens/src/widgets/textfield.c` and declared in
`libs/lens/include/lens/lens.h` (Rust wrappers in
`bindings/lens-rs/crates/lens/src/lib.rs`):

1. `void lens_textfield_set_caret(lens *ui, const char *label, uint32_t caret)`
   — collapse the selection and place the caret.
2. `void lens_textfield_set_selection(lens *ui, const char *label, uint32_t anchor, uint32_t caret)`
   — set anchor and caret independently; select-all is anchor 0, caret
   `UINT32_MAX`.

Semantics:

- Offsets are **byte** offsets into the edit buffer, not character
  indices. This is the unit the widget state already stores, so no
  translation layer is introduced.
- The label is resolved in the current id scope with
  `lensi_gen_widget_id` + `lensi_store_touch` (find-or-create), so the
  call works before the field's first-ever frame; a label whose field
  never appears is reaped with the store's leaving-node grace frames
  (ADR-0038).
- The write is **unconditional** (unlike `lens_collapsing_set_open`'s
  first-frame seed): the host's rewrite wins for that frame, then the
  field's own editing takes over again.
- Range and character-boundary repair happen at the next build, beside
  the existing clamp: out-of-range offsets clamp to the buffer length,
  and offsets that land mid-character snap back to a UTF-8 boundary
  (back off while `(buf[p] & 0xc0) == 0x80`).
- While an IME preedit is active the field manages its own caret and
  selection (it clears the selection each focused preedit frame), so
  host writes during a preedit have no visible effect. Documented, not
  special-cased.

## Alternatives Considered

- **Keep pointing hosts at app-owned surfaces (ADR-0057).** Rejected:
  it forces hosts to re-implement editing, selection, and IME for
  trivial buffer rewrites like Tab completion or a pre-filled value —
  a whole document editor to move one caret.
- **A pending command slot consumed at build.** Rejected: no precedent
  benefit. `lens_scroll_to` and `lens_set_focus` write their target
  state directly, and the widget's build already repairs stale state;
  a queue would only add a second path to reason about.
- **Caret in `lens_textfield_opts`.** Rejected: opts are per-frame
  transient, while the caret is cross-frame retained state — the wrong
  home. An opts field would also re-apply every frame, fighting the
  user's own caret movements.

## Consequences

- The byte-offset contract (clamping, boundary snap, select-all idiom,
  preedit caveat) is documented at both declaration sites: the C header
  and the Rust `Frame` docs; `TextBuf::set` points at the caret API.
- ABI: two new exported symbols (`lens_textfield_set_caret`,
  `lens_textfield_set_selection`); no struct layout changes.
- The build-time clamp now also snaps mid-character offsets to a UTF-8
  boundary. Widget-written offsets are always boundaries, so the snap
  only affects host-written (or corrupted) state.

## References

- [ADR-0036](0036-lens-input-clipboard-ime.md) — input / clipboard /
  IME seam.
- [ADR-0057](0057-paste-drain-and-caret-rect-for-app-surfaces.md) —
  app-owned editing surfaces.
- [ADR-0038](0038-lens-node-state-gc.md) — 8-frame grace window for
  leaving nodes (bounds the lifetime of a set-caret label whose field
  never appears).
