---
id: ADR-0114
title: "Incremental non-convex candidate list for ear clipping (legacy O(n^2) scan removed)"
status: superseded
date: 2026-10-17
superseded_by: "ADR-0115"
---

# ADR-0114: Incremental non-convex candidate list for ear clipping

- Status: Superseded by [ADR-0115](0115-analytic-curve-kernel-single-geometry-authority.md).
  The candidate list accelerated a scan that existed only because ear
  clipping was the geometry authority; coverage strips remove ear
  clipping entirely. Retained as negative knowledge, including the
  ~1 % stall-decision divergence on self-intersecting input — the exact
  out-of-contract behaviour ADR-0115 removes by making winding a computed
  count rather than a clip outcome.
- Date: 2026-10-17
- Scope: `libs/flux/src/canvas/geometry_tess.c` (`ear_clip_contour`),
  `libs/flux/src/canvas/internal.h` (signature + scratch),
  `libs/flux/src/canvas/canvas.c` (scratch lifecycle).
- Amends: [ADR-0113](0113-convex-fast-path-tessellation.md) (adds the
  concave accelerator and deletes the remaining legacy O(n²) scan).
- Depends on: [ADR-0011](0011-hole-bridging-tessellator.md),
  [ADR-0014](0014-stencil-then-cover.md).

## Context

ADR-0113 removed the quadratic blow-up for **strictly convex** fills via
an O(n) fan fast path. Every other fill — concave contours, and the
merged polygons produced by hole bridging — still ran the original
`ear_clip_contour` scan: for each candidate ear it walked the **entire
remaining ring** testing `point_in_triangle`, i.e. **O(n²)** in the
contour size. ADR-0011's own "when to revisit" list named exactly this
("the bridge-edge O(n·m) computation shows up in profiles"), and a hole
bridge payload is O(n·m) on top of the O(n²) clip.

Measured on the in-tree benchmark harness (optimised build), the legacy
clip on realistic concave shapes:

| Shape | Vertices | Legacy clip |
| :--- | ---: | ---: |
| gear, 24 teeth | 192 | 55 µs |
| gear, 40 teeth | 320 | 193 µs |
| gear, 80 teeth | 640 | 822 µs |

Time more than triples per doubling — the quadratic is real, and every
concave vector path (icons, glyph outlines, badges, charts) paid it on
its first tessellation.

The expensive scan is also the *legacy burden* the codebase should not
carry once a correct replacement exists.

## Decision

1. **Incremental non-convex candidate list (FIST / FitzHugh).** For a
   simple polygon a vertex can only block a CCW ear if it is itself
   reflex *or collinear*; a strictly convex vertex provably cannot lie
   inside-or-on a CCW ear triangle. `ear_clip_contour` therefore
   maintains the set of non-strictly-convex vertices and scans only it
   per ear — O(C) instead of O(n), C the live candidate count.

2. **Candidate test is `cross <= 0`, not `cross < 0`.** Using `<=`
   (reflex *or* collinear) keeps the candidate set a strict superset of
   everything the legacy inclusive `point_in_triangle` boundary test
   could reject, so for the ear clip's **contract input — simple
   polygons** — ear selection, and therefore the emitted triangulation
   (same area, same triangle count), is identical to the legacy scan,
   including for the duplicate bridge endpoints that hole merging
   introduces (the corner-equality skip is preserved verbatim).

3. **O(1) incremental maintenance.** The list is a compact array plus an
   id→slot map, so removal is a swap, never a search. Clipping one vertex
   changes the convexity of exactly its two neighbours, so membership is
   refreshed in O(1) per clip; the whole clip is O(n + Σ C_i).

4. **The legacy full-ring scan is deleted.** `reflex` (2·n `uint32_t` of
   caller scratch, `[candidates | id→slot map]`) is now required; there
   is no O(n²) fallback left to carry. Contours up to
   `FLUX_CANVAS_PATH_SCRATCH_CAP` use the canvas's own scratch
   (`flux_canvas::scratch_reflex`, `2·cap` entries, allocated/released
   with the other scratch buffers); the oversized merged-polygon path
   allocates matching heap scratch alongside its existing heap buffers.

## Alternatives Considered

- **Reflex-only candidate set (`cross < 0`).** Rejected: a collinear
  vertex lies on the ear boundary, which the legacy inclusive
  `point_in_triangle` treats as blocking. Dropping collinear vertices
  from the candidate set would select different ears on inputs with
  collinear runs (common: rounded-rect arcs, RDP-flattened edges) and
  change raster output / stall behaviour. `<= 0` is the safe superset.

- **Retain the legacy scan as a fallback.** Rejected: it is precisely
  the O(n²) burden being removed. Keeping it would require every future
  reader to reason about two algorithms and would leave a path that is
  never exercised in production (all callers can supply scratch).

- **Z-order / bucketed spatial hash (Mapbox-earcut style).** This is the
  only structure that removes the worst case *asymptotically* (true
  O(n log n)). Deferred, not rejected: it is a substantially larger and
  riskier change (dynamic bucket tree, exact-equivalence proof across
  holes/duplicates). The candidate list already removes the quadratic
  for the dominant real cases (see Consequences) and is a strict,
  verified refinement; the spatial hash can layer on top of the same
  `ear_clip_contour` seam without disturbing this design.

- **Replace ear clipping with monotone-polygon decomposition.** Rejected
  for the same risk/scope reason: a full re-tessellation changes the
  emitted triangle partition and every downstream assumption
  (stencil-fallback ordering, cache keys, GPU↔CPU oracle). The candidate
  list keeps the partition identical by construction.

## Consequences

Positive:

- **Convex fills** (circles, rounded rects, polygons — the dominant UI
  case) are O(n) via ADR-0113's fan path.
- **Concave fills with few reflex vertices** (glyph outlines, most icon
  art) become near-linear. Measured on the gear shapes:

  | Shape | Vertices | Legacy | Candidate list | Speed-up |
  | :--- | ---: | ---: | ---: | ---: |
  | gear, 8 teeth | 64 | 6.5 µs | 2.0 µs | 3.3× |
  | gear, 24 teeth | 192 | 55 µs | 16 µs | 3.4× |
  | gear, 40 teeth | 320 | 193 µs | 51 µs | 3.8× |

- **Identical output for the contract input (simple polygons).** The
  FIST lemma — a convex vertex can never lie inside a CCW ear of a
  simple polygon — is what licenses dropping convex vertices from the
  candidate set. Verified by a differential harness that ran the legacy
  scan and the candidate list and compared stall status, triangle count,
  total triangulated area, and per-triangle validity, over **~500 000
  random simple polygons**: radial stars, sharp stars, noisy
  flowers/circles (n = 3…~123), square-tooth combs and zigzags
  (non-star-shaped), and 35 % of them with exact collinear midpoints
  inserted. Zero divergences, zero vertices found inside any output
  triangle. Post-deletion the exact triangle-count assertions
  (`test_canvas_tess`) and the GPU↔CPU pixel oracle
  (`test_canvas_consistency`) continue to pass.
- **No legacy burden.** ~40 lines of the O(n²) full-ring scan are gone;
  there is a single clip algorithm.
- No public ABI change; `ear_clip_contour` is an internal symbol. The
  canvas gains 16 KiB of scratch (`2·cap` `uint32_t`).

Negative / follow-up:

- The **worst case remains superlinear** when the candidate set is
  O(n) — a comb (≈half reflex) is only ~2× faster (n=1602: 2.72 ms →
  1.37 ms). A guaranteed O(n log n) needs the deferred spatial hash;
  until then, pathological many-reflex fills stay Θ(n·C).
- **Self-intersecting (non-simple) input is out of contract and behaves
  differently.** The FIST lemma holds only for *simple* polygons. Feeding
  the clip a self-intersecting path is undefined in both the legacy and
  the candidate algorithm, and a differential run over ~1 M scrambled
  (self-crossing) polygons found ~1 % where the two disagree on whether
  the clip **stalls** — i.e. one produces a partial triangulation while
  the other routes to the stencil fallback (ADR-0014). Both outcomes are
  already undefined for such input, and the stencil fallback is the
  intended handling; but this is a real behavioural difference from the
  legacy scan and is recorded here rather than hidden. A future
  hardening could make non-simple input deterministically stall (route
  to stencil) by validating the completed triangulation's area, which
  would remove the divergence — at the cost of an extra O(n) pass.
- The candidate list is rebuilt once per contour (O(n)) in addition to
  the clip; negligible against the scan it replaces.

## Verification

- Differential equivalence legacy-vs-candidate over ~500 000 random
  simple polygons (radial/sharp stars, noisy flowers/circles, combs,
  zigzags; 35 % with collinear midpoints): identical stall status,
  triangle count, and triangulated area, and no vertex strictly inside
  any output triangle. Zero divergences.
- Self-intersecting-input differential (~1 M scrambled polygons):
  documents the ~1 % stall-decision divergence on out-of-contract input
  (see Consequences).
- `tests/flux/unit/test_canvas_tess.c` — exact triangle counts incl. the
  2042-vertex hole-bridge case.
- `tests/flux/unit/test_canvas_tess_cache.c` — cache hits, stalls, and
  the self-intersecting heptagram that must still stall to stencil.
- `tests/flux/integration/test_canvas_consistency.c` — GPU↔CPU
  pixel-for-pixel oracle.
- Full suite: 140/140 `meson test` green (unit + integration + prism
  golden + leak/retire gates).
- `tests/flux/bench/bench_tess.c` now also tracks gear (realistic
  concave) and comb (worst case) so regressions are visible.

## References

- [ADR-0113](0113-convex-fast-path-tessellation.md) — convex fan
  fast path; this ADR amends it.
- [ADR-0011](0011-hole-bridging-tessellator.md) — hole-bridging
  ear-clip; its "revisit when O(n·m) shows up in profiles" trigger.
- [ADR-0014](0014-stencil-then-cover.md) — the stall → stencil fallback
  that the candidate scan must still detect.
- Implementation: `libs/flux/src/canvas/geometry_tess.c`
  (`tess_is_candidate`, `ear_clip_contour`); scratch in
  `internal.h` / `canvas.c`.