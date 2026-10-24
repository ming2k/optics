---
id: ADR-0113
title: "Convex fast-path tessellation and winding-robust convexity test"
status: superseded
date: 2026-10-17
superseded_by: "ADR-0115"
---

# ADR-0113: Convex fast-path tessellation and winding-robust convexity test

- Status: Superseded by [ADR-0115](0115-analytic-curve-kernel-single-geometry-authority.md);
  amended by [ADR-0114](0114-ear-clip-candidate-list.md). The convex fan
  fast path optimized a triangulator that no longer exists: coverage is
  computed from curve strips, so there is no ear-clip entry point to gate.
  Retained as negative knowledge (the quadratic blow-up measurements and
  the winding-total convexity test remain relevant to any future
  triangulating consumer).
- Date: 2026-10-17
- Scope: `libs/flux/src/canvas/geometry_tess.c`, the `ear_clip_contour`
  entry point, and the CPU/GPU triangle output for convex fills.
- Depends on: [ADR-0011](0011-hole-bridging-tessellator.md),
  [ADR-0014](0014-stencil-then-cover.md).

## Context

`ear_clip_contour` is the hole-bridging ear-clip triangulator introduced
by ADR-0011 and extended by ADR-0014. It is **O(n²)** in the input
size: for every candidate ear it scans the entire remaining ring for a
blocking vertex before committing (plus a point-in-triangle pass over
all remaining vertices). The scan exists because an arbitrary simple
polygon can hide a *reflex* vertex inside an ear's triangle — but that
guard is paid even when the polygon is a convex shape where no reflex
vertex can possibly exist.

Profiling on the actual rasterise path shows the blow-up is not
theoretical:

| Polygon (regular CCW) | Vertices | Time/iter (before) |
| :--- | ---: | ---: |
| tess_regular_32gon | 32 | 1.11 µs |
| tess_regular_64gon | 64 | 4.47 µs |
| tess_regular_128gon | 128 | 18.2 µs |
| tess_regular_256gon | 256 | 70.8 µs |
| tess_regular_512gon | 512 | 279 µs |
| tess_regular_1024gon | 1024 | 1.06 ms |
| tess_regular_2048gon | 2048 | 7.05 ms |

Time roughly quadruples per doubling of vertex count — a clean
quadratic. A large filled circle, rounded rectangle, polygon, or a
high-zoom vector path flattens to several hundred points routinely, and
every one of those pays hundreds of microseconds **per fill**, per frame.

The natural fix is a convex fast path: a strictly-convex, consistently
oriented polygon has **no reflex vertices**, so every triangle
`(v0, vi, vi+1)` is a valid ear and the blocking scan is pure wasted
work. Fan-triangulating such a polygon is O(n). Note the fan does **not**
emit the same *triangle partition* the ear clipper would — the legacy
clipper starts its walk from the current cursor with `(v_{n-1}, v0, v1)`,
while the fan emits `(v0, v1, v2), (v0, v2, v3), …` from vertex 0 — but
both produce a CCW triangle set covering the exact same convex area (n−2
triangles), so the rasterised output is bit-identical. Galvanised on
`test_canvas_consistency`, which proves the GPU and CPU backends agree
pixel-for-pixel; the triangle *ordering* differs and is unobservable to
downstream consumers (submission order, not geometry).

The subtle trap: detecting convexity by "all consecutive turns have the
same sign" is **not sufficient**. A self-intersecting star polygon placed
on a circle (join every k-th vertex) has all-positive local exterior
turns — every exterior angle lands in (0, π) — yet winds K > 1 times and
crosses itself. Such a polygon must NOT take the fan fast path: an
unthinking fan would silently emit a triangle soup whose parity differs
from the ear clipper's stencil-fallback stall, changing rendered output
for a documented edge case (ADR-0014). The sign test alone therefore
mis-classifies the exact self-intersecting paths the stencil fallback
exists to serve.

## Decision

1. **Linear convexity test at the top of `ear_clip_contour`.**
   `contour_is_strictly_convex_ccw` runs a single O(n) pass over the
   CCW-normalised contour requiring:
   - every consecutive exterior turn strictly positive in orientation
     (`cross2d(ab, bc) > 0`), i.e. no reflex and no degenerate
     collinear corner, and
   - the **total signed exterior turning** to land in
     `(2π − 0.5, 2π + 0.5)`. A simple convex polygon winds exactly
     once (turning sums to +2π); a K-times-wound self-crosser totals
     K·2π. The 0.5 rad tolerance absorbs float summation drift across
     many vertices while keeping K ≥ 2 (~4π = 12.57, over 2 rad away)
     unambiguously out of range.

2. **O(n) fan triangulation fast path.** When the contour passes the
   convexity test, `fan_clip_contour` emits `(v0, vi, vi+1)` for
   `i = 1..n−2` — the identical CCW fan the ear clipper would produce —
   and returns. Capacity exhaustion is handled like the legacy clipper:
   `emit_tri` bounds-checks, silently drops the overflow, and records
   `FLUX_FAIL`; it is **not** a stall, so a convex contour never routes
   to the stencil fallback.

3. **Every non-convex input falls through on the legacy path, plus a
   single O(n) predicate scan.** A non-convex (concave, collinear-run, or
   K-wound self-intersecting) contour skips the fan and runs the unchanged
   O(n²) ear clip exactly as before; the only difference is the added
   linear convexity pass (negligible next to O(n²)). No stencil fallback
   trigger, cache-key, or render-plan behaviour changes.

> Scope of this ADR: it removes the measured quadratic specifically for
> **strictly-convex fills** — the large circle / rounded-rect / polygon /
> high-zoom-path cases that dominate raster-cost profiles. It does **not**
> claim to remove O(n²) for concave or hole-bearing paths; that requires a
> full O(n log n) triangulator (see Alternatives) and stays a follow-up.

## Alternatives Considered

- **Fast path gated on sign-of-crosses only.** Rejected: the regression
  suite (`test_canvas_tess_cache`) pins the self-intersecting heptagram
  `{7/3}` — which has all-positive local turns — to stall and fall back
  to stencil. The sign test mis-classifies it as convex and the generated
  fan broke that invariand. The winding-total discriminator is the
  minimal O(n) test that separates winding-one convex from K-wound
  self-crossers exactly.

- **Replace the ear clipper with an O(n log n) triangulator outright.**
  Rejected for this change: it is a far larger behavioural surface (all
  concave/self-intersect/hole-bridging semantics, cache interplay,
  GPU/CPU oracle equivalence, and stencil fallback ordering). This ADR
  removes the measured quadratic for the *dominant* real-world case —
  convex fills — while leaving the full legacy semantics untouched; an
  O(n log n) ear-clip replacement can layer on top later without
  disturbing the fast path's correctness.

- **Fix the convexity test by checking centroid-inside / bbox.**
  Rejected: those tests reject many genuinely convex polygons (a
  centroid-on-boundary or extreme-aspect polygon) and are not true
  simplicity tests; the winding-total test is exact O(n) with no false
  negatives on convex input.

## Consequences

Positive:

- The measured quadratic blow-up on convex fills becomes linear:
  2048-gon ~7.05 ms → ~20 µs (~340×), 1024-gon ~1.06 ms → ~10 µs
  (~100×), 512-gon ~279 µs → ~5 µs (~54×). Non-convex paths keep their
  exact legacy cost (plus one O(n) pass, negligible next to O(n²)).
- Output determinism is preserved: convex paths render the same CCW
  triangle **set** (n−2 triangles covering the same area) as the legacy
  clipper, so the CPU oracle (`test_canvas_consistency`) and the pixel
  golden tests remain bit-stable even though the triangle *ordering*
  differs (ordering is unobservable to raster output). Verified by
  `test_canvas_tess` (exact triangle counts on 2048-vertex convex cases),
  `test_canvas_tess_cache` (stalling-star now correctly *avoids* the fast
  path), and `test_canvas_consistency` (GPU↔CPU oracle equivalence).
- No ABI, cache-key, or render-plan surface change; the fast path is an
  internal optimisation of `ear_clip_contour`.

Negative / follow-up:

- A domain with *mostly* non-convex animated fills (splash/brush strokes,
  organic silhouettes) sees no gain from this path; the O(n²) ear clip
  remains the worst-case floor for those. An O(n log n) triangulator
  (monotone decomposition / earcut-with-quadtree) is the follow-up,
  tracked separately and able to sit beneath the same fast-path
  predicate.
- The convexity predicate is O(n) with a `atan2f` per vertex using float
  accumulation; on very degenerate near-collinear inputs the winding sum
  may land near the 0.5 rad tolerance edge. This is a non-issue for
  real geometry but is the reason the tolerance is not tightened further
  (a false "non-convex" is always safe — it just falls through).

## References

- [ADR-0011](0011-hole-bridging-tessellator.md) — hole-bridging
  ear-clip tessellator.
- [ADR-0014](0014-stencil-then-cover.md) — the self-intersecting
  stencil fallback that must never be bypassed by a faulty convexity
  gate.
- Implementation: `libs/flux/src/canvas/geometry_tess.c`
  (`contour_is_strictly_convex_ccw`, `fan_clip_contour`,
  `ear_clip_contour`); regression coverage in
  `tests/flux/unit/test_canvas_tess.c`, `test_canvas_tess_cache.c`, and
  `tests/flux/integration/test_canvas_consistency.c`.