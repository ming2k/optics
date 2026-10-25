---
id: ADR-0115
title: "Analytic curve kernel — one geometry authority for Bézier fills and strokes"
status: accepted
date: 2026-10-25
superseded_by: null
scope: libs/flux, bindings
negative_knowledge: true
---

# ADR-0115: Analytic curve kernel — one geometry authority for Bézier fills and strokes

- Status: Accepted
- Date: 2026-10-25
- Scope: new public value surface `libs/flux/include/flux/curve.h` and
  implementation `libs/flux/src/curve/`; consumers `libs/flux/src/canvas/`,
  `libs/flux/src/scene/`, `libs/glyph`, `libs/lens`.
- Supersedes the geometry authority of [ADR-0005](0005-tessellator-scope.md),
  [ADR-0011](0011-hole-bridging-tessellator.md),
  [ADR-0014](0014-stencil-then-cover.md),
  [ADR-0113](0113-convex-fast-path-tessellation.md) and
  [ADR-0114](0114-ear-clip-candidate-list.md).
- Depends on: [ADR-0016](0016-pure-rhi-and-draw-primitives.md) (module
  boundaries), [ADR-0083](0083-flux-orthogonal-shape-paint-model-and-c23-baseline.md)
  (geometry/brush model), [ADR-0089](0089-rendering-program-and-execution-plan.md)
  (single semantic authority).
- Enables: [ADR-0116](0116-coverage-contract-canvas-rasterization.md),
  [ADR-0117](0117-perspective-correct-scene-vector-coverage.md).
- Implementation: **kernel landed** (`libs/flux/src/curve/`, `include/flux/curve.h`,
  tested by `tests/flux/unit/test_curve.c`), and the flattening consumer is
  migrated: the recursive `geometry_flatten.c` is deleted and replaced by
  `libs/flux/src/canvas/curve_flatten.c`, which samples each curve at the
  kernel's Wang segment count (one stage, no recursion). The analytic offset
  and its closed-form retrograde interval also landed
  (`flux_curve_seg_offset`, `flux_curve_seg_offset_retrograde`), and the
  **stroker now clamps the inner-side offset to the local radius of curvature**
  (`clamp_inner_offset` in `geometry_stroke.c`) — the discrete form of the same
  rule — which removes the band self-overlap that double-blended translucent
  strokes. The tessellation and stroking consumers are **not** migrated:
  `geometry_stroke.c` still offsets a *flattened polyline*, so its curvature is
  a per-vertex estimate rather than the kernel's closed form, and
  `flux_curve_seg_offset_retrograde` currently has no production consumer —
  converting the stroker to offset the reduced segments is the next step. The
  remaining work is tracked in
  [the implementation matrix](../dev/architecture-implementation-status.md).

## Context

Curve geometry in `flux` is currently the *authority* for fill and stroke
semantics, and that authority is a CPU triangle machine:

- `libs/flux/src/canvas/geometry_flatten.c` — recursive De Casteljau midpoint
  subdivision with a chord-to-control-point flatness test, tolerance mapped
  from screen space through the operator norm of the active transform's
  linear part, capped at recursion depth 16.
- `libs/flux/src/canvas/geometry_tess.c` — ear clipping, hole bridging
  (ADR-0011), a convex fan fast path gated by a winding-total convexity test
  (ADR-0113), an incremental reflex candidate list (ADR-0114), an LRU
  tessellation cache, and a routing decision into a stencil-then-cover
  fallback (ADR-0014) when the clip stalls on non-simple input.
- `libs/flux/src/canvas/geometry_stroke.c` — polyline stroke expansion by
  per-vertex bisector offsetting with miter/bevel/round joins.

The contract this produces is *triangles*. Every consumer of curve geometry
must therefore reproduce this machine: the GPU triangle pipeline
(`backend_vk.c`), the bit-exact CPU oracle (`backend_cpu.c`, ADR-0019), and any
future backend. `libs/glyph` outline consumers and `libs/lens` icon/SVG
runtimes are downstream of the same representation.

Four liabilities are structural, not incidental:

1. **Correctness is bounded by "simple polygon".** Self-intersecting input is
   out of contract (ADR-0114 records a ~1 % stall-decision divergence on
   scrambled input); the fallback is a stencil pass, not a correct
   triangulation.
2. **Cost is Θ(n²) in the worst case** (amortised away by a cache that is
   invalidated whenever `pixel_scale` changes, i.e. on every zoom/scale
   animation).
3. **Stroke fidelity is bounded by polyline offsetting.** A true offset of a
   curved segment is not the offset of its flattening; wide strokes on high
   curvature and at cusps necessarily produce inner self-intersection
   (retrograde overlap), visible as dark seams under translucency.
4. **Every new rasterizer inherits the whole machine.** A backend that wants
   correct curves must either re-run ear clipping bit-for-bit or invent a
   second geometry semantics — the "two sources of drawing semantics" that
   [ADR-0089](0089-rendering-program-and-execution-plan.md) explicitly
   forbids.

Industry convergence is unambiguous: Vello/Sparse Strips, Skia Graphite,
Loop-Blinn, and Slug all place curve evaluation and coverage in the rasterizer
and share a *small, backend-agnostic curve algebra* beneath it. `flux` already
proved the direction in miniature — `canvas_sdf.frag` is analytic coverage for
rounded rectangles and circles, and it exists precisely because triangles plus
a sample count cannot express curve coverage. ADR-0112 then had to bolt a
CPU coverage bake onto icons to work around the same ceiling.

## Decision

Introduce **`flux_curve`**: a backend-agnostic curve algebra kernel that becomes
the **single authority for curve geometry semantics**. It is a leaf module
(depending only on `math`, exactly like `math`), public as a value surface, and
it emits **coverage strips**, not triangles.

1. **Reduction.** A cubic segment is split at its inflections and curvature
   extrema and each monotone piece is reduced to a quadratic (v1) or Euler
   spiral (later). The error bound is **exact, not empirical**: for a cubic
   with third difference `E = P3 − 3·P2 + 3·P1 − P0`, the quadratic through the
   endpoints with control point `(3·P1 − P0 + 3·P2 − P3)/4` differs from the
   cubic by exactly `(E/6)·3t(1−t)(1−2t)`, whose magnitude is at most
   `|E| / (6·2√3) ≈ 0.048113·|E|`. A piece is accepted only when that is within
   the caller's ε; otherwise it is halved and retried. The reduced pieces —
   never the raw control points — are the input to both offsetting and
   coverage.
2. **Exact splitting.** Tangents, inflections, cusps (retrograde points) and
   curvature extrema are located analytically in the segment parameter. A
   cusp-free, monotone decomposition is a *precondition* for correct
   offsetting and for analytic coverage; it is computed once, in the kernel.
3. **Segment count by Wang's formula.** For a chord tolerance ε the minimal
   uniform segment count N is closed-form
   (`N = ceil(sqrt(L · 3 / (4·ε)))` over the second-difference bound `L`).
   There is no recursion and no tolerance search: N is a function of the
   segment alone.
4. **Analytic parallel offset.** Stroking offsets the *reduced curve* (cubic
   pieces are their own offsets, per Levien's parallel-curve construction),
   resolves offset self-intersection analytically, and removes the loops that
   polyline offsetting produces. **The self-intersection test is closed-form**:
   for a quadratic the curvature numerator `cross(B', B'')` is constant, so
   "the offset exceeds the local radius of curvature" reduces to a quadratic in
   the segment parameter, and the retrograde interval falls out of it
   (`flux_curve_seg_offset_retrograde`). A line never retrogrades. Join and cap
   topology is a discrete choice applied to the offset curve, not a patch over
   its artifacts.
5. **Coverage primitives.** Conservative bounds, ray/quadratic intersection,
   and signed winding — the minimum a rasterizer needs to compute per-pixel
   coverage (or per-tile winding) without re-deriving geometry.
6. **Output value.** A compact, immutable, serialisable `flux_curve_cover`
   ("curve strip"): reduced segments, per-segment clip spans, and bounds. It
   carries no backend, device, or raster type, and it is the *only* geometry
   currency that crosses the seam into a rasterizer.

### Invariants & Behavioral Boundaries

- **INV-CRV-01 — Single authority.** Curve geometry semantics live only in
  `flux_curve`. No rasterizer, backend, or consumer may reinterpret control
  points, tolerance, or winding.
- **INV-CRV-02 — Leaf module.** `flux_curve` depends only on `math`; it never
  includes `canvas`, `scene`, `compute`, or any backend header (ADR-0016).
- **INV-CRV-03 — No hot-path allocation.** Encoding uses caller arena/scratch;
  a fill or stroke never triggers `malloc`/`free` on the recording hot path.
- **INV-CRV-04 — Total and fail-closed.** Degenerate or unsupported input
  returns an explicit error and produces no geometry. Silent wrong shapes,
  partial strips, and "best-effort" output are prohibited (ADR-0095).
- **INV-CRV-05 — Determinism.** For identical inputs the kernel produces
  bit-identical strips on every platform; it is the shared reference for the
  GPU/CPU coverage agreement test.
- **INV-CRV-06 — Error bound is contractual.** The reduction's deviation from
  the true curve is bounded by the caller's ε and is asserted in tests; the
  produced offset is within ε of the true offset.

## Rejected Alternatives & Negative Knowledge

Each entry records a live option that was evaluated and **why it fails**, so
the next reader does not re-derive the trade-off.

- **Option A — Keep ear clipping; add a GPU compute tessellator.**
  *Why considered:* smallest semantic change; moves cost off the CPU.
  *Why rejected:* it preserves triangles and therefore the whole correctness
  ceiling — simple-polygon-only fills, stencil fallback for self-intersection,
  polyline stroke artifacts — and still forces every backend to reproduce
  ear-clip output exactly. It relocates the cost without removing the
  liability, and it adds a second geometry implementation rather than
  removing one.
- **Option B — Vendor `libtess2` (or any general tessellator).**
  *Why considered:* a proven, complete triangulator; kills the self-
  intersection gap immediately. *Why rejected:* it triangulates a *polygon*
  that we would still have to flatten and stroke first, so it pays for a
  dependency without touching curves, stroking, or anti-aliasing; and it
  imports a foreign semantics into a C23-minimal codebase — the same reasoning
  that rejected it in ADR-0005, now with the added failure mode that it makes
  the *later* coverage work harder, not easier.
- **Option C — Keep recursive De Casteljau, tighten the tolerance.**
  *Why considered:* one-line quality/cost dial, zero API change.
  *Why rejected:* tolerance is a quality dial, not a correctness fix. Recursion
  has unbounded branch/stack behaviour on pathological input (the depth cap is
  a guard, not a bound), cannot be evaluated in parallel, and produces a
  non-deterministic vertex ordering that a per-tile/per-fragment coverage
  rasterizer cannot consume.
- **Option D — Polyline offsetting with richer join geometry.**
  *Why considered:* incremental improvement to the existing stroker.
  *Why rejected:* inner self-intersection under curvature and cusps is
  *intrinsic* to offsetting a polyline. Adding bevel/round/miter variants and
  caps changes how the artifact is filled, not whether it exists. Translucent
  wide strokes remain visibly wrong.
- **Option E — One uniform "N segments per curve" sampler.**
  *Why considered:* trivial and branch-free. *Why rejected:* over-tessellates
  low-curvature geometry (N is fixed for a nearly straight line) and
  under-tessellates high-curvature geometry, so it is simultaneously more
  expensive and lower quality than a curvature-derived N; it also makes the
  error bound a function of the caller's scene, not of ε.
- **Option F — Euler spirals as the v1 primitive.**
  *Why considered:* spirals are the strictly better mathematical primitive
  (arc length, curvature and offset all behave; Levien's parallel-curve
  construction is spiral-native) and are the strongest long-term target.
  *Why rejected for v1 only:* a full spiral pipeline (arc-length inversion,
  curvature-continuous joins, spiral↔quadratic conversion) is a larger surface
  than a quadratic-reduction kernel, and the strip interface is
  representation-agnostic. **Recorded as an explicit migration path, not a
  hedge:** the kernel's output type must admit a spiral backend without an ABI
  break. (This is the one alternative that is *deferred on cost*, not rejected
  on merit; it is the first thing to revisit after ADR-0116 lands.)
- **Option G — Keep the tessellation cache and layer the kernel on top.**
  *Why considered:* protects existing frame-time wins for static UI.
  *Why rejected:* the cache keyed on `pixel_scale` is exactly the coupling that
  makes zoom animations pathological. The kernel's strip is cheap enough to
  re-derive per frame (O(paths), not O(vertices)); retaining a second,
  divergent cache re-creates the drift failure mode of ADR-0112. **If a cache
  is later needed it must key on the strip, not on a triangle batch.**
- **Option H — Clamp the stroker's inner offset with the kernel's exact
  source-curve curvature.** *Why considered:* the stroker infers curvature from
  the flattened polyline (arc length over turn angle), which under-estimates R
  by `sin(x)/x`; replacing a guess with the kernel's exact radius is exactly the
  single-authority principle this ADR establishes. *Why rejected:* **measured,
  and it is worse.** The band being emitted is a *polyline* band, so the clamp
  limit must be where that band's own inner edges converge; substituting the
  analytic value makes the collapse miss, because a cubic approximating a small
  circle has slightly larger curvature radius than the circle. The `r = 2`
  retrograde case regressed from 0 to 28 coverage units of double blending.
  Taking the minimum of both limits reproduces the polyline result exactly
  (measured 0 / 10 / 0 / 15 / 0 across the oracle's five cases), so the analytic
  radius buys nothing here. **Consequence:** `flux_curve_*_curvature_radius` was
  implemented, measured, and *removed* rather than left in the tree as an
  unused primitive, and the stroker keeps its discrete estimate with the reason
  recorded at `clamp_inner_offset`.
- **Option I — Re-express the stroker on reduced segments (retire
  `geometry_stroke.c`).** *Why considered:* it is the last consumer of the
  polyline model, and the kernel already exposes `flux_curve_seg_offset` and
  `flux_curve_seg_offset_retrograde`, which would then have a production
  consumer. *Why rejected — for now, on measured grounds:* both improvements the
  rewrite was meant to deliver have been measured and neither helps. Exact
  normals change nothing because the offset placement is already within the
  flattening tolerance; exact curvature is actively worse (Option H). What
  remains is the segment-body and inner-join topology, whose only measured
  benefit is deleting a file. That is not a justification for a rewrite of
  corner, cap and join geometry that the icon pixel tests would have to defend
  without an oracle that can tell "different" from "wrong" for a stroked curve.
  **The stroker is retained by decision, not deferred:** it is the correct
  stroker for a polyline rasterizer. Revisit only when the coverage backends of
  ADR-0116 make the fill path stop emitting polylines — at that point the
  stroker's premise changes, and the retrograde interval becomes reachable.

## Consequences

Positive:

- Self-intersection, holes, and arbitrary winding stop being special cases:
  they become winding *counts* over strips, which is what [ADR-0116](0116-coverage-contract-canvas-rasterization.md)
  rasterizes. ADR-0014's stencil fallback and ADR-0011's bridge search are
  retired rather than maintained.
- Stroke fidelity becomes curve-accurate: no polyline offset artifacts, no
  retrograde overlap, no miter-limit fudge that trades a spike for a bevel.
- Hot-path geometry cost becomes O(paths + segments) with a closed-form
  segment count, and is parallel-friendly by construction.
- Coverage can be derived from a strip two independent ways — winding over the
  reduced segments, and the closed-form field for the analytic classes — and the
  two agree on the same shape (`test_curve`'s cross-route check). That agreement
  is part of the evidence ADR-0116's coverage backends rest on.
- Curves gain a *value* representation (`flux_curve_cover`) that can be
  retained by the caller, diffed, and — critically — shared between the 2D
  canvas and the 3D scene ([ADR-0117](0117-perspective-correct-scene-vector-coverage.md))
  without either module seeing the other. (Not a serialization format; see
  Decision point 6 and ADR-0090.)
- A better primitive can be dropped in later (Option F) without touching any
  consumer, because the consumer contract is the strip, not the representation.

Negative & trade-offs:

- This is a source and behavioural break for curve rendering. `geometry_flatten.c`,
  `geometry_tess.c`, `geometry_stroke.c`, their scratch buffers, the
  tessellation cache, and the `FLUX_CANVAS_PATH_SCRATCH_CAP` budget model are
  replaced, not deprecated. ADR-0089's no-forwarder rule applies: no shim.
- The kernel is new, subtle code (cusp detection, offset self-intersection). It
  is only acceptable with the differential and area-reference verification
  below; the alternative (an unverified kernel) is worse than the legacy
  machine it replaces.
- The exact-error reduction is float-precision-sensitive near tangencies and
  cusps; the kernel evaluates its analytic tests in `double` internally (the
  public values stay float), and `solve_quadratic` forms the larger-magnitude
  root first so a root near zero is not lost to cancellation.
- Glyph and icon consumers that currently assume a flattened contour buffer
  must migrate to strips in the same change (ADR-0095's "move together").

## Verification

Landed in `tests/flux/unit/test_curve.c` (181 assertions): reduction error
bound vs a dense sampling of the source cubic, inflection and cusp location,
Wang monotonicity and the straight case, analytic line offset and
quadratic-midpoint offset, ray crossings, winding under both fill rules
(including a double-wound ring where NONZERO and EVENODD must differ),
fails-closed on every bad input, and byte-for-byte determinism. The
**retrograde interval is verified against its defining condition**, not against
its own formula: the closed form is checked by sampling `T(t)·O'(t)` across the
segment and asserting the sign flips exactly at the reported interval, with
positive controls (offset within the radius of curvature, offset the other way,
a line) returning no interval.

Landed with the flattening migration: `tests/flux/unit/test_canvas_flatten.c`
(screen-space tolerance still drives subdivision; object radius 1 at scale 100
and radius 100 at scale 1 flatten identically; a degenerate cubic emits
nothing and does not starve later contours) plus the unchanged
`canvas_consistency` GPU↔CPU pixel oracle and `canvas_render` golden tests,
which pass without modification — evidence that the replacement is
output-equivalent for the recorded cases.

Landed with the inner-offset clamp: `tests/flux/unit/test_canvas_stroke_region.c`,
an analytic stroke-coverage oracle. It renders a *translucent* stroke and a
solid fill of the same paint as a single-coverage reference (self-calibrating,
so it assumes nothing about the linear→sRGB readback), then asserts every pixel
inside the stroke's own band carries exactly one coverage. Measured before the
clamp: 204 double-blended pixels on an r=6 stroke of width 20. After: 4, each
about one supersample. Every non-retrograde case measures 0.

The same oracle produced the negative result recorded under Option H: replacing
the stroker's discrete curvature estimate with the kernel's exact radius
regressed the r=2 retrograde case from 0 to 28, and taking the minimum of both
limits reproduced the baseline exactly. The five cases currently measure
0 / 10 / 0 / 15 / 0 against a bound of 20, which is what makes the bound a
measured headroom rather than a number chosen to pass.

Cross-route coverage evidence: `flux_curve_cover_coverage` (winding over curve
strips, supersampled and named so) is compared against the closed-form distance
field on the same square. The two routes share no code, so agreement is
independent evidence that both are right: worst difference 0.08 at 16×16
samples and 0.04 at 32×32, monotone under refinement, and a curved (quadratic)
strip yields more than eight distinct coverage levels — far beyond the five a 4×
sample count can express, which is the property ADR-0116's backends must have.

That comparison also pinned a limitation of the analytic branch worth recording:
`coverage = 0.5 − d` is exact where a single edge governs, but where two
one-pixel ramps overlap near a corner the field's min/max combination is *not*
the true area coverage (measured 0.25 against a true 0.125 on a unit square).
This is why ADR-0116's oracle compares against area rather than against the
field, and why the cross-route test restricts its probes to single-edge pixels.

The remaining checks below describe the migration target, not landed coverage.

- **Reduction error bound.** Randomised cubic corpus: max position/tangent
  deviation of the reduced pieces vs the analytic curve is asserted ≤ ε over
  10⁶ segments, including near-inflection, near-cusp, and near-collinear
  degenerates.
- **Offset correctness.** For each stroked contour, the produced offset is
  compared against a 4096-sample-per-curve parallel curve; signed distance
  error ≤ ε, and zero retrograde self-intersection inside the stroke width.
- **Fails-closed suite.** Degenerate inputs (zero-length, coincident control
  points, NaN/Inf, N > budget) return an explicit error and emit nothing.
- **Determinism.** Identical strips across architectures (x86-64, AArch64),
  optimisation levels, and the CPU oracle path.
- **No-allocation gate.** The existing leak/fault-injection harness asserts
  zero allocator calls on the encode path.
- **Consumer equivalence.** `tests/flux/unit/` geometry suites re-expressed
  against strips; `tests/flux/integration/test_canvas_consistency.c` continues
  to bound GPU↔CPU agreement, now over coverage rather than triangles
  (ADR-0116).
- **Benchmarks.** `tests/flux/bench/bench_flatten.c`, `bench_tess.c`, and
  `bench_path.c` are replaced by a strip-encode benchmark over the same corpus,
  so the comparison to the legacy numbers is explicit rather than lost.

## References

- [ADR-0005](0005-tessellator-scope.md), [ADR-0011](0011-hole-bridging-tessellator.md),
  [ADR-0014](0014-stencil-then-cover.md), [ADR-0113](0113-convex-fast-path-tessellation.md),
  [ADR-0114](0114-ear-clip-candidate-list.md) — the geometry authority this
  ADR replaces.
- [ADR-0016](0016-pure-rhi-and-draw-primitives.md) — module boundaries the leaf
  rule preserves.
- [ADR-0083](0083-flux-orthogonal-shape-paint-model-and-c23-baseline.md) —
  geometry/brush orthogonality; `flux_curve_cover` is the path geometry value.
- [ADR-0089](0089-rendering-program-and-execution-plan.md) — single semantic
  authority; no dual planners or forwarders.
- [ADR-0095](0095-flux-public-contract-convergence.md) — explicit semantics,
  sticky errors, move-together migration.
- [ADR-0116](0116-coverage-contract-canvas-rasterization.md),
  [ADR-0117](0117-perspective-correct-scene-vector-coverage.md) — the two
  rasterization consumers of the strip.
- Prior art: Loop & Blinn (2005) implicit curve rendering; Levien, *Parallel
  curves of cubic Béziers* (2024); Vello/Sparse Strips; Skia Graphite; Slug.
- Implementation: `libs/flux/src/curve/` (`reduce`, `split`, `wang_count`,
  `offset`, `cover`, `ray_quadratic`); public surface
  `libs/flux/include/flux/curve.h`.
nn (2005) implicit curve rendering; Levien, *Parallel
  curves of cubic Béziers* (2024); Vello/Sparse Strips; Skia Graphite; Slug.
- Implementation: `libs/flux/src/curve/` (`reduce`, `split`, `wang_count`,
  `offset`, `cover`, `ray_quadratic`); public surface
  `libs/flux/include/flux/curve.h`.
