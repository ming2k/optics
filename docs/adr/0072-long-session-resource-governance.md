# ADR-0072: Long-session resource governance — bounded queues, O(1) eviction, live-list GC

- Status: Accepted
- Date: 2026-08-21
- Scope: flux core (retire queue), flux-text (glyph cache, itemizer
  scratch), lens (store GC). Records the design invariants behind the
  v0.0.24+ hardening pass and the regressions that motivated them.

## Context

An audit of long-session behaviour found four structural weaknesses that
shared one shape: **a bound that existed in spirit but not in
mechanism**.

1. The retire FIFO had no entry cap. Normal operation drains it every
   frame (the watermark advances in `begin_frame`), but a host that
   releases resources while submitting no frames — minimised, rebuilding
   pools, or a wedged GPU — parks zombies indefinitely, each pinning its
   GPU memory. The only bound was an assumption about the caller.
2. The glyph cache's LRU eviction scanned the whole table per evicted
   glyph. At production scale (16384 slots) a frame rasterising N new
   CJK glyphs paid N full scans of ~1 MB — on the frame path.
3. The glyph cache's tombstone sweep was chained under the load-factor
   branch as `else if`. At saturation, live is pinned at cap/2, so the
   load branch always fired and the sweep was **dead code precisely
   where evictions mint tombstones fastest**. Probe chains degraded
   toward a full-table walk per miss.
4. lens's store reap scanned all `cap` slots every frame — O(cap) even
   at 1 % load — so a transient population spike raised per-frame cost
   for the whole shrink-hysteresis dwell (300 frames).

## Decision

**Every queue, cache, and periodic scan carries its bound in code, not
in caller discipline.** Concretely:

1. **Retire FIFO backpressure** (`FLUX_RETIRE_MAX_PENDING = 4096`):
   parking past the bound forces a full drain (queue wait + watermark
   advance + destroy-everything). The stall is deliberate and strictly
   better than unbounded GPU-memory growth. Normal frames-in-flight
   operation never approaches the bound.
2. **Intrusive LRU ring for the glyph cache**: doubly-linked slot-index
   ring, MRU at head; eviction pops the tail in O(1). 8 B/entry of host
   memory (~128 KB at 16 K entries) buys exact O(1) eviction. The ring
   is rebuilt in order across rehash by walking the old ring.
3. **Admission control is two independent rules, never chained**:
   working-set bound (live > max_cap/2 → evict LRU) and occupancy
   hygiene ((live+tomb) > cap/2 → grow below max_cap; tomb > cap/4 →
   same-cap rehash). Each is evaluated on its own trigger, so neither
   starves at the other's saturation point.
4. **Live-list GC for the lens store**: a singly-linked list of occupied
   slots threaded through a `next_live` index; reap walks O(live).
   Cluster clears are transactional — unlink every displaced entry
   first, re-insert after — because `store_insert` may rehash
   mid-cluster, and any slot index saved before the clear is
   untrustworthy (the walk re-anchors at the head).
5. **Per-context high-water scratch** for itemizer working arrays
   (codepoints/offsets/bidi types/levels): grow-to-max-seen, reuse
   thereafter, release on `flux_text_destroy` / `flux_text_compact`.
   This replaces per-call malloc/free with an 8-branch manual cleanup
   at every early exit — the classic leak-on-error shape.

## What we deliberately did NOT do

- **No closed-form flex solver.** `flex_level` looks O(n²) but converges
  in a handful of passes even on adversarial ratio ladders (measured:
  ≤10 passes at n=500). An exact O(n log n) sort+prefix-scan
  replacement — verified equivalent over 400 000 randomised cases —
  measured 2–3× *slower* at every realistic size from qsort constant
  overhead. The iterative form stays, with the measurements recorded in
  a comment so the next reader does not repeat the experiment.
- **No internal watchdog.** The library's obligation is bounded
  frame-path cost and one-time costs parked in create/init; deadline
  enforcement belongs to the host. (See also: consumer-neutral wording
  in the comments that used to cite a specific downstream's watchdog
  parameters.)

## Verification

- `test_retire_backpressure` — 2 × 16384 releases with a frozen
  watermark; steady-state allocator counts must match across windows.
- `test_glyph_cache` — production-scale saturation (init 256 / max
  16384): live pinned at the ceiling, LRU ring consistency checked every
  512 inserts, tombstones bounded at saturation, survivors are exactly
  the most recent keys.
- `test_store_spike` — 70…100 000-node spikes with cluster clears and
  mid-reap rehashes (two real bugs found during the rework are pinned by
  this test).
- `test_text_soak` — 9000-distinct-glyph working set cycles the cache;
  steady replay is near-pure hits; two identical churn windows land on
  identical counters (no per-frame growth); no atlas clear storm.
- CI runs the suite with LeakSanitizer enabled (suppressing only
  fontconfig's process-lifetime cache and ICD driver caches).

## Consequences

- A host that idles while releasing resources pays one bounded stall
  per 4096 releases instead of silent unbounded memory growth.
- Worst-case glyph eviction cost drops from O(cap) per glyph to O(1);
  a CJK scroll burst pays one FreeType rasterisation per new glyph and
  nothing else.
- lens per-frame GC cost tracks the live tree, not historical peak
  capacity.
- The cache's observable behaviour (eviction counts, LRU victim choice,
  `put()` never returning NULL when entries are evictable) is unchanged;
  the rework is internal, and the pre-existing unit contract — including
  `evictions == 1` for a single over-cap insert — still passes.

## References

- ADR-0020 — retire queue origins (i915 GPU-hang hazard)
- ADR-0021/0022 — watermark semantics, deferred uploads
- ADR-0027 — lens retained store; ADR-0038 — node GC grace window
- ADR-0030 (amended) — display-list replay, the other long-session lever
