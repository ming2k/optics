# ADR-0041: Lens resizable split panel — persisted ratio, draggable divider

- Status: Accepted
- Date: 2026-07-21
- Scope: lens (L2 toolkit). Defines the split-panel primitive.

## Context

A split panel is a two-pane container whose divider the user drags to
redistribute space. Nesting splits yields 3/4-pane layouts. The ratio
must persist per id across frames (and ideally across restarts via the
host), and the divider must be grabbable with a clear drag state
machine.

Forces:

1. **Retained ratio.** The first pane's share is a 0..1 fraction that
   must survive across frames and be readable by the host for
   persistence.
2. **Drag state machine.** Dragging the divider must capture input
   (`active_id`) and track a delta from the press anchor, mirroring the
   scroll-thumb pattern ([ADR-0029](0029-lens-interaction-model.md)).
3. **Min-size floors.** Each pane needs a logical-px floor so a drag
   cannot collapse it to zero.
4. **Horizontal and vertical.** The split direction determines whether
   the divider drags horizontally (left|right panes) or vertically
   (top|bottom panes).
5. **Host cursor hint.** The host reads the returned
   `.hovered`/`.pressed` to set a platform resize cursor.

## Decision

1. **A split is a container.** `lens_split_begin` opens a two-pane
   container whose axis follows the direction
   (`LENS_SPLIT_VERTICAL` → `LENS_ROW`, `LENS_SPLIT_HORIZONTAL` →
   `LENS_COLUMN`). Implemented in `libs/lens/src/widgets/split.c`.
2. **Retained ratio via `lens_node_state`.** `lens_split_state` holds
   `ratio` (0..1, first pane's share; seeded from `opts.ratio`, default
   0.5), `min_first`/`min_second` floors, `thickness`, a `dragging` flag,
   and the drag anchor (`drag_start_ratio`, `drag_start_pos`). The layout
   pass distributes main-axis space by the ratio after reserving the
   divider strip.
3. **Drag state mirrors the scroll thumb.** A press inside the divider
   captures `active_id` and sets `dragging`; subsequent frames mutate
   `ratio` from `(cursor - drag_start_pos) / track_len`, clamped to
   `[min_first, 1 - min_second]`. Release clears `dragging`. Same shape
   as the scrollbar thumb in `scroll.c`.
4. **`lens_split_pane` opens each pane.** The caller calls it twice;
   fill content between, then call again (or `lens_split_end`).
5. **`lens_split_ratio` for persistence.** Returns the current ratio so
   the host can save it across restarts and re-seed via `opts.ratio` on
   the first frame.
6. **`lens_split_opts`.** `ratio` (seed only), `min_first`, `min_second`,
   `thickness` (default 6).

References: `libs/lens/include/lens/lens.h` (Resizable split panel
section), `libs/lens/src/widgets/split.c`, `libs/lens/src/widgets/scroll.c`
(shared drag state machine).

## Alternatives Considered

- **Fixed (non-resizable) panes.** Reject: callers can already do that
   with two `lens_flex` children; a split exists specifically to be
   draggable.
- **Percentage widths on children.** Reject: loses the divider hit
   target and the drag state machine; a split is a coherent primitive.
- **Runtime-mutable direction.** Reject: no caller has needed it; the
   direction is a build-time choice per call.
- **Host-owned drag state.** Reject: the drag must capture `active_id`
   inside lens to stay consistent with every other widget; pushing it
   to the host would split the interaction model.

## Consequences

Positive:

- The ratio persists per id and is host-readable for cross-restart
  persistence.
- The drag state machine is shared with the scrollbar, so behaviour
  (capture, delta, release) is uniform.
- Nesting splits gives 3/4-pane layouts without a new primitive.

Negative:

- A split with very small floors can still be dragged to an awkward
  ratio; the floors are caller-supplied and not enforced beyond
  clamping.
- The divider hit target is `thickness` px; on touch it may need
  enlargement by the host.

## References

- [ADR-0028](0028-lens-flexbox-layout.md) — container layout the split
  participates in.
- [ADR-0029](0029-lens-interaction-model.md) — drag/capture model.
- [ADR-0027](0027-lens-retained-store.md) — `lens_node_state` for the
  retained ratio.
