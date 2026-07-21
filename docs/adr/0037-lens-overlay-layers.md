# ADR-0037: Lens overlay layers — transient overlays + persistent floating panels

- Status: Accepted
- Date: 2026-07-21
- Scope: lens (L2 toolkit). Defines the floating-layer primitives.

## Context

Many widgets need content that escapes the parent's clip and layout
flow: dropdowns, menus, tooltips, modals, docks, status bars,
notification stacks. These are "floating layers" — positional sub-roots
rendered above the base tree. Two flavours exist: *transient* popups
with open/close state and dismissal, and *persistent* chrome that is
always on screen.

Forces:

1. **Escape parent clip + layout flow.** A dropdown inside a scroll area
   must not be clipped to the viewport nor consume main-axis space.
2. **Eclipse base widgets.** A click on a popup or dock must not also
   activate the widget painted under it.
3. **Dismissal semantics differ.** Transient popups close on
   click-outside / Escape (after a same-frame-open grace); persistent
   panels never auto-dismiss.
4. **Placement differs.** Transient overlays drop below an anchor and
   flip above when out of room; persistent panels are placed exactly at
   a supplied rect (clamped to the display).
5. **Reusable machinery.** Both flavours share positioning, rendering,
   eclipse, and accessibility, so they should share an implementation.

## Decision

1. **Two sibling primitives in one file.**
   - **Overlays** (`lens_overlay_begin`): transient popups — dropdowns,
     menus, tooltips, modals. Open state is retained per id;
     `lens_overlay_begin` enters the body only when the id is currently
     open. Dismissal runs at `lens_end`: Escape closes the top open
     overlay; a press outside an open overlay's last-frame rect closes
     it, subject to a same-frame-open grace.
   - **Panels** (`lens_layer_begin`): persistent chrome — docks, status
     bars, notification stacks, per-window title bars. Always rendered,
     never dismissed. Placed exactly at the supplied rect (clamped to
     the display), no below-anchor drop or flip.
   Both implemented in `libs/lens/src/overlay/overlay.c`.
2. **Shared per-frame layer array.** Both register a sub-root in
   `overlay_layers[]`, which `lensi_overlay_layout` places (after the
   base tree) and `lensi_overlay_render` draws above the base tree.
3. **Open-set only for overlays.** Tracked in `open_overlays[]`
   (`LENSI_OVERLAY_MAX`, 8) for dismissal and `is_open` queries; panels
   never enter this table.
4. **Eclipse helper.** `lensi_point_in_floating_layer` and
   `lensi_widget_eclipsed` (in `input.c`) return true when a floating
   layer covers the cursor for a base widget. Every interactive
   hit-test consults this so popups/panels eclipse base widgets.
5. **Cross-frame layer ids carried across the arena reset.**
   `prev_overlay_layer_ids[]` records layers as of the end of the
   previous frame so this frame's eclipse checks cover base widgets
   built *before* the layer re-registers (the common case: popups are
   declared after the content they cover).
6. **Placement helpers.** `lensi_overlay_constrain_current` lets an
   owner (e.g. a dropdown) inherit an owner-rect boundary; the layout
   pass (`lensi_layout_subtree`, [ADR-0028](0028-lens-flexbox-layout.md))
   arranges the sub-root into the placed rect.
7. **Built-on overlays.** Modal dialogs ([ADR-0039](0039-lens-modal-dialog.md))
   and menus ([ADR-0040](0040-lens-menus.md)) are built on top of the
   overlay primitives; dropdowns are an overlay with an item list.

References: `libs/lens/include/lens/lens.h` (Overlay layer, Floating
layers sections), `libs/lens/src/overlay/overlay.c`,
`libs/lens/src/input/input.c` (eclipse helpers),
`libs/lens/src/layout/solve.c` (`lensi_layout_subtree`).

## Alternatives Considered

- **One overlay primitive, dismiss flag toggles persistence.** Reject:
   the placement rules differ enough (drop-and-flip vs exact-place) that
   a single API would need a flag cluster; two named primitives read
   more clearly at the call site.
- **Portals / separate scenes per popup.** Reject: heavier than needed;
   a positional sub-root that escapes layout flow but reuses layout and
   render is the right granularity.
- **No eclipse — let the host sort z-order.** Reject: the host cannot
   know which widget is under a popup; lens owns the tree and must own
   the eclipse.
- **Tombstone dead layers across frames.** Reject: carrying plain ids
   across the arena reset (with no per-layer struct) is enough for
   eclipse; the layer re-registers its geometry each frame.

## Consequences

Positive:

- Dropdowns, menus, tooltips, modals, docks, and status bars all share
  one positioning + render + eclipse path.
- Persistent panels are first-class (not a hack on overlays), so docks
  and title bars have a documented home.
- Eclipse uses last-frame geometry, so a popup covers the content it
  will paint over even when declared late in build order.

Negative:

- The shared layer array is bounded (`LENSI_OVERLAY_MAX`, 8); a noisy
  overlay set drops the oldest, which surfaces as behaviour (a bug) not
  a crash.
- Eclipse correctness depends on every custom hit-test remembering to
  call `lensi_widget_eclipsed`; forgetting lets clicks fall through
  popups.

## References

- [ADR-0028](0028-lens-flexbox-layout.md) — sub-tree layout entry point.
- [ADR-0029](0029-lens-interaction-model.md) — eclipse + scroll-clip in
  hit-testing.
- [ADR-0035](0035-lens-accessibility-tree.md) — overlay sub-roots
  visited by the a11y walk.
- [ADR-0039](0039-lens-modal-dialog.md), [ADR-0040](0040-lens-menus.md)
  — built on overlays.
