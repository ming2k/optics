# ADR-0028: Lens two-phase flexbox layout (measure / arrange)

- Status: Accepted (amended by [ADR-0060](0060-lens-single-tree-placement-and-z-bands.md):
  measure/arrange became ABS-aware, and overlay placement moved from the
  parallel `lensi_overlay_layout` pass into arrange's ABS segment)
- Date: 2026-07-21
- Scope: lens (L2 toolkit). Defines the layout algorithm.

## Context

The retained tree built each frame must be turned into absolute pixel
rectangles before drawing or hit-testing. Lens needs a layout model that
handles containers (rows/columns), flex grow/shrink, fixed sizes, min/max
constraints, cross-axis alignment, padding, and gaps — and that the same
algorithm must also lay out overlay sub-roots whose placement is computed
after the base tree.

Forces:

1. **Parent-size-dependent children, child-size-dependent parents.** A
   container's main-axis size depends on its children, but flex children
   depend on the container's free space. This is the classic two-pass
   flexbox problem.
2. **Min/max floors and ceilings.** Widgets must not collapse below a
   usability floor or grow past a stated maximum; contradictory ranges
   resolve in favour of the minimum.
3. **Reusable for overlays.** The overlay layer
   ([ADR-0037](0037-lens-overlay-layers.md)) places a sub-root and then
   needs to lay out the subtree inside a given rect — same algorithm, a
   different entry point.
4. **Pure, deterministic.** Layout runs every frame; it must be free of
   side effects and converge in one measure + one arrange pass.

## Decision

1. **Two-pass flexbox.** `lensi_layout_solve` runs a bottom-up *measure*
   pass that computes each node's intrinsic size, then a top-down
   *arrange* pass that distributes free space along the main axis
   according to flex grow, applies cross-axis alignment, and writes
   `final_rect`. Implemented in `libs/lens/src/layout/solve.c`.
2. **Flex grow + fixed sizes + min/max.** Each node carries `flex_grow`,
   `fixed_w`/`fixed_h` (0 = intrinsic), and `min_w`/`max_w`/`min_h`/`max_h`
   (0 = unconstrained). `constrain_extent` applies the ceiling then the
   floor so a contradictory range resolves in favour of the minimum.
3. **Cross-axis alignment.** `lens_align` (`LENS_START`, `LENS_CENTER`,
   `LENS_END`, `LENS_STRETCH`) controls cross-axis placement inside the
   parent; `LENS_STRETCH` fills the parent's cross axis.
4. **Padding and gap.** Containers carry `pad` (all sides) and `gap`
   (main axis between children); both reduce free space before flex
   distribution.
5. **Implicit root.** `lens_begin` opens a column container covering
   `input.display_size`, so callers build directly into a laid-out root.
6. **Subtree entry point for overlays.** `lensi_layout_subtree(n, rect)`
   arranges an already-measured subtree into a given rect — used by the
   overlay layer to place a sub-root after the base tree is arranged.
7. **Scroll clamping.** `lensi_scroll_clamp` runs after arrange so scroll
   offsets stay within the resolved content bounds.

References: `libs/lens/include/lens/lens.h` (Containers / layout section),
`libs/lens/src/layout/solve.c`, `libs/lens/src/layout/tree.c`.

## Alternatives Considered

- **Single-pass layout.** Reject: cannot resolve flex grow against free
  space that depends on children's intrinsic sizes.
- **Constraint solver (Cassowary-style).** Reject: far heavier than the
  flexbox model lens needs; the two-pass measure/arrange is sufficient
  and deterministic.
- **Per-widget ad-hoc layout.** Reject: every container would
  re-implement padding/gap/alignment; the shared solver keeps behaviour
  consistent.
- **CSS Grid.** Reject: grid is powerful but more complex than the row/
  column + flex model lens widgets need.

## Consequences

Positive:

- One algorithm covers the base tree and overlay sub-roots.
- Min/max constraints give widgets predictable floors without per-widget
  arithmetic.
- Deterministic single measure + single arrange keeps per-frame cost
  linear in node count.

Negative:

- Flexbox semantics (especially shrink) are subtle; bugs in distribution
  surface as visual misalignment rather than crashes.
- Cross-axis stretch is the default, which surprises callers who expect
  leaf widgets to keep their intrinsic cross size.

## References

- [ADR-0024](0024-lens-foundations.md) — foundations.
- [ADR-0037](0037-lens-overlay-layers.md) — overlay sub-root layout via
  `lensi_layout_subtree`.
