# ADR-0042: Lens virtualized table / data grid

- Status: Accepted
- Date: 2026-07-21
- Scope: lens (L2 toolkit). Defines the virtualized table primitive.

## Context

A data grid backed by thousands of rows must not build a widget per
row — the per-frame cost would be linear in row count, blowing the arena
and the layout pass. The scrollbar must reflect the full row count, but
only the rows intersecting the viewport may be built and drawn, so cost
is O(visible) regardless of `row_count`.

Forces:

1. **O(visible) cost.** Building, laying out, and drawing must depend on
   the visible window, not the total row count.
2. **Full-row-count scrollbar.** The thumb reflects `row_count` so the
   user can drag to any position.
3. **Pull-based cells.** The caller supplies a cell callback so the
   table does not own or copy the data; cells are requested by
   `(row, col)` for the visible window only.
4. **Selection persists.** The selected row must survive across frames
   (and scrolling) in the retained store.
5. **Reusable scroll machinery.** The scrollbar drag state machine is
   the same as a scroll area; reuse it rather than re-implementing.

## Decision

1. **Scroll-area-backed grid.** `lens_table` is a scroll container whose
   content height is `row_count * row_height`; only rows intersecting
   the viewport are positioned and drawn (as text). Implemented in
   `libs/lens/src/widgets/table.c`.
2. **Pull callback for cells.** `lens_table_cell_fn` returns the string
   for `(row, col)`; the table calls it only for visible cells, so cost
   is O(visible rows × columns).
3. **Full-row-count scrollbar.** The scrollbar geometry is computed from
   `row_count` and the viewport; the thumb is draggable with the same
   state machine as a scroll area (`lens_table_state` mirrors
   `lens_scroll_state`: offset, thumb geometry, track length, scroll
   range, dragging/hovering, drag anchor).
4. **Selection in retained state.** `selected` (-1 = none) lives in
   `lens_table_state`; clicking a row sets it and reports
   `selection_changed`. Selection persists across scroll and frames.
5. **Column model.** `lens_table_column { title, width, align }`. A
   fixed `width` sizes the column; zero-width columns share the
   remainder as equal flex. `lens_table_opts { row_height, show_header,
   selectable, zebra }` controls row height (default `font_size +
   padding`), header rendering, click-to-select, and zebra striping.
6. **`lens_table_result`.** `{ selected, selection_changed, clicked }`
   so the caller can branch without a follow-up query.

References: `libs/lens/include/lens/lens.h` (Virtualized table / data
grid section), `libs/lens/src/widgets/table.c`, `libs/lens/src/widgets/scroll.c`
(shared scrollbar state machine).

## Alternatives Considered

- **Build a widget per row.** Reject: O(row_count) per frame; a
   10 000-row grid would blow the arena and the layout pass.
- **Host-owned virtualization (caller slices the data).** Reject: every
   caller re-implements the viewport math, scrollbar, and selection
   state; a table primitive is the right granularity.
- **Push-based cells (caller fills an array).** Reject: forces the
   caller to materialise the visible window even when the table could
   compute it cheaper; pull keeps the data in the caller's domain.
- **External virtual list library.** Reject: the scroll-area substrate
   already exists in lens; a table is a thin, cohesive layer on top.

## Consequences

Positive:

- A grid of any row_count has the same per-frame cost as a single screen
  of rows.
- Selection and scroll offset persist per id.
- The cell callback keeps data ownership with the caller.

Negative:

- The cell callback is invoked per visible cell per frame; a caller with
  expensive cell computation should cache. Acceptable: the contract is
  pull, and caching is the caller's choice.
- Variable row heights are unsupported (the viewport math assumes a
  fixed `row_height`); a future variant could relax this.

## References

- [ADR-0028](0028-lens-flexbox-layout.md) — scroll container layout.
- [ADR-0029](0029-lens-interaction-model.md) — click selection and
  scroll-clip hit-testing.
- [ADR-0027](0027-lens-retained-store.md) — `lens_node_state` for
  selection and scroll offset.
