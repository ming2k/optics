# ADR-0040: Lens menus — menubar, context menu, submenu, items (hover-dwell)

- Status: Accepted
- Date: 2026-07-21
- Scope: lens (L2 toolkit). Defines the menu primitives.

## Context

Menus are a standard desktop primitive: a menu bar is a horizontal row
of triggers with click-then-drag switching; a context menu opens at the
cursor on right-click; a submenu nests to the side of its parent item;
items carry an optional shortcut, check/radio mark, and disabled state.
All are floating content that escapes layout flow, so they are built on
the overlay layer ([ADR-0037](0037-lens-overlay-layers.md)).

Forces:

1. **Overlay-backed.** A menu is an overlay laid out as a vertical list
   of items; reuse the placement, eclipse, and dismissal machinery.
2. **State must persist.** Bar-switch tracking, submenu hover-dwell
   timers, and the cursor position captured at context-menu open must
   survive across frames — but menus have no label of their own to key
   per-node state.
3. **Hover-dwell for submenus.** A submenu should open after a short
   hover to avoid flicker when the pointer traverses a sibling.
4. **Click-then-drag bar switching.** Once a menu bar trigger is open,
   dragging into a sibling switches the open menu without a second
   click.
5. **Distinct ids.** A submenu trigger and its parent item must stay
   distinct nodes even when they share a visible label.

## Decision

1. **Menus are overlays.** `lens_menubar_begin`/`_end`, `lens_menu_begin`/
   `_end`, `lens_submenu_begin`/`_end`, `lens_context_menu_begin`/`_end`
   build on `lens_overlay_*`. A menu is an overlay laid out as a vertical
   list of items; a menubar is a horizontal row of triggers. Implemented
   in `libs/lens/src/widgets/menu.c`.
2. **Retained state via `lens_node_state`.** Per-item hover dwell and
   per-context-menu cursor position live in `lens_menu_item_state` /
   `lens_ctxmenu_state` borrowed from the retained store
   ([ADR-0027](0027-lens-retained-store.md)).
3. **Hover-dwell timers.** A submenu opens after accumulated hover dwell
   crosses a threshold, preventing flicker on sibling traversal. The
   dwell accumulator is persisted on the item node.
4. **Click-then-drag bar switching.** The menubar tracks whether a
   trigger is already open; while one is, dragging into a sibling opens
   that sibling and closes the previous one without requiring a click.
5. **`lensi_overlay_open_id_pub`.** `menu.c` derives the trigger id
   itself and opens the overlay by pre-computed id (via
   `lensi_overlay_open_id_pub`) so it does not perturb the sibling
   sequence counter ([ADR-0037](0037-lens-overlay-layers.md)).
6. **Distinct trigger vs. item ids.** A submenu trigger and its parent
   item use distinct ids so both nodes stay in the retained store; the
   comment "##ov" in `dropdown.c` documents the same pattern.
7. **Items.** `lens_menu_item[_disabled|_flags]` with optional shortcut
   (right-aligned, dimmed), `LENS_MENU_CHECKED` / `_RADIO` marks, and
   `LENS_MENU_DISABLED`. `lens_menu_separator` draws a rule. Clicking an
   item fires its return and closes the menu stack
   (`lens_menubar_close_all_open`).
8. **Context menu.** `lens_context_menu_open` (right-click or
   programmatic) captures the cursor position into retained state;
   `lens_context_menu_begin` anchors the overlay at that position.

References: `libs/lens/include/lens/lens.h` (Menus section),
`libs/lens/src/widgets/menu.c`, `libs/lens/src/overlay/overlay.c`,
`libs/lens/src/widgets/dropdown.c` (shared overlay pattern).

## Alternatives Considered

- **A separate menu scene graph.** Reject: would duplicate overlay
   placement, eclipse, and dismissal; menus are floating content and the
   overlay layer is the right substrate.
- **Open submenus instantly on hover.** Reject: flickers when the
   pointer crosses siblings; the dwell timer is the standard fix.
- **Per-menu allocation for state.** Reject: `lens_node_state` already
   gives stable per-id persistent storage; reusing it keeps memory
   bounded by the store.
- **Single trigger+item node.** Reject: the trigger and the item need
   independent bounds and state; collapsing them loses the layout
   distinction.

## Consequences

Positive:

- Menus inherit overlay dismissal (click-outside, Escape) and eclipse
  for free.
- Hover-dwell and click-then-drag give the desktop menu feel callers
  expect.
- Retained state on the store keeps menu memory proportional to visible
  items, not history.

Negative:

- Menu correctness depends on the overlay open-set and sibling-sequence
  id derivation; a contributor who rebuilds the id scheme must keep
  `lensi_overlay_open_id_pub` in mind.
- The dwell threshold is a magic number; a future theme token or
  per-instance tunable would be cleaner.

## References

- [ADR-0037](0037-lens-overlay-layers.md) — the layer menus are built
  on.
- [ADR-0027](0027-lens-retained-store.md) — `lens_node_state` for
  per-item/per-menu retained state.
- [ADR-0029](0029-lens-interaction-model.md) — hover/press resolution.
- [ADR-0035](0035-lens-accessibility-tree.md) — `LENS_ROLE_MENU`.
