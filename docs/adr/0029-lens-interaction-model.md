# ADR-0029: Lens interaction model — prev-frame geometry, one-frame hit-test latency

- Status: Accepted
- Date: 2026-07-21
- Scope: lens (L2 toolkit). Defines how input is resolved against the
  widget tree.

## Context

Lens builds the widget tree each frame from immediate-mode calls, but
layout (which produces `final_rect`) runs at `lens_end`, *after* the
build calls that want to query hover/press. Input therefore cannot be
hit-tested against this frame's geometry — the geometry does not exist
yet during build.

Forces:

1. **Input must be resolved during build.** Widgets return a
   `lens_response` (hovered/pressed/clicked/focused) synchronously from
   their build call, so the caller can branch on it.
2. **Geometry is only final after arrange.** The two-pass layout
   ([ADR-0028](0028-lens-flexbox-layout.md)) finishes at `lens_end`.
3. **Stability across frames.** Using last frame's solved geometry is
   safe *because* lens is retained: the same id has the same node, and
   its `prev_rect` is what was actually painted.
4. **Eclipse semantics.** A floating layer
   ([ADR-0037](0037-lens-overlay-layers.md)) above a base widget must
   swallow hover/press for widgets under it; scroll clipping must apply
   identically to hit-testing and rendering.

## Decision

1. **Hit-test against `prev_rect`.** `lensi_interact` resolves hover,
   press, click, and focus for a widget using its `prev_rect` (last
   frame's `final_rect`). A brand-new widget (no `prev_rect`) reports no
   interaction this frame — the documented one-frame latency. Implemented
   in `libs/lens/src/input/input.c`.
2. **`hot_id` / `active_id` / `focused_id`.** The hottest (topmost)
   widget under the cursor sets `hot_id`; a press inside a widget
   captures `active_id` until release; `focused_id` is the keyboard focus.
   Later widgets (drawn on top) win `hot_id`.
3. **Scroll-clip hit-testing.** `lensi_point_clipped_by_scroll` mirrors
   the render clip (`prev_rect` inset by pad, minus the scrollbar gutter)
   so a child scrolled out of view is not hoverable/clickable through
   whatever is painted over it. Every interactive hit-test must consult
   this in addition to the widget's own rect.
4. **Floating-layer eclipse.** `lensi_widget_eclipsed` returns true when
   a floating layer covers the cursor for a widget that does not belong
   to that layer. Widgets with their own hit-testing (table rows,
   scrollbars, wheel routing) must also check this so popups eclipse them.
5. **`lens_response.rect` is the prev rect.** The response carries the
   geometry that was actually hit-tested, so the caller can position
   overlays against it.
6. **Tab focus order collected during build.** Focusable widgets append
   their id to `tab_order`; `lensi_focus_tab` in `lens_end` resolves
   Tab/Shift+Tab against that slice. A modal
   ([ADR-0039](0039-lens-modal-dialog.md)) clamps the cycle to its body.

References: `libs/lens/include/lens/lens.h` (Input snapshot, Interaction
result, Interaction queries sections), `libs/lens/src/input/input.c`,
`libs/lens/src/input/focus.c`, `libs/lens/src/internal.h` (interaction
state on `struct lens`).

## Alternatives Considered

- **Hit-test against this frame's geometry.** Reject: geometry is not
  final until `lens_end`, but input must be resolved during build.
  Requires either deferring the whole build (breaks the immediate-mode
  contract) or two layout passes.
- **A separate event queue processed at `lens_end`.** Reject: widgets
  could not return a synchronous `lens_response`; the caller would have
  to defer every branch.
- **Cursor-based z-order (topmost widget under cursor wins, no id
  tracking).** Reject: `active_id` capture (drag) needs an explicit
  owner that survives even when the cursor leaves the widget.

## Consequences

Positive:

- Widgets return a synchronous, accurate-enough response; one-frame
  latency is the documented, acceptable cost of the retained/immediate
  split.
- Eclipse and scroll-clip rules compose: popups over scroll areas behave
  correctly because every hit-test consults the same helpers.
- Drag (`active_id`) and focus (`focused_id`) survive cursor travel and
  Tab cycling.

Negative:

- A brand-new widget is not interactive on its first frame. Documented,
  but occasionally surprises a caller who expects instant feedback.
- Any widget doing custom hit-testing must remember to consult
  `lensi_widget_eclipsed` and `lensi_point_clipped_by_scroll`; forgetting
  produces clicks that fall through popups or scroll viewports.

## References

- [ADR-0024](0024-lens-foundations.md) — retained/immediate split.
- [ADR-0028](0028-lens-flexbox-layout.md) — geometry produced by layout.
- [ADR-0037](0037-lens-overlay-layers.md) — eclipse semantics.
- [ADR-0039](0039-lens-modal-dialog.md) — focus trap.
- [ADR-0036](0036-lens-input-clipboard-ime.md) — input ABI guard.
