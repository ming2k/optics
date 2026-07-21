# ADR-0039: Lens modal dialog — centered overlay + backdrop + Tab focus trap

- Status: Accepted
- Date: 2026-07-21
- Scope: lens (L2 toolkit). Defines the modal-dialog primitive.

## Context

A modal dialog is a common UI primitive: centered content above a dim
backdrop that eclipses the base tree, with keyboard cycling trapped
inside the dialog body. It is built on the overlay layer
([ADR-0037](0037-lens-overlay-layers.md)), so the question is what a
modal adds on top of a plain overlay.

Forces:

1. **Centered placement, not anchored.** Unlike a dropdown (anchored
   below a trigger), a modal centers on the display.
2. **Backdrop eclipses the base tree.** Clicks and hover on the base
   must be swallowed; the base should be dimmed.
3. **Focus trap.** Tab/Shift+Tab cycling must stay inside the dialog
   body for accessibility and correctness.
4. **Optional dismissal.** Some modals close on Escape / click-outside;
   others (a forced confirmation) are pinned until a button closes them.
5. **Open state per id.** A modal's open/close lifecycle should reuse
   the overlay open-set rather than invent a parallel mechanism.

## Decision

1. **Two cooperating floating layers per open modal.**
   - A full-display **backdrop panel** (persistent layer, non-dismissible,
     dims + eclipses the base tree via the floating-layer eclipse check).
   - A **centered content overlay** (transient, carries the dialog body).
   Implemented in `libs/lens/src/widgets/modal.c`.
2. **Open state via the overlay open-set.** `lens_modal_open` /
   `_is_open` / `_close` are thin wrappers over `lens_overlay_*`. The
   content id keys the open state; the backdrop id is a derived sibling
   (`"<id>##bd"`) so it never collides and has no open state of its own.
3. **`is_centered` node flag.** The content overlay sets
   `n->is_centered` so the overlay placement pass centers it on the
   display instead of dropping below an anchor.
4. **`dismissable` flag.** Set at `lens_modal_begin` time from
   `lens_modal_opts.dismissable` (default true). When false, the overlay
   is pinned: Escape and click-outside leave it alone; only
   `lens_modal_close` (a button) closes it. The flag is stored on the
   open-overlay slot and consulted by the dismissal pass.
5. **Tab focus trap.** While `modal_active` is set, `lensi_focus_tab`
   clamps Tab cycling to `[modal_tab_lo, modal_tab_hi)` — the slice of
   `tab_order` recorded while the modal body was built. `lens_end`
   resets the trap so a frame with no open modal falls back to
   whole-range cycling.
6. **`lens_modal_opts`.** Title (optional heading), `backdrop` colour
   (default `0x80000000`), `min_width` (default 240), `dismissable`.

References: `libs/lens/include/lens/lens.h` (Modal dialog section),
`libs/lens/src/widgets/modal.c`, `libs/lens/src/overlay/overlay.c`,
`libs/lens/src/input/focus.c`.

## Alternatives Considered

- **One layer, backdrop drawn by the host.** Reject: the host cannot
   reliably dim and eclipse the base tree; lens owns the tree and the
   eclipse.
- **Separate modal layer type (not built on overlays).** Reject: would
   duplicate placement, rendering, and open-state machinery.
- **Focus trap by disabling base widgets.** Reject: would mutate base
   widget state and require restore; the tab-range clamp is non-destructive.
- **Always dismissable; no pinned variant.** Reject: forced
   confirmations (unsaved-changes, license prompt) need a pinned modal.

## Consequences

Positive:

- A modal reuses the overlay placement, render, and eclipse paths —
  no new floating-layer machinery.
- The Tab trap makes modal keyboard navigation accessible by default.
- The `dismissable` flag covers both the common and the pinned cases.

Negative:

- Two layers per modal counts against `LENSI_OVERLAY_MAX` (8); a deep
  modal stack plus other popups could drop the oldest. Documented and
  surfaced as behaviour.
- The focus trap relies on the modal body being built between
  `lens_modal_begin` and `_end`; a caller who builds focusable widgets
  outside that range escapes the trap.

## References

- [ADR-0037](0037-lens-overlay-layers.md) — the layer primitives a modal
  is built from.
- [ADR-0029](0029-lens-interaction-model.md) — Tab focus order and
  eclipse.
- [ADR-0035](0035-lens-accessibility-tree.md) — `LENS_ROLE_DIALOG`.
