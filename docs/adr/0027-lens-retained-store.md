---
id: ADR-0027
title: "Lens retained store — open-addressing id→node map with ENTERING/STABLE/LEAVING GC"
status: accepted
date: 2026-07-21
scope: ui (L2 toolkit)
superseded_by: null
negative_knowledge: true
---

# ADR-0027: Lens retained store — open-addressing id→node map with ENTERING/STABLE/LEAVING GC

- Status: Accepted
- Date: 2026-07-21
- Scope: lens (L2 toolkit). Defines the persistent node map and its
  garbage-collection policy.
- Amended: 2026-11-25 — shrink sizing rule, the store's structural
  invariants and their debug-build enforcement, and the model-based
  verification that guards them (see the amendment below).

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

## Amendment — 2026-11-25: shrink sizing, invariants, verification

**What failed.** The shrink path (added after this ADR, alongside the
live list) chose its target with `while (want > MIN_CAP && count * 8 >
want) want /= 2` — the comparison was inverted, so it halved *while the
table was already too small*. Any population in (cap/16, cap/8] after a
spike, held for the 300-frame dwell, was rehashed into 64 slots: e.g.
100 live nodes at cap 1024 (a file manager leaving a large thumbnail
grid). Linear probing into a full table never finds a free slot, so
`store_grow` spun forever on the UI thread. It shipped through v0.0.86
because no test held a surviving population across the dwell — the
spike test let everything expire, and the public-API fuzz harness mints
a fresh id per widget, so neither could reach the band.

**Decision.**

1. **Shrink target.** After `OPTICS_UI_STORE_SHRINK_FRAMES` consecutive
   frames at load <= 1/8, halve while the *next* size keeps load <= 1/4
   (`count * 4 <= want / 2`), never below `OPTICS_UI_STORE_MIN_CAP`. The
   dwell restarts after any attempt, successful or not.
2. **Rehash precondition.** `store_grow` refuses (returns
   `OPTICS_GFX_ERROR_INVALID_ARGUMENT`) any capacity at which the table
   would be at or above 3/4 load. A wrong caller can now only fail to
   resize; it cannot hang.
3. **Invariants, stated and checked.** `optics_ui__store_validate`
   proves: capacity is a power of two >= 4; load < 3/4 (so every probe
   terminates); every occupied slot is reachable from its home slot; ids
   are unique; the live list threads every occupied slot exactly once
   with consistent back-links and tail. Debug builds
   (`OPTICS_UI_STORE_VALIDATE`, on unless `NDEBUG`) run it at the end of
   every reap whose frame changed the table's structure (a wrapping
   `epoch` counts inserts, erases, and rehashes) and abort with the
   broken invariant — at the frame that broke it, not later as a hang.
4. **Model-based verification.** `tests/ui/store_model.h` drives
   `store.c` directly against a reference model (who must be present,
   at which retained pointer) and checks, every changed frame: the
   invariants, model agreement, pointer stability, hysteresis (no
   shrink before the dwell), and proportionate capacity (the table must
   shrink after a sustained collapse). It backs a deterministic unit
   test (`test_store_model`: the freeze regression, a shrink-band sweep
   for capacities 128–16384, gradual decline, threshold wobble, grace
   re-entry, seeded random walks) and a libFuzzer harness
   (`fuzz_ui_store_model`, byte programs with population *holds* so the
   dwell is reachable) with a committed seed corpus. Mutation-checked:
   the original inverted loop, a wrong backward-shift test, a forgotten
   reap cursor, a grace off-by-one, dropped back-links, a hysteresis that
   never resets, and a disabled shrink each fail it.

**Negative knowledge.** A table that can resize in both directions needs
tests that *hold* a population through every resize trigger, not just
spike-and-expire; crash-only fuzzing over the public API cannot reach
hysteresis-gated paths. State the load invariants in code and check them
in debug builds — the bug was a one-character class of error that the
invariant check reports immediately.

## References

- [ADR-0024](0024-lens-foundations.md) — foundations.
- [ADR-0026](0026-lens-id-system.md) — the keys this store is indexed by.
- [ADR-0038](0038-lens-node-state-gc.md) — grace-window policy detail.
