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
- Implementation: not started; tracked in
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
   spiral (later) with a bounded, provable error in position and tangent. The
   reduced pieces — never the raw control points — are the input to both
   offsetting and coverage.
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
   polyline offsetting produces. Join and cap topology is a discrete choice
   applied to the offset curve, not a patch over its artifacts.
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
- Curves gain a *value* representation (`flux_curve_cover`) that can be
  cached, serialised, diffed, and — critically — shared between the 2D canvas
  and the 3D scene ([ADR-0117](0117-perspective-correct-scene-vector-coverage.md))
  without either module seeing the other.
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
  cusps; the kernel must fall back to `double` internally where the analytic
  test is ill-conditioned (documented, not silent).
- Glyph and icon consumers that currently assume a flattened contour buffer
  must migrate to strips in the same change (ADR-0095's "move together").

## Verification

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
