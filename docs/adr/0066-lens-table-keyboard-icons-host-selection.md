# ADR-0066: Keyboard cursor, icons, and host-owned selection for lens_table

- Status: Accepted
- Date: 2026-08-12
- Scope: lens (L2 toolkit). Extends the virtualized table
  ([ADR-0042](0042-lens-virtualized-table.md)) with an opt-in keyboard
  cursor, per-cell icons, and host-owned selection/cursor, via an
  additive `lens_table_opts` / `lens_table_result` extension.

## Context

ADR-0042's table is mouse-only, text-only, and single-select with the
selection retained in per-node widget state. That covers simple
pick-from-a-list screens, but a file-chooser-class listing needs three
things it cannot do:

1. **Keyboard operation.** The table never called `lensi_interact`, so
   it was absent from the tab order and had no keyboard path at all —
   arrow-key navigation, Home/End, and Return activation are table
   stakes for a dense list.
2. **Per-cell icons.** File/entry lists lead with a glyph. The grid
   payload (`lens_grid_row`) carried only strings.
3. **Host-owned selection and cursor.** A real picker tracks a
   selection SET (Ctrl/Shift semantics, type-ahead, model resets) that
   the host already owns and mutates; a retained single int in widget
   state cannot express it, and the widget cannot know when the host's
   model resets underneath the retained row index.

## Decision

Extend `lens_table_opts` / `lens_table_result` in place (ABI layout
change, acceptable per ADR-0058 — all consumers rebuild), keeping the
default behavior of `lens_table` bit-for-bit unchanged:

- `opts.selectable` tables now join the tab order (the widget calls
  `lensi_interact` after its scrollbar block so a thumb press still
  claims `active_id` first). The table keeps its own row hit-test for
  the mouse; `lensi_interact`'s `clicked` is honored only for the a11y
  DoAction path, which reports through the new `result.activated`.
- `opts.keyboard` (requires `selectable`): while the table is focused,
  Up/Down move a cursor row by one, Home/End jump to the first/last
  row, and Return activates the cursor row
  (`result.activated`). From `-1` (no cursor) Down lands on the first
  row, Up on the last; the ends clamp. Handled keys are marked
  consumed so the ADR-0062 central activation cannot double-fire and no
  later widget observes the table's navigation. Space is deliberately
  NOT handled: it stays available to the host (search-as-you-type into
  names with spaces, Ctrl+Space selection toggles). The cursor row is
  scrolled into view with a minimal scroll, on keyboard moves and on
  host-driven cursor jumps alike. Arrow keys move the
  table's own cursor row, NEVER `focused_id` (the ADR-0058 rule: arrow
  navigation is intra-widget).
- The cursor is a row INDEX (`result.cursor`, -1 = none). It is
  retained in `lens_table_state` by default, or host-owned via
  `opts.cursor` (dropdown-style in/out): the table reads it at build
  start and writes it back whenever the effective cursor moves,
  including the clamp when the model shrank under it — hosts re-seed
  on model resets by owning the variable. `result.cursor_changed`
  reports movement in both modes.
- `opts.icon_fn` supplies a per-cell `lens_icon_id` (built-in or
  runtime-registered SVG id). Ids ride in an arena array parallel to
  the cells on `lens_grid_row.icons`; for a valid id the widget shifts
  the precomputed text x right by `font_size + 8` (the
  `lens_selectable_icon` precedent), keeping the "skin never measures"
  invariant. Only `LENS_START`-aligned columns carry icons.
- `opts.selected_fn` makes the selection host-owned: the row highlight
  and the a11y SELECTED flag come from the pull callback, clicks only
  report the new `result.clicked_row`, and the retained `selected`
  stays -1. The retained store is fixed-size per node — a dynamic
  selection set does not belong there.
- The cursor row carries `LENS_STATE_FOCUSED` in the row record (and
  `LENS_A11Y_FOCUSED` on its a11y ROW child); the default skin draws a
  focus border for it, the same border a focused button gets.

Implementing files: `libs/lens/src/widgets/table.c`,
`libs/lens/src/skin/table.c`, `libs/lens/include/lens/lens.h`; Rust
binding `Frame::table_ex` in `bindings/lens-rs/crates/lens/src/lib.rs`
(`TableOpts.keyboard`, `TableResult.{cursor,cursor_changed,activated,clicked_row}`).
Tests: `tests/lens/test_table.c`,
`bindings/lens-rs/crates/lens/tests/smoke.rs`.

## Alternatives Considered

- **Widget-owned multi-select with Ctrl/Shift semantics.** Rejected: it
  duplicates the host's selection logic inside the widget and still
  cannot answer model resets (the host re-sorts or filters and the
  retained indices mean nothing). The pull callback keeps the host the
  single source of truth.
- **A separate `lens_list` widget.** Rejected: the table already
  virtualizes, scrolls, and skins rows; a second widget would fork all
  of that for a superset of the same feature. The additive opts
  extension gets the same reach at a fraction of the surface.
- **Moving `focused_id` on arrow keys (menu-style nav).** Rejected per
  ADR-0058: focus moves on Tab traversal and pointer press; arrows are
  intra-widget. The cursor is widget state (or host state), not focus.

## Consequences

- `lens_table_opts`, `lens_table_result`, and `lens_grid_row` change
  ABI layout; per ADR-0058 every consumer rebuilds. `lens_table`'s
  default behavior (keyboard off, NULL callbacks) is unchanged.
- `lens_table_state` must never collide in byte size with
  `lens_scroll_state`: `lens_node_state` keys a node's allocation by
  size, and the layout clamp pass plus the scrollbar skin request a
  `lens_scroll_state` on every `is_scroll` node (the table is one). A
  `static_assert` in `libs/lens/src/widgets/table.c` pins the
  distinction — growing the state struct to exactly
  `sizeof(lens_scroll_state)` silently aliased the two layouts and
  reset the retained selection/scroll offset every frame.
- Selectable tables are now focusable: they appear in the tab order
  and take focus on pointer press, per the shared interaction model.
- Docs updated at both declaration sites (the C header contract and
  the Rust `Frame::table_ex` docs), plus the lens-rs changelog.
- The icon callback in the Rust binding returns the raw
  `sys::lens_icon_id` rather than the safe `Icon` enum, because the
  enum surfaces only a subset of glyphs and hosts use
  runtime-registered SVG ids.

## References

- [ADR-0042](0042-lens-virtualized-table.md) — the table this extends.
- [ADR-0058](0058-lens-widget-state-and-instance-styles.md) — state
  bitflags; arrow keys are intra-widget; ABI-extends-in-place rule.
- [ADR-0059](0059-lens-widget-skins.md) — the replaceable skin seam
  (`lens_grid_row` payload).
- [ADR-0062](0062-lens-bidirectional-a11y-action-and-text-changed.md) —
  central activation and a11y DoAction.
