# ADR-0027: Lens retained store — open-addressing id→node map with ENTERING/STABLE/LEAVING GC

- Status: Accepted
- Date: 2026-07-21
- Scope: lens (L2 toolkit). Defines the persistent node map and its
  garbage-collection policy.

## Context

Lens is an immediate-mode façade over a retained core
([ADR-0024](0024-lens-foundations.md)). Every widget the caller builds
maps to a `lens_node` that must persist across frames to carry
animation, focus, prev-frame geometry (for hit-testing), and per-node
user state. Nodes whose ids are *not* seen this frame should not be
freed immediately — a one-frame flicker (or a pop-up that closes and
re-opens) would discard their state — but they also cannot live forever.

Forces:

1. **Stable lookup by id.** The id system
   ([ADR-0026](0026-lens-id-system.md)) produces a `lens_id`; the store
   must resolve it to a node in (amortised) O(1).
2. **Allocate-on-first-touch.** A node is created the first time its id
   is seen and kept across frames; per-node user state
   (`lens_node_state`) is allocated lazily and zeroed on first use.
3. **Graceful expiry.** Nodes not seen this frame should linger briefly
   so animations can play out and transient absences don't drop state,
   then be reaped.
4. **No tombstones.** Open-addressing deletion must preserve probe-chain
   correctness.

## Decision

1. **Open-addressing hash map.** `lens_store` is an array of
   `(id, node*)` slots indexed by a mixed-and-masked id. Capacity is a
   power of two; load factor is kept below 0.75 by doubling. Implemented
   in `libs/lens/src/store/store.c`.
2. **Linear probing.** Collisions walk `(i+1) & (cap-1)`; load-factor
   growth triggers a full rehash into a doubled table.
3. **`lensi_store_touch` is find-or-create.** First touch allocates a
   zeroed node, sets its phase to `LENS_NODE_ENTERING`, and inserts it;
   subsequent touches reset per-frame fields and advance the phase to
   `LENS_NODE_STABLE` once the node has a `prev_rect`.
4. **Three-phase node lifecycle.**
   - `LENS_NODE_ENTERING` — first frame this id was seen.
   - `LENS_NODE_STABLE` — seen this frame and last frame.
   - `LENS_NODE_LEAVING` — not seen this frame; inside the grace window.
5. **Reap at `lens_end`.** `lensi_store_reap` walks the table, advances
   any node not seen this frame toward the grace limit
   (`LENSI_LEAVE_GRACE_FRAMES`, 8), and on expiry frees the node and its
   user state, then re-inserts the following probe cluster to preserve
   open-addressing invariants (tombstone-free deletion by cluster
   rehash).
6. **Per-node user state is one fixed type per id.** After first
   allocation, requesting a different byte size returns `NULL` rather
   than moving the allocation and invalidating prior borrows.

References: `libs/lens/include/lens/lens.h` (`lens_node`,
`lens_node_state`), `libs/lens/src/internal.h` (`lens_node`,
`lens_store`), `libs/lens/src/store/store.c`,
`libs/lens/src/store/node.c`.

## Alternatives Considered

- **Separate chaining with linked lists.** Reject: extra pointer per
  slot and an allocation per node; open-addressing keeps nodes flat and
  cache-friendly.
- **Free nodes immediately when not seen.** Reject: breaks leave
  animations and makes transient popups drop their state on close.
- **Tombstones.** Reject: tombstones accumulate and degrade lookup;
  cluster rehash on delete keeps the table clean without them.
- **Variable-size per-node state with realloc.** Reject: would
  invalidate pointers borrowed by widgets across a frame.

## Consequences

Positive:

- Lookup, insert, and delete are amortised O(1); the table stays
  tombstone-free.
- Nodes (and their user state) survive one-frame flickers and the grace
  window, enabling leave animations and stable focus.
- The phase enum is exposed (`lens_node_phase_of`) so widgets can branch
  on enter/leave for transitions.

Negative:

- A pathological workload that touches many short-lived ids pays for the
  grace-window storage; mitigated by the small (8-frame) window.
- Cluster rehash on delete is O(cluster) — acceptable because the grace
  window bounds how many nodes die per frame.

## References

- [ADR-0024](0024-lens-foundations.md) — foundations.
- [ADR-0026](0026-lens-id-system.md) — the keys this store is indexed by.
- [ADR-0038](0038-lens-node-state-gc.md) — grace-window policy detail.
