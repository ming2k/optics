---
id: ADR-0116
title: "Coverage-contract canvas rasterization — analytic coverage backends, MSAA retired"
status: accepted
date: 2026-10-25
superseded_by: null
scope: libs/flux, libs/lens, bindings
negative_knowledge: true
---

# ADR-0116: Coverage-contract canvas rasterization — analytic coverage backends, MSAA retired

- Status: Accepted
- Date: 2026-10-25
- Scope: `libs/flux/src/canvas/` (front end, `backend.h`, `backend_vk.c`,
  `backend_cpu.c`, `renderer.c`, the fill/stroke entry points),
  `libs/flux/include/flux/canvas.h` (anti-aliasing policy), and
  `libs/lens/src/render/icon_mask.{h,c}` (retired).
- Supersedes [ADR-0071](0071-pass-scoped-canvas-antialiasing.md) (pass-scoped
  MSAA policy) and [ADR-0112](0112-device-resolution-icon-coverage-masks.md)
  (device-resolution icon coverage masks and software supersampling).
- Amends [ADR-0009](0009-canvas-msaa-pipeline-key.md)'s lineage (sample count
  stops being a pipeline-key axis), and refines
  [ADR-0019](0019-canvas-backend-seam-and-cpu-backend.md) (the backend seam is
  retained; what crosses it changes), plus
  [ADR-0090](0090-display-list-resource-ownership.md) /
  [ADR-0092](0092-resource-planning-and-retirement.md) (target-specific
  resources the new backends require).
- Depends on [ADR-0115](0115-analytic-curve-kernel-single-geometry-authority.md)
  (the strip), [ADR-0118](0118-coverage-strategy-placement-distance-fields-and-curve-kernel.md)
  (the peer analytic branch this contract also serves) and
  [ADR-0089](0089-rendering-program-and-execution-plan.md)
  (the execution owner).
- Implementation: not started. Executed by the staged plan in
  [ADR-0119](0119-coverage-seam-migration-staged-plan.md), which sequences the
  work behind per-stage gates and makes the acceptance harness its first stage.
  Progress is tracked in
  [the implementation matrix](../dev/architecture-implementation-status.md).

## Context

The canvas rasterizer accepts **triangles** and produces anti-aliasing from a
**sample count**:

- Every fill and stroke is flattened, tessellated, and stroked on the CPU
  (ADR-0115) into `flux_canvas_vertex` triangles, submitted per draw.
- Anti-aliasing is pass-scoped 4× MSAA (ADR-0071), fixed at four samples;
  the CPU backend mirrors it with 4× supersampling (ADR-0019).
- Rounded rectangles and circles alone escape this via an analytic SDF
  fragment shader (`canvas_sdf.frag`); self-intersecting fills escape it via a
  canvas-owned stencil attachment (ADR-0014).

Three ceilings follow from a sample count being the AA authority:

1. **Five coverage levels.** 4× MSAA quantises edge coverage to
   {0, ¼, ½, ¾, 1}. Human sensitivity to a high-contrast oblique edge needs
   far more; ADR-0112 documents that icons at 14–24 px were "visibly
   stair-stepped, badly enough that a downstream consumer reorganised its own
   artwork to compensate".
2. **A workaround for the workaround.** ADR-0112's answer was a *bake*: render
   icons at 2× supersampling in software into an R8 coverage mask (65 levels)
   and blit it. That is a general anti-aliasing principle trapped in one
   consumer, keyed to icon identity and stroke width, invalidated on resize,
   and it cannot run at all as a GPU pass.
3. **Bandwidth and a stuck pipeline key.** Every canvas pass carries a 4×
   attachment and a resolve; ADR-0071 needs a per-pass policy to switch it off,
   and ADR-0009 needed the sample count to discriminate pipelines. The
   attachment exists to serve edges that a coverage model does not need to
   sample.

Meanwhile the representation that reaches the rasterizer (triangles) is the
geometry authority (ADR-0115), so the rasterizer cannot become better than the
tessellator, and a second rasterization strategy cannot exist without a second
source of drawing semantics — which ADR-0089 forbids.

Every modern 2D rasterizer that is not sample-count-based has converged on the
same shape: **coverage is computed analytically from geometry, at 1× sample
rate.** Two families exist in production, and they are *rasterization
strategies of one contract*, not two semantics:

- **Tile compute** (Vello / Sparse Strips class): bin path segments into screen
  tiles, compute each tile's backdrop by a prefix sum over winding crossings,
  and integrate exact per-pixel area coverage inside the tile. Yields
  resolution-independent coverage with no framebuffer multisampling.
- **Analytic fragment** (Slug / Loop-Blinn class): reconstruct each primitive's
  curve coverage inside the fragment shader by intersecting the pixel with the
  curve (quadratic root-finding), with the coverage width derived from the
  screen-space Jacobian.

## Decision

**The canvas rasterizes coverage strips (ADR-0115), and anti-aliasing is
analytic coverage at 1× sample rate.** The backend seam (ADR-0019) is retained;
what crosses it is redefined. Concretely:

1. **The seam currency is the strip, not the triangle.** The canvas front end
   encodes fills and strokes as `flux_curve_cover` values plus a brush and
   submits those; it never hands a rasterizer control points to reinterpret
   (INV-CRV-01). Triangle submission as a *public* geometry path is removed.
   The **glyph-run primitive survives unchanged** (ADR-0010): its coverage
   atlas is simply precomputed coverage for that run, so an on-canvas glyph run
   remains valid — only its anti-aliasing axis stops being the canvas's sample
   count. In-scene glyphs take the ADR-0117 path instead.

2. **One coverage contract, three backend classes.** Every backend maps
   strips + brush → premultiplied linear-light framebuffer samples. A backend
   is a *rasterization strategy* selected by device capability and pass policy
   (the flux counterpart of [ADR-0044](0044-iris-backend-selection.md)'s
   host-side selection), never a different meaning for the same draw:

   | Class | Coverage computed | Framebuffer | Role |
   |---|---|---|---|
   | `TILE_COMPUTE` | per tile: prefix-sum winding + exact per-pixel area | 1× | default GPU canvas rasterizer |
   | `ANALYTIC_FRAGMENT` | per fragment: pixel↔quadratic coverage | 1× | per-primitive coverage; small icon/text-heavy surfaces; the strategy the 3D scene reuses (ADR-0117) |
   | `CPU` | deterministic software evaluation of the same contract | 1× host | headless, and the portability oracle |

   `TILE_COMPUTE` and `ANALYTIC_FRAGMENT` differ in *where* coverage is
   computed, not in what a pixel's coverage means. A target that supports
   neither returns an explicit error; there is no silent triangle fallback.

3. **MSAA is retired as the anti-aliasing authority.** The
   `flux_canvas_antialias` enum and its `AUTO`/`NONE`/`MSAA_4X` values are
   replaced by a coverage policy (sampling resolution is no longer the axis;
   coverage accuracy is). The canvas no longer owns a multisample attachment;
   `CANVAS_SAMPLE_SINGLE`/`CANVAS_SAMPLE_MSAA` pipeline variants collapse.

4. **The stencil fill path is retired.** Self-intersection and holes are
   winding counts over strips; there is nothing to stall, so ADR-0014's
   stencil write + cover pass and the canvas-owned stencil attachment are
   removed.

5. **The CPU backend becomes a coverage oracle, not a shader port.** It
   evaluates the coverage contract deterministically. The portability proof
   (`test_canvas_consistency`) becomes an agreement test over *coverage* with
   documented tolerances, instead of a bit-exact reimplementation of the GPU's
   triangle pipeline, blend state, dither, and MSAA resolve. The existing
   host-coverage glyph path (ADR-0019/ADR-0112) is subsumed: glyph coverage is
   simply coverage.

6. **The icon mask cache is deleted.** Icons render through the same coverage
   path as every other curve; there is no per-icon bake, no mask pool, no
   stroke-width key, and no consumer-specific resolution workaround. What is
   deleted is the *implicit* fallback: the technique itself is retained as an
   explicit opt-in by [ADR-0118](0118-coverage-strategy-placement-distance-fields-and-curve-kernel.md).

7. **The analytic distance-field branch is a peer strategy, not a casualty.**
   Geometry whose class has a closed-form boundary distance (box, rounded rect,
   circle) keeps its exact analytic path — see
   [ADR-0118](0118-coverage-strategy-placement-distance-fields-and-curve-kernel.md).
   "The rasterizer derives no geometry" (INV-COV-01) constrains backends that
   consume strips; it does not turn an exact analytic field into a strip.

### Invariants & Behavioral Boundaries

- **INV-COV-01 — No geometry in the rasterizer.** A backend consumes strips; it
  must not derive, re-derive, or approximate geometry itself.
- **INV-COV-02 — Resolution independence.** Coverage must be *continuous*:
  computed from geometry, not quantised by a sample count. A high-contrast
  oblique edge must produce a smooth coverage ramp, not a small set of
  discrete values. A sample-count ceiling (≤ 16 quantisation steps) is a
  contract violation, not a quality setting.
- **INV-COV-03 — One meaning per draw.** Backends may differ in cost, tiling,
  and scheduling; they may not differ in the colour, coverage, blend result,
  or failure of a given strip + brush. Divergence is a bug in a backend.
- **INV-COV-04 — Fail explicit.** Unsupported strip/brush/capability returns a
  sticky error (ADR-0095). No geometry substitution, no lower-quality
  fallback, no silent sample-count degradation.
- **INV-COV-05 — No multisample attachment.** The canvas must not allocate or
  resolve a multisample target for coverage. (Scene MSAA, if a 3D producer
  wants it, is that producer's attachment, not the canvas's.)
- **INV-COV-06 — CPU/GPU agreement is bounded and tested.** Coverage agreement
  is asserted per-frame within documented tolerances and is the definition of
  "a platform port is consistent".

## Rejected Alternatives & Negative Knowledge

- **Option A — Keep 4× MSAA.** *Why considered:* zero breakage; already
  implemented. *Why rejected:* five coverage levels is a hard quality ceiling,
  it already failed for icons badly enough to need a bake (ADR-0112), and it
  spends a 4× attachment and resolve on every pass for it.
- **Option B — Raise MSAA to 8×/16×.** *Why considered:* "one enum value,
  better edges". *Why rejected:* wrong axis and measurably so (ADR-0112):
  8× gives 9 levels, still worse than the bake's 65, while multiplying
  bandwidth and memory for *every* draw (text, images, effects) to buy a
  handful of levels on a few small shapes. It cannot reach the levels an
  analytic model gets for free.
- **Option C — Generalise the CPU coverage bake (ADR-0112) to all geometry.**
  *Why considered:* reuses proven code; deterministic. *Why rejected:* a bake
  is resolution-, identity-, and transform-keyed; a cache that re-implements an
  existing rendering path and is left alongside it is exactly the ADR-0112
  postmortem failure mode ("a fixed bug comes back"). It also cannot run on
  the GPU path at all.
- **Option D — Analytic coverage for icons and glyphs only; triangles + MSAA
  for everything else.** *Why considered:* targeted, cheap, no general break.
  *Why rejected:* it institutionalises two AA authorities and two coverage
  meanings; the general path — the one users actually see on large fills and
  strokes — stays aliased. This is the "partial solution" failure.
- **Option E — Mandate a single rasterization strategy (tile compute only).**
  *Why considered:* one backend is less to verify. *Why rejected:*
  per-primitive analytic coverage is measurably better for icon/glyph-dense
  surfaces (small primitives, many of them, where per-tile binning is overhead)
  and is *required* by the 3D scene reuse (ADR-0117). Mandating one forces the
  other's consumers onto a mismatched path; admitting two strategies behind one
  contract costs a second backend but preserves one semantics.
- **Option F — Keep the triangle contract and require backends to rasterize
  triangles.** *Why considered:* preserves the current front end. *Why
  rejected:* it re-imports the CPU geometry authority and its correctness
  ceiling, and triangles cannot yield >5 coverage levels without MSAA — the
  very thing being removed.
- **Option G — Keep the CPU oracle as a bit-exact shader port.** *Why
  considered:* strongest possible equivalence claim. *Why rejected:* it
  duplicates the GPU's *implementation* (rasterization, blend, dither, resolve)
  rather than the *semantics*. Under the coverage contract the oracle evaluates
  coverage math — a strictly smaller surface — and the honest statement is a
  bounded coverage tolerance rather than a claim of bit-exactness that the
  dither clause (ADR-0069) already contradicts.
- **Option H — Retain MSAA alongside coverage AA, selectable per pass.**
  *Why considered:* safest migration. *Why rejected:* two AA authorities are
  two semantics (ADR-0089); the multisample attachment's bandwidth is precisely
  the cost being removed; and a working MSAA path left in place is how a
  retired mechanism is silently resurrected.

## Consequences

Positive:

- Coverage is analytic and resolution-independent: oblique edges, wide strokes,
  and glyph outlines are smooth at 1× with ≥256 levels, at 1× bandwidth.
- Icons and text stop needing consumer-specific workarounds; the mask pool
  (~150 lines plus the atlas-pool coupling) is deleted rather than maintained.
- Self-intersection, holes, and arbitrary winding are handled by winding counts
  over strips — no stencil attachment, no two-pass cover, no stall routing
  (ADR-0014 retired).
- One semantic contract means GPU↔CPU parity is a coverage-tolerance test, a
  smaller and more honest surface than a bit-exact pipeline port.
- The `ANALYTIC_FRAGMENT` strategy is the same coverage math the 3D scene needs,
  so ADR-0117 adds a consumer, not a second rasterizer.

Negative & trade-offs:

- The new backends introduce target-specific resources (tile buffers,
  prefix-sum scratch, per-primitive control-point storage) that must obey
  ADR-0090/0092 ownership, retirement, and budget rules; the transient-buffer
  contract grows.
- Tile compute changes the frame's shape: a resolution/pass over strips rather
  than a sequence of draws. This is a real integration into ADR-0089's
  RenderPlan, not a drop-in replacement of a pipeline object.
- The CPU coverage evaluator is new code and must be verified against the GPU
  with explicit bounds; "it looks right" is not evidence.
- Every in-tree consumer that today submits triangles, and both Rust bindings,
  migrate in the same change (ADR-0095). No compatibility path is kept.
- Devices without a compute queue are not a supported canvas target for the
  default strategy; they take `ANALYTIC_FRAGMENT` or fail explicitly. Vulkan
  requires compute, so this is a policy question, not a hardware gap.

## Verification

- **Analytic area reference.** For a fixed geometry corpus (fill, hole,
  self-intersecting star, wide translucent stroke, glyph outline, icon), a
  brute-force 1024× supersampled reference image is the ground truth; every
  backend must match within a stated per-channel and mean tolerance at 1×.
- **Cross-strategy differential.** `TILE_COMPUTE` vs `ANALYTIC_FRAGMENT` vs
  `CPU` on the same strips must agree within the coverage tolerance, on the
  same corpus, on every CI lane.
- **Coverage-level assertion.** A high-contrast oblique edge must yield ≥ 256
  distinct coverage values (INV-COV-02), asserted directly rather than by eye.
- **Portability.** `tests/flux/integration/test_canvas_consistency.c` is
  rewritten to assert coverage agreement with tolerances; the Windows/macOS
  lanes prove the port against the CPU coverage oracle.
- **No-multisample gate.** A structural assertion that no canvas pass allocates
  or resolves a multisample attachment (INV-COV-05).
- **Retirement gates.** The stencil attachment, the icon mask pool, and the
  triangle-submission path are deleted; the leak/retire suites run green with
  them gone.
- **Performance budget.** Per-frame CPU geometry work is O(paths) strips, not
  O(vertices); run a benchmark over `bench_frame_loop` with a zoom-animation
  scene (where the legacy tessellation cache is defeated) as the headline
  number, with the legacy result recorded for comparison rather than dropped.

## References

- [ADR-0115](0115-analytic-curve-kernel-single-geometry-authority.md) — the
  strip this ADR rasterizes.
- [ADR-0117](0117-perspective-correct-scene-vector-coverage.md) — the 3D
  consumer of `ANALYTIC_FRAGMENT`.
- [ADR-0019](0019-canvas-backend-seam-and-cpu-backend.md) — the seam retained.
- [ADR-0071](0071-pass-scoped-canvas-antialiasing.md),
  [ADR-0009](0009-canvas-msaa-pipeline-key.md) — the MSAA authority retired.
- [ADR-0112](0112-device-resolution-icon-coverage-masks.md) — the icon bake
  retired; its negative knowledge is retained in place.
- [ADR-0014](0014-stencil-then-cover.md) — the stencil fill path retired.
- [ADR-0089](0089-rendering-program-and-execution-plan.md),
  [ADR-0090](0090-display-list-resource-ownership.md),
  [ADR-0092](0092-resource-planning-and-retirement.md),
  [ADR-0095](0095-flux-public-contract-convergence.md) — execution owner,
  ownership, synchronization, and explicit-failure contracts this must satisfy.
- Prior art: Vello / Sparse Strips (tile compute, prefix-sum winding, area AA);
  Slug (perspective-correct analytic coverage); Loop & Blinn (2005); Skia
  Graphite.
- Implementation: `libs/flux/src/canvas/backend.h` (coverage vtable),
  `backend_tile.c`, `backend_analytic.c`, `backend_cpu.c` (coverage oracle),
  `renderer.c` (pipeline keys without a sample axis).
