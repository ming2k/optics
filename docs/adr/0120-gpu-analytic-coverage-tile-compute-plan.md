---
id: ADR-0120
title: "GPU analytic coverage — tile-compute plan for a modern 2D rasterizer"
status: accepted
date: 2026-10-25
superseded_by: null
scope: libs/flux, bindings, CI (tests/flux)
negative_knowledge: true
---

# ADR-0120: GPU analytic coverage — tile-compute plan for a modern 2D rasterizer

- Status: Accepted as a plan. **No implementation is claimed by this record.**
- Date: 2026-10-25
- Scope: `libs/flux/src/canvas/` (a device coverage backend), `tests/flux/`
  (the acceptance corpus), CI (a real-driver lane).
- Implements: stage S3 of [ADR-0119](0119-coverage-seam-migration-staged-plan.md).
- Depends on: [ADR-0115](0115-analytic-curve-kernel-single-geometry-authority.md)
  (the strip), [ADR-0118](0118-coverage-strategy-placement-distance-fields-and-curve-kernel.md)
  (the analytic-primitive branch, which stays), ADR-0119 S0–S2 (the harness, the
  CPU reference rasterizer and the active seam).

## Context

### What "modern" means here, stated as three testable conditions

1. Arbitrary paths — holes, self-intersection, either fill rule — rasterize with
   **continuous** coverage at **1× sample rate** on the device.
2. A fill needs **no MSAA attachment and no stencil attachment**.
3. Frame cost is dominated by covered pixels, not by CPU geometry processing.

### Where the engine stands, measured

| Fact | Value | Source |
|---|---|---|
| Device coverage on one oblique edge | **10 distinct levels** | `test_canvas_coverage_ceiling` |
| CPU coverage, same edge, solid fill | **117 levels** | same |
| CPU coverage, same edge, gradient fill | **88 levels** | same |
| INV-COV-02 requirement | > 16 levels | ADR-0116 |

The device path therefore **cannot** meet the coverage contract, and does so by
construction: its coverage is quantised by a sample count. That single number is
the gap this plan closes.

### The assets this plan spends rather than rebuilds

- **`flux_curve`** already emits **covers** (reduced segments with bounds, ray
  and winding primitives). A tile-compute rasterizer consumes exactly that;
  triangles are the wrong currency for it.
- **`tests/flux/coverage_harness.{h,c}`** is an *independent* exact-area
  reference (per-pixel clipping, area identity, per-row localisation) with 62
  assertions. It is what makes a device backend *judgeable* rather than merely
  plausible.
- **`coverage_raster.c`** is a CPU rasterizer that agrees with that reference to
  **≤ 5×10⁻⁷** on squares, circles, a half disc and the production flattener's
  own output (4186 assertions). It is the **reference implementation** the
  device backend is compared against, and the semantic definition of the
  contract.
- **The analytic-primitive branch** (ADR-0118) already gives rrect / circle /
  squircle exact 1× coverage on the device via a closed-form field, including
  under rotation.

### The one thing that cannot be verified on the development machine

Everything above is CPU-side and fully checkable here. A device coverage backend
is not: tile sizes, memory visibility, subgroup availability, atomics and barrier
correctness, and driver-specific precision are all **device behaviour**, and only
lavapipe is available locally. This plan therefore treats a real-driver CI lane as
a **prerequisite deliverable**, not as follow-up work.

## Decision

**Adopt a tile-compute coverage backend for the device, developed in phases whose
acceptance is the existing CPU reference and harness — and do not remove any
existing mechanism until a later phase strands it.**

### Phase A — Make the reference fast and safe (all verifiable locally)

1. Replace the rasterizer's per-band O(vertices) active-edge rescan with an
   **active edge table** (sort once by y, maintain a live list). Measured need:
   256 → 512 vertices is 9 ms → 46 ms, and a per-tile reference is called far
   more often than a per-fill one.
2. **Sanitizer and fuzz coverage** for the rasterizer: self-intersecting,
   near-degenerate, and high-vertex-count inputs under ASAN/valgrind. Not run at
   the time of writing.
3. A frame-level benchmark in `tests/bench` over a realistic UI scene, so cost is
   tracked per frame rather than per fill.

**Gate:** a ≥ 1024-vertex fill in single-digit milliseconds; ASAN clean; the
harness and the 4186 assertions unchanged and green.
**Kill criterion:** if the edge table cannot remove the quadratic term, the
per-tile reference is not viable at scale and B's design must change before any
shader is written.

### Phase B0 — A real-driver CI lane (prerequisite, not follow-up)

A runner with an actual Vulkan implementation plus a test that asserts the device
is *not* a software fallback, so a silently-degraded CI cannot pass a device
backend gate. Every B gate below runs on that lane; lavapipe remains useful for
breadth, not for acceptance.

**Kill criterion:** if no real driver can be brought into CI, **stop before B1**.
The rest of this plan would produce code nobody can prove correct, which is a
worse outcome than an admittedly CPU-only renderer.

### Phase B — Tile-compute coverage backend

| Step | Content | Gate |
|---|---|---|
| B1 | Upload covers as a flattened segment SSBO | segment count and endpoints agree with the CPU flattener's |
| B2 | Coarse raster: bin segments into `16×16` tiles | per-tile segment sets match a CPU computation of the same binning |
| B3 | Winding backdrop: two-level prefix sum (tile column sums, then in-tile scan) | per-tile winding matches `flux_curve_cover_winding` |
| B4 | Fine raster: per-pixel area integration **using the same formulation as `coverage_raster.c`** | S0 corpus on the device, within the coverage tolerance |
| B5 | Blend and clip integration through the ADR-0089 RenderPlan | `canvas_consistency` re-expressed with coverage tolerances |

**Gate for the phase:** the S0 corpus (simple fill, hole, self-intersecting star,
wide translucent stroke, glyph outline, 12-px icon) passes on the device; the
ceiling test's device expectation is moved above 16; `canvas_render`'s goldens
either pass or are consciously re-baselined with the reason recorded.
**Kill criterion:** if B4 cannot match the CPU reference within tolerance and the
divergence is not attributable to a specific implementable cause, revert the phase
whole. A device backend that is *approximately* right is worse than a slow
mandated one, because it silently changes what a pixel means.

### Phase C — Retire what B strands

ADR-0119's S4 gating table becomes executable row by row: the ear-clip
tessellator and its cache, the stencil fallback for even-odd, the MSAA attachment
and `flux_canvas_antialias`, the CPU supersample buffer where B covers those
paints, and the icon coverage bake. Each is deleted by the phase that makes it
unreachable, with no forwarders (ADR-0089).

Strokes follow: once fills are coverage-based, a stroke band is simply another
cover, which is the first point at which ADR-0115's
`flux_curve_seg_offset_retrograde` has a correct consumer.

### Invariants & Behavioral Boundaries

- **INV-GPU-01 — The reference precedes the shader.** A device backend is
  accepted only by agreement with the CPU coverage rasterizer *and* the S0 area
  reference. Agreement with itself, or with a stored image, is not acceptance.
- **INV-GPU-02 — The contract, not a quality dial.** A fill must need no MSAA and
  no stencil attachment, and must carry more than 16 distinct coverage levels for
  a high-contrast oblique edge on the device.
- **INV-GPU-03 — Closed form beats binning where it exists.** Analytic primitive
  classes keep the ADR-0118 field. Tile binning a rounded rectangle is slower and
  no more accurate than evaluating its distance; a backend must not "unify" them
  for uniformity's sake.
- **INV-GPU-04 — One meaning per draw.** Device and CPU must agree within the
  coverage tolerance for every draw the corpus contains. Divergence is a bug in a
  backend, never a tuning difference.
- **INV-GPU-05 — No device behaviour without a device lane.** Code whose
  correctness depends on a driver is not accepted on the strength of a software
  Vulkan implementation.

## Rejected Alternatives & Negative Knowledge

- **Option A — Adopt Vello (or another external 2D renderer) wholesale.** *Why
  considered:* it already implements this plan, well, and in production. *Why
  rejected:* it is a Rust engine with its own architecture, so it replaces the
  C23 contract surface, the RenderPlan authority (ADR-0089), the ABI, and the
  deterministic harness that makes every claim in this repository checkable. The
  plan above deliberately reuses the *technique* while keeping the contracts.
  The failure mode being avoided is not "slow" — it is losing the ability to say
  what a pixel means.
- **Option B — Per-primitive analytic fragments (Slug-class) for general
  fills.** *Why considered:* it is the cheapest route to perceptually perfect
  curves, and it is what already serves analytic primitives here. *Why rejected
  for general paths:* it presumes a primitive per curve, cannot express holes,
  self-intersection or arbitrary winding, and needs per-primitive control-point
  storage and a per-primitive draw. Those are precisely the cases the current
  canvas answers with a stencil pass; adopting it would leave that limitation in
  place. It remains correct — and is retained — for the classes where a closed
  form exists (INV-GPU-03).
- **Option C — Rasterize coverage on the CPU and upload it as a texture.** *Why
  considered:* it reuses `coverage_raster.c`, is trivially "correct", and would
  make the ceiling number good immediately. *Why rejected:* it is a bake. Its
  quality is bounded by an upload resolution, it pays a per-draw CPU raster plus
  a PCIe transfer, and it is exactly the mechanism ADR-0112 was superseded for
  and ADR-0117 rejects for scene vector content. It would also make the coverage
  contract unfalsifiable in practice: the number would improve while nothing
  about the renderer did.
- **Option D — Raise MSAA instead (8×, 16×).** *Why considered:* one enum value,
  no architecture. *Why rejected:* measured — 4× yields 10 distinct values on the
  probe edge; the ceiling moves roughly linearly with sample count, so 16× buys
  ~34 levels at 16× the attachment bandwidth and still cannot reach a continuous
  ramp. Wrong axis, and it costs every draw rather than the shapes that need it.
- **Option E — Build the device backend first, derive a reference afterwards.**
  *Why considered:* the visible complaint is on screen. *Why rejected:* a backend
  without a reference cannot be judged; this repository already paid for that
  lesson twice (ADR-0119 S1's two reverted attempts, whose defects were only
  visible because the harness existed first).
- **Option F — Start Phase B with no real-driver lane.** *Why considered:* it
  unblocks work immediately. *Why rejected:* every failure mode in B is
  device-specific. Writing it against a software implementation produces code
  whose correctness claim is unbacked — the precise pattern this plan exists to
  avoid.
- **Option G — Delete the tessellator now that fills are coverage-based on the
  CPU.** *Why considered:* it looks like dead weight after S2. *Why rejected:*
  measured — the device has no coverage backend, and the CPU still needs it for
  images, glyph runs, strokes and non-SRC_OVER blends; `test_canvas_tess_cache`
  exercises it through such a draw and passes. Deleting it today breaks the only
  presentable path.

## Consequences

Positive:

- The gap between the device and the contract is a single measured number
  (10 vs 117), asserted in the suite, so progress is observable and regression is
  loud.
- The acceptance machinery — exact-area harness, CPU reference rasterizer, corpus
  and ceiling test — already exists; B spends its effort on shaders rather than on
  inventing tests.
- Analytic primitives keep their cheaper closed-form path, so the tile backend is
  only responsible for what genuinely needs binning.

Negative & trade-offs:

- Phase B0 is a real cost and a hard prerequisite: a GPU runner, driver pinning,
  and a fallback-detection test. Without it the plan stops at A by design.
- Tile-compute adds transient GPU resources (segment buffers, tile lists,
  winding scratch) that must obey ADR-0090/0092 ownership and budget rules, and
  it changes a frame's shape from a draw sequence into dependent passes, which is
  an integration into the RenderPlan rather than a pipeline swap.
- Complex paths are slow on the software backend today (86 ms for a 1024-vertex
  fill) and Phase A only removes the quadratic term; the software backend is not
  a performance target, and this plan does not pretend otherwise.
- Three backends will coexist for a period (tile-compute, analytic-fragment,
  triangle), so the seam must admit all three until Phase C retires the third.

## Verification

Per phase, with the gates stated above. The corpus and the two references are the
standing instruments:

- `tests/flux/coverage_harness.{h,c}` — independent exact-area reference; area
  identity, per-row localisation; refuses self-intersecting input rather than
  reporting a cancelling zero.
- `tests/flux/unit/test_coverage_raster.c` — the CPU reference implementation,
  4186 assertions, exact to 5×10⁻⁷.
- `tests/flux/integration/test_canvas_coverage_ceiling.c` — the ceiling, and the
  assertion whose inversion defines B's completion.
- `tests/flux/integration/test_canvas_consistency.c` — the device/CPU agreement
  gate, to be re-expressed with coverage tolerances in B5.

No phase of this plan is implemented. The device path still renders 4× MSAA
triangles, and [the implementation matrix](../dev/architecture-implementation-status.md)
records that plainly.

## References

- [ADR-0115](0115-analytic-curve-kernel-single-geometry-authority.md) — the cover
  this backend consumes.
- [ADR-0116](0116-coverage-contract-canvas-rasterization.md) — the coverage
  contract and INV-COV-02.
- [ADR-0118](0118-coverage-strategy-placement-distance-fields-and-curve-kernel.md)
  — the analytic-primitive branch retained by INV-GPU-03.
- [ADR-0119](0119-coverage-seam-migration-staged-plan.md) — S0–S2 landed, the
  staging discipline, and the S4 gating table.
- [ADR-0112](0112-device-resolution-icon-coverage-masks.md) — the bake that
  Option C would reintroduce.
- [ADR-0089](0089-rendering-program-and-execution-plan.md) — the execution owner
  the fine pass must integrate with; no forwarders.
- Prior art: Vello / Sparse Strips (tile binning, prefix-sum winding, analytic
  area coverage); Slug (per-primitive analytic coverage, retained for the classes
  where a closed form exists); Skia Graphite.
