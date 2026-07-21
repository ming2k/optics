# ADR-0038: Lens node state GC — 8-frame grace window for leaving nodes

- Status: Accepted
- Date: 2026-07-21
- Scope: lens (L2 toolkit). Defines the leaving-node garbage-collection
  policy.

## Context

The retained store ([ADR-0027](0027-lens-retained-store.md)) keeps a
node alive as long as its id is seen each frame. When an id stops being
seen (the widget is not built this frame), the node's state (animation
curve, focus, per-node user data) should not be freed instantly — a
one-frame absence is common (a pop-up that closes and re-opens, a
branch that flickers) and immediate free would drop leave animations
and force a re-alloc on return. But nodes also cannot live forever;
the store would grow unboundedly.

Forces:

1. **Leave animations.** A widget that disappears should be able to play
   a fade/slide for a few frames after its last build.
2. **Transient absences.** A widget that is not built for one or two
   frames and then returns should keep its state (hover curve, scroll
   offset, text buffer).
3. **Bounded memory.** Nodes that never come back must be reaped.
4. **Tombstone-free deletion.** Open-addressing deletion must preserve
   probe-chain correctness (rehash the following cluster).

## Decision

1. **8-frame leaving grace window.** `LENSI_LEAVE_GRACE_FRAMES` is 8. A
   node not seen this frame enters `LENS_NODE_LEAVING` and increments
   `leaving_frames`; it is reaped when the counter exceeds the window.
   Defined in `libs/lens/src/internal.h`, executed by
   `lensi_store_reap` in `libs/lens/src/store/store.c`.
2. **Reap at `lens_end`.** After the frame is built,
   `lensi_store_reap` walks the table: alive nodes are skipped; leaving
   nodes count down; expired nodes are freed (user state + node) and
   their slot is cleared, then the following probe cluster is re-inserted
   to preserve open-addressing invariants.
3. **Per-node user state freed with the node.** `lens_node_state`
   allocations are owned by the node; reap frees them alongside.
4. **Phase exposed.** `lens_node_phase_of` returns
   `ENTERING`/`STABLE`/`LEAVING` so widgets and tests can branch on a
   node's lifecycle (e.g. to drive a leave animation inside the grace
   window).

References: `libs/lens/include/lens/lens.h` (`lens_node_phase`,
`lens_node_phase_of`), `libs/lens/src/internal.h`
(`LENSI_LEAVE_GRACE_FRAMES`), `libs/lens/src/store/store.c`
(`lensi_store_reap`), `libs/lens/src/store/node.c`.

## Alternatives Considered

- **Free immediately on absence.** Reject: drops leave animations and
  forces re-alloc when a widget returns after a one-frame flicker.
- **Infinite lifetime / explicit close.** Reject: the store would grow
  unbounded for any UI that dynamically adds and removes widgets.
- **One global LRU.** Reject: LRU would evict the wrong nodes (recently
  used but not this frame) and complicate the open-addressing map.
- **Per-node configurable grace.** Reject: no widget has demonstrated a
  need for a different value; one tunable keeps the policy uniform.

## Consequences

Positive:

- Leave animations have 8 frames to play out.
- A widget that flickers out for a frame or two keeps its state.
- Memory is bounded: a node that never returns is reaped within 8 frames
  of its last appearance.

Negative:

- A workload that churns many short-lived ids pays for up to 8 frames of
  storage per id; mitigated by the small window and the power-of-two
  store growth.
- The grace window is a compile-time constant; a caller cannot extend it
  for a specific widget.

## References

- [ADR-0027](0027-lens-retained-store.md) — the store and its
  phase/lifecycle model.
- [ADR-0024](0024-lens-foundations.md) — retained/immediate split.
