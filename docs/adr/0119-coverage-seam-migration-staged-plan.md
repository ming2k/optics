---
id: ADR-0119
title: "Coverage-seam migration — staged plan, acceptance harness first"
status: accepted
date: 2026-10-25
superseded_by: null
scope: libs/flux, libs/lens, bindings
negative_knowledge: true
---

# ADR-0119: Coverage-seam migration — staged plan, acceptance harness first

- Status: Accepted as a plan. **Stages S0–S3 have landed; stage S4 underway.**
- Date: 2026-10-25
- Scope: `libs/flux/src/canvas/` (the seam, backends, dispatch), `libs/lens`
  (icon mask cache removal), Rust bindings.
- Implements: [ADR-0116](0116-coverage-contract-canvas-rasterization.md).
- Depends on: [ADR-0115](0115-analytic-curve-kernel-single-geometry-authority.md)
  (the strip and its coverage primitives),
  [ADR-0118](0118-coverage-strategy-placement-distance-fields-and-curve-kernel.md)
  (the analytic branch, which already proves the device-side field math).
- Subordinate records: [ADR-0071](0071-pass-scoped-canvas-antialiasing.md),
  [ADR-0112](0112-device-resolution-icon-coverage-masks.md),
  [ADR-0014](0014-stencil-then-cover.md) — all superseded as mechanisms, all
  still live in the tree until the last stage of this plan lands.

## Context

ADR-0116 decides that the canvas rasterizes coverage strips at 1× sample rate
with analytic coverage, retiring 4× MSAA as the anti-aliasing authority. That is
a seam change, not a pipeline swap: every draw, both backends, the display-list
replay path, the icon bake and the stencil fallback depend on the current
contract. Attempting it in one change would be a multi-thousand-line rewrite with
no intermediate state that can be judged.

This session established a set of **measured baselines** that any staged plan has
to beat, and they are the reason for the ordering below:

| Fact | Measurement | Where |
|---|---|---|
| 4× MSAA quantises coverage to 5 levels | structural (INV-COV-02 ceiling) | ADR-0112 |
| A strip yields > 8 distinct levels at only 16×16 winding samples | measured | `test_curve` |
| Strip winding agrees with the closed-form field | 0.08 @16×16, 0.04 @32×32, monotone | `test_curve` |
| The analytic branch's `0.5 − d` ramp is a **single-edge** approximation | 0.25 vs true 0.125 at a sharp corner | `test_curve` |
| Analytic coverage carries a systematic surplus vs sampled coverage | 1.2–1.4 % of ink, matching perimeter×0.25 px | `test_canvas_squircle` |
| Polyline stroke bands double-blend only in retrograde cases | 204 → 4 pixels, then 0/10/0/15/0 | `test_canvas_stroke_region` |

Two of those rows are traps for a naive migration, which is why the plan puts the
harness first:

- The `0.5 − d` rule is **not** ground truth near corners. A backend that
  matches the analytic branch will therefore *differ* from a true area
  reference there, and a test that treated the branch as the reference would
  encode that error.
- Analytic and sampled coverage legitimately differ by ~1.3 % of ink on the
  same geometry. A migration gate that demanded bit-equality between the old and
  new paths would fail for the right change and pass for the wrong one.

## Decision

**Migrate in five stages, each with its own acceptance gate and kill criterion.
The acceptance harness (S0) lands before any backend exists, and no stage
removes a legacy mechanism until a later stage has made it unreachable.**

### S0 — Acceptance harness (must land alone, first)

Build the ground truth that every later stage is measured against, in
`tests/flux/unit/` and reusing the kernel primitives rather than new geometry:

1. **Area reference.** For a fixed corpus — simple fill, fill with a hole,
   self-intersecting star, wide translucent stroke, glyph outline, 12-px icon —
   render coverage from a strip by exact-area trapezoid accumulation at fine
   flattening tolerance, and cross-check it two ways: the coverage sum must
   equal the geometry's signed area, and it must converge to
   `flux_curve_cover_coverage` at increasing sample counts.
2. **Coverage-level assertion** (INV-COV-02): a high-contrast oblique edge must
   produce more than 16 distinct levels; the harness reports the count for every
   backend.
3. **Per-backend conformance report**: given a coverage plane and the reference,
   emit per-channel and mean deviation plus the fraction of pixels differing by
   more than a threshold. Every later stage cites this report rather than a
   bespoke test.

**Kill criterion:** if the area reference cannot be shown to agree with 64×64
supersampling to better than 0.5 % of area on the corpus, stop — the reference is
wrong and every later gate would be meaningless.

**Status: landed.** `tests/flux/coverage_harness.{h,c}` provides the two
references and the conformance report; `tests/flux/unit/test_cover_reference.c`
(60 assertions) verifies the yardstick itself, not a rasterizer. Measured:

| Check | Result |
|---|---|
| Area identity (Σ per-pixel coverage = signed area) | exact, at integer and fractional offsets |
| Area identity with a wound hole | exact (100 − 16 = 84) |
| Area vs supersampled, 8×8 / 32×32 | mean 0.02 / 0.005, monotone under refinement |
| Oblique edge coverage levels (INV-COV-02) | **31 from the area reference vs 4 from a 4× sample count** |
| Self-intersecting input | **refused** (`FLUX_ERROR_UNSUPPORTED`); the supersampled reference is the correct tool there |

Three defects in the harness itself were found by running it and are recorded
because each one would have poisoned every later gate:

1. The column clip wrote into the buffer the next row reads. It stayed invisible
   at integer offsets, where every row's band is identical.
2. `flux_curve_cover` is a *flat* segment run with no contour delimiter, so the
   first version treated a hole as part of the outer chain and summed the wrong
   area. Contours are now split where the chain breaks
   (`seg[i].p2 != seg[i+1].p0`), which is the only delimiter the representation
   offers — and worth noting for S1: a real backend needs the same split.
3. The reference **silently returned ~0** for a self-intersecting contour, because
   clipping a crossing polygon and summing signed areas cancels the lobes. A
   yardstick that reports nothing must never be trusted, so self-intersection is
   now detected (strict-straddle edge test) and reported as unsupported.

### S1 — A coverage rasterizer behind the existing seam, not the default

Implement a strip-consuming CPU coverage backend (`CANVAS_COVERAGE_CPU`) beside
the current triangle path, selectable but not default. It must not modify the
tessellator, the stroker, or any draw verb.

**Gate:** the backend passes S0's report on the whole corpus; the existing suite
and every lens icon test are unchanged; a frame rendered through it is compared
pixel-wise against the existing path and the *expected* ~1.3 % ink surplus data
is recorded, not treated as failure.
**Kill criterion:** if the backend cannot reach > 16 coverage levels on the
oblique-edge assertion, the strip representation is inadequate and ADR-0115's
output type needs revisiting before any further stage.

**Attempted, rejected, and why (recorded before retrying).** A row-span exact
integrator (`libs/flux/src/canvas/coverage_raster.c`) was written and measured
against S0, then removed rather than shipped. Results:

| Shape | mean \|Δ\| | max \|Δ\| | Verdict |
|---|---|---|
| axis-aligned square, 3 sub-pixel origins | **0.00000** | **0.00000** | exact |
| circle r=9 (production flattening) | 0.00046 | **0.1946** | 28 pixels wrong |
| self-intersecting star | 0.0085 | 1.0 | split budget exhausted |

Two defects were found and fixed on the way — a fixed-size band array that
silently dropped half-bands (losing area), and a mis-designed span that assumed
one straight chord per band when a flattened polygon's extremum vertex sits
inside it. What remained is localized to rows containing the polygon's
near-horizontal boundary regions and was not root-caused. **Max deviation 0.19
is not shippable**, so the file is gone and S1 is still open.

A second attempt fixed the band array and changed the crossing sort key to each
band's midpoint x (which removes the equal-x tie a shared vertex produces at a
band top). That was *necessary but not sufficient*:

| Shape | mean \|Δ\| | max \|Δ\| | Σcoverage − Σarea |
|---|---|---|---|
| square, integer and fractional origins | 0.000000 | 0.000000 | 0.000 |
| 64-gon circle | 0.001344 | 0.230 | **−4.16 of 254** |
| 32-gon circle | 0.000618 | 0.213 | −1.14 of 119 |

The diagnostic that matters: the loss is **concentrated in one row**. For the
64-gon centred at the origin in a grid starting at `oy = −9.77`, row 9 is the row
containing `y = 0` — the circle's **midline**, where the boundary is vertical and
the polygon's leftmost and rightmost vertices sit. Measured: row 9 contributes
13.82 where the reference has 17.97, while every other row is within 0.12.

So the fault is not the column integration (squares prove that) and not the sort
key (the midpoint fix changed nothing): it is the **span-closing condition at an
extremal vertex**, where two nearly-parallel edges meet inside a row and the walk
either leaves a span unclosed or closes it against the wrong partner. Precise
next hypothesis, for whoever retries: instrument the band containing the
extreme vertex, log `na`, the sorted x list and the winding walk, and check
whether the band is reached with an odd crossing count (which would leave `left`
non-null and drop the whole band).

A third pass was run as an explicit debugging task and found **three** root
causes, each fixed and measured:

1. **Odd crossing count at a band boundary.** Breakpoint dedup keeps one
   representative of a cluster of near-equal endpoint values, so an edge could
   end at `y` yet still compare as *active* in the band starting at `y`. The
   walk then left a span open and dropped the entire band. Fixed by snapping
   every edge endpoint onto the surviving breakpoint. (Diagnosed by logging the
   band: `na = 3`, which is impossible for a closed contour.)
2. **Row assignment from the band's start.** `floor(ya - oy)` lands one row low
   when `ya` *is* a computed row boundary, shifting a whole band into the row
   above. Fixed by taking the row from the band's midpoint, which is safe because
   bands never straddle a row boundary.
3. The earlier fixes: dynamic band array (was silently dropping half-bands) and
   a midpoint x sort key (removes the shared-vertex tie at a band top).

Effect on the 64-gon circle, against the harness:

| | area error | mean |max| | max |Δ| |
|---|---|---|---|
| before the debugging pass | −4.16 of 254 (1.6 %) | 0.001344 | 0.230 |
| after | **−0.025 of 254 (0.01 %)** | 0.000334 | 0.0718 |

Still **not shippable**, and the remaining evidence is specific:

- A half-disc (32-gon closed by a chord) still loses 0.95 % of its area, and the
  loss sits in two rows.
- Rows containing the polygon's top and bottom extrema over-count by ~20 % of
  their own (small) area: at the top of the circle, 5 pixels read 0.08–0.23
  against 0.05–0.22.

Two further hypotheses were then tested and **disproved**, which narrows the
space by elimination:

- *Extremum band spans are wrong.* Disproved: logging the three bands of the row
  containing the circle's top vertex shows correct spans (left 0 → −0.88, right
  0 → +0.88 across the cap band), correct `dir` (+1 / −1), and correct t ranges
  (0.770 → 1.000 across the row).
- *Bands straddle row boundaries* — the alternating ±0.01–0.05 pattern across
  **every** row looks like a one-row shift, and breakpoint dedup can merge a row
  boundary with a nearby endpoint. Disproved: accumulating each band into every
  row it overlaps produced **byte-identical** results, so `r0 == r1` always.

What remains is therefore a **redistribution, not a loss**: total area is right
to 0.01 %, the mean error is 0.0003, and the residuals alternate sign at
±0.01–0.05 per row with the two extremum rows gaining ~0.1. The cause is not
identified. The candidates left are the sub-interval trapezoid when a span's two
sides are both near-vertical (the cap bands are ~0.01 tall, so `t1 − t0` is tiny
and the overlap width changes fast), and a disagreement with the harness in how a
polygon vertex lying exactly on a pixel boundary is treated.

The per-row and pixel-level breakdown that made this pass productive is now part
of the S0 report (`worst_row`, `worst_row_delta`), so it need not be rebuilt, and
the prototype's three fixes are specified above precisely enough to re-apply —
the prototype itself lived outside the tree and is deliberately not preserved as
dead code.

Two lessons that are inputs to the retry, not hindsight:

1. **Per-row sums localise this class of bug instantly.** The global numbers
   ("mean 0.001, area short by 1.6 %") say a rasterizer is wrong; only the
   per-row breakdown says *where*, and it reduced the search to a single band.
   That breakdown belongs in the harness's report, not in a debug patch.
2. **S0 did its job on its first customer.** Every number above came from the
   harness built in the previous stage; without it, "the square is exact and the
   mean is 0.001" would have been taken as evidence that the whole thing worked.

**Status of S1: the algorithm has landed, the seam wiring has not.**

`libs/flux/src/canvas/coverage_raster.c` implements the sub-band exact integrator
described above, and `tests/flux/unit/test_coverage_raster.c` (4186 assertions)
gates it against S0. Measured against the harness, on the same polygon:

| Shape | mean \|Δ\| | max \|Δ\| | levels |
|---|---|---|---|
| square, 4 grid origins (integer and fractional) | 0.0000000 | ≤ 5e-7 | 2–10 |
| circle 64-gon, two origins | 0.0000000 | ≤ 5e-7 | 61 / 17 |
| half disc (chord-closed 32-gon) | 0.0000000 | ≤ 5e-7 | 49 |
| the production flattener's own circle output | 0.0000000 | ≤ 5e-7 | 65 |
| self-intersecting star (vs 64× supersampling) | 0.00006 | 0.059 | 22 |

Simple shapes are exact to float precision, band by band, so the remaining
deviation on the star is the *yardstick's* error at its thin spikes, not the
rasterizer's. The star also confirms that non-zero paints overlapping lobes
(total coverage 40.5) where the algebraic reference refuses the input entirely.

**Four defects were found and fixed during the debugging pass**, and each is now
a comment at the code that depends on it. Two of them are worth naming here
because they are the traps a future reimplementation will hit:

1. **The integration parameter must be the band's own [0,1], never the
   row-relative y.** The span's endpoints describe the band, so a row-relative
   parameter interpolates them wrongly unless the band happens to fill the entire
   row. That is exactly why axis-aligned squares measured exact while curves were
   off by 1.6 %: squares' bands fill their rows. This single bug accounted for
   the whole residual error.
2. **Edge endpoints must be snapped onto the surviving breakpoints.** Breakpoint
   dedup keeps one representative of a cluster of near-equal values, so without
   snapping an edge can end at a band boundary yet still compare as active in the
   band below, leaving that band with an odd crossing count and dropping its
   entire area.

Two hypotheses were tested and disproved along the way (extremum band spans;
bands straddling row boundaries), which is what narrowed the search to the
parameter-space bug. The decisive instrument was the harness's per-row report
plus a per-band comparison of accumulated area against the analytic trapezoid —
the accumulated-vs-analytic line is what made the parameter error obvious.

**S2 has landed: coverage is the default for solid fills and the route is active.**

Enabling it required exactly the three changes measured in the previous pass, and
the two test updates were corrections of stale expectations rather than
regressions:

| Change | What it was |
|---|---|
| Null `backend`/scratch guard in the route | `test_canvas_tess` builds a canvas by hand with `backend == NULL`; the route dereferenced it. |
| `test_canvas_tess_cache` now paints a **gradient** | Solid fills no longer reach the tessellator, so they generate no cache events. The cache is **narrowed, not dead** — it still serves every painted fill that needs triangles. |
| `test_canvas_cpu`'s even-odd case inverted | It asserted that an even-odd fill in a no-stencil pass must *fail*. Analytic coverage accumulates winding from sorted crossings, so **even-odd now works with no stencil attachment**; the test now asserts the capability on two nested same-winding squares (non-zero fills both, even-odd leaves the inner one empty) instead of the old refusal. |

Two corrections to this plan's own earlier wording, made because the code taught
them:

1. **S2 does not delete the supersample buffer.** Gradients, images, glyphs and
   strokes still take the triangle path and still need it. Deleting it belongs to
   S4, after S3 gives those paints a coverage representation too.
2. **The tessellation cache is narrowed, not retired.** It is a property of the
   tessellation path, and that path still serves every non-solid fill.

`canvas_consistency` (GPU↔CPU) and `canvas_render`'s goldens pass **unmodified**
with the flip active — the strongest single statement available here, since the
GPU still renders MSAA triangles and the CPU now renders analytic coverage for
the same draws, and the existing tolerances already accept the difference.

**A third correction: the oracle rewrite belongs to S3, not S2.** This plan said
S2 would rewrite `test_canvas_consistency` as a coverage oracle. It did not, and
should not: while the GPU still renders MSAA triangles, comparing coverage
against triangles *with the current tolerances* is exactly the right check — it
is what proves the flip changed nothing a user can see. Rewriting the oracle
becomes meaningful once both sides speak coverage, which is S3.

**The seam is not solid-specific (extension landed after S2).** Coverage answers
*where* a pixel is painted, so it belongs to every brush a backend can evaluate,
not only to a flat colour. `submit_cover` therefore takes the draw's
`canvas_paint` — kind, colour and gradient stops — and the CPU backend evaluates
a gradient per pixel with the same `frag_gradient` the triangle path uses. A
backend that cannot evaluate a brush returns `FLUX_ERROR_UNSUPPORTED` and the
front end falls back per draw.

Measured on the same oblique edge as the ceiling test:

| Path | Distinct levels |
|---|---|
| CPU, solid fill | 117 |
| CPU, **gradient fill** | **88** |
| GPU, 4× MSAA | 10 |

What still tessellates on the CPU is therefore: image paints, glyph runs,
strokes, and any non-SRC_OVER blend. `test_canvas_tess_cache` now exercises the
cache through one of those (a gradient with `FLUX_BLEND_PLUS`), which is the
honest statement of the cache's remaining scope — it serves the draws the
coverage seam declines, not every draw.

**Performance, measured (this was missing from S1 and S2 until now).** The CPU
software backend, one fill of a filled shape, 512² canvas, per-fill microseconds:

| Shape | Coverage | Tessellation (forced via a non-SRC_OVER gradient) | Ratio |
|---|---|---|---|
| circle, ~120 vertices | 7.5 ms | 43.2 ms | 0.17× |
| star, 64 vertices | 4.7 ms | 16.9 ms | 0.28× |
| star, 256 vertices | 9.1 ms | 38.3 ms | 0.24× |
| star, 512 vertices | 45.9 ms | 56.1 ms | 0.82× |
| star, 1024 vertices | 86.3 ms | 95.6 ms | 0.90× |

Three conclusions, all load-bearing:

1. **The change is never a regression.** Coverage is faster than the tessellation
   path at every vertex count measured, by 3–6× on the shapes UI is made of and
   by ~10 % on pathological ones. That is why no vertex-count guard was added:
   a guard would push complex paths onto the *slower* path.
2. **The absolute cost is high and quadratic-ish in both paths.** 256 → 512
   vertices is 9 ms → 46 ms. The cause is the same shape as the old one — an
   O(vertices) active-edge rescan per scanline band, against ear clipping's own
   O(n·candidates) — so this is a pre-existing property of the software backend,
   now visible. The obvious fix is an **active edge table** (sort once by y, keep
   a live list), which would remove the quadratic term from the new path; it is
   recorded here rather than done, because it is an optimisation of a backend
   that is not the production renderer.
3. **The per-fill allocation was a real defect and is gone.** The coverage plane
   is canvas-owned and reused (it would otherwise churn 8 MB at 1080p and 33 MB
   at 4K per draw). It remains resident for the canvas's lifetime — a disclosed
   memory cost, not a hidden one.

**A capability gained, not just a path swapped:** even-odd fills no longer need a
stencil attachment for solid paint on a coverage backend, because winding is
accumulated from sorted crossings rather than from an algebraic area sum. Before
the flip this function had to *diagnose and drop* such a draw in a no-stencil
pass (that refusal was `test_canvas_cpu`'s expectation, now inverted into an
assertion that two nested same-winding squares produce a ring under even-odd and
a solid block under non-zero). The stencil fallback of ADR-0014 is therefore
unused for solid fills on CPU; it is **not** deleted, because the Vulkan backend
still routes even-odd through it.

**The seam is also landed:** `flux_canvas_backend` gained
an optional `submit_cover` op (nullable, so a backend that omits it keeps the
triangle path), and the CPU backend implements it: it rasterizes the contour set
at pixel resolution and composites the premultiplied brush colour through the
plane once per supersample, so the existing downsample averages to exactly the
analytic coverage. `canvas_fill_path_internal` carries the routing — geometry
transformed to physical pixels, both fill rules handled with no stencil
attachment — behind `canvas_fill_path_internal`, now active with the guards described above.

Activation was attempted and measured, and it *is* stage S2. Enabling it broke
exactly three tests, and the breakages are the S2 work list:

| Test | What it showed |
|---|---|
| `canvas_tess_cache` | asserts cache hits/misses/stores; with coverage fills there are none, because the ear-clip path and its cache are **dead for solid fills on the CPU backend**. The S4 deletion arrives early. |
| `canvas_cpu` | asserts a particular draw returns `FLUX_ERROR_INVALID_STATE`; that draw now succeeds, so a test that pins a *failure* no longer describes the system. |
| `canvas_tess` | SIGSEGV: it stubs `flatten_path_to_contours` and builds a canvas without backend `init`, so `scratch_contours` is NULL when the route touches it. The route needs a null-scratch guard regardless. |

So the flip is not a one-line change and must not be pretended to be: it retires
a cache, invalidates a failure expectation, and needs a defensive guard. All
three are enumerated above with their causes, which is what makes S2 mechanical.
Until then the seam is a designed extension point with an unused implementation,
not an integrated path — stated plainly rather than implied otherwise.

### S2 — Make it the CPU default and delete the supersampling path

Switch the CPU backend's default to the coverage rasterizer; delete the
supersample buffer, its downsample, and `flux_canvas_cpu_coverage`'s
level-counting contract. The CPU backend stops being a *port of the GPU
pipeline* and becomes the **coverage oracle**.

**Gate:** the GPU↔CPU consistency oracle is rewritten to assert coverage
agreement against S0's reference with documented tolerances; the leak, retire
and fault-injection suites pass with the supersampling machinery gone.

### S3 — Device backends

**S3's target is now a measurement, not an argument.**
`tests/flux/integration/test_canvas_coverage_ceiling.c` draws one oblique edge
through both paths and counts the distinct coverage values along it:

| Path | Distinct levels |
|---|---|
| CPU, analytic coverage | **117** |
| GPU, 4× MSAA | **10** |

The assertion is the contract, not a hand-picked number: INV-COV-02 requires more
than 16, the device path cannot reach it because its coverage is quantised by a
sample count, and the gap is asserted as an order of magnitude. **S3's completion
condition is that this test's device expectation moves above 16** — it fails
loudly the day the device gains coverage, so the step cannot be forgotten.

**Correction to this plan's own wording:** earlier drafts described
`ANALYTIC_FRAGMENT` as "already proven, mainly wiring". That was imprecise. What
ADR-0118 proved is the analytic branch for *analytic primitive classes*
(rrect / circle / squircle), which is why those already render at 1× on the
device. For *arbitrary paths* the device has no coverage backend at all, and a
per-fragment reconstruction for general fills — holes, self-intersection,
arbitrary winding — is not a small extension of the primitive shader; at that
scope it is the tile/winding backend. So S3 has no cheap half:

- **`TILE_COMPUTE`** (segment binning into tiles, prefix-sum winding backdrop,
  per-pixel area integration) is the real content, and it is the one
  research-grade piece in this program. It is planned in
  [ADR-0120](0120-gpu-analytic-coverage-tile-compute-plan.md), which makes a
  real-driver CI lane a prerequisite rather than follow-up work.
- Its acceptance is already built: the same S0 corpus and the ceiling test above,
  measured on the device.


`ANALYTIC_FRAGMENT` for analytic classes is already proven in shape — ADR-0118
landed the shape-space field and rotation, verified by the GPU↔CPU oracle. The
new work is `TILE_COMPUTE` for general strips behind the same seam.

**Gate:** both device strategies pass S0's report within the coverage tolerance;
a zoom-animation scene (where the legacy tessellation cache is defeated) shows
CPU geometry work proportional to paths, not vertices, recorded against the
legacy baseline.

### S4 — Delete what is now unreachable

**S4 in progress following S3/ADR-0120 Phase B delivery.** Updated status:

| Deletion | Status & Blockers |
|---|---|
| MSAA attachment and `flux_canvas_antialias` | **Unblocked & default retired:** The Vulkan backend now renders with 1x analytic coverage (`vk_submit_cover`, 104 levels in `test_canvas_coverage_ceiling`). `AUTO` antialias policy operates strictly in 1x single-sample without allocating an MSAA attachment. |
| Stencil fallback for even-odd | **Unblocked & retired:** Native analytic coverage evaluates even-odd parity without stencil attachment on both CPU and Vulkan paths (`test_canvas_render`). |
| Ear-clip tessellator and its cache | **Partially unblocked:** Solid and gradient fills on Vulkan route through `submit_cover`. The tessellator is stranded for all SRC_OVER fills; retained only for strokes, image paints, and non-SRC_OVER blend fallbacks pending stroke-band cover migration. |
| Icon coverage bake in `libs/lens` | **Unblocked:** Device path now has native analytic coverage (104 levels), rendering vector icon geometry directly at 1x with high-contrast edge fidelity. |
| CPU supersample buffer | Still serves gradients, strokes, images and glyphs on the software backend. |

The largest remaining role is now the **device** path, and there is no
incremental step left on the CPU that narrows it further without a GPU coverage
backend: everything the CPU can evaluate analytically it now does. Strokes are a
separate axis — they are expanded on the CPU before they reach the seam, so
giving them coverage means offsetting reduced segments, which ADR-0115 closed on
measurement (Options H and I).

Deletions land with the stage that makes them unreachable, never ahead of it —
no forwarders, and no mechanism left in place "for now" once its stage is green
(INV-SEAM-04). The rows above are the standing checklist: each becomes actionable
only when its blocker column is empty.

### Invariants & Behavioral Boundaries

- **INV-SEAM-01 — Reference before backend.** No backend change may be accepted
  before S0's harness exists and is trusted.
- **INV-SEAM-02 — The field is not the reference.** Coverage conformance is
  measured against area, never against the analytic branch's `0.5 − d` ramp,
  which is a single-edge approximation (0.25 vs 0.125 at a sharp corner).
- **INV-SEAM-03 — One meaning per draw.** Stages may differ in cost and
  scheduling; they may not differ in the colour, coverage or failure of a given
  strip + brush. Divergence is a bug in a stage, not a tuning difference.
- **INV-SEAM-04 — Legacy mechanisms leave with the stage that strands them.**
  No stage may leave an unreachable mechanism in place "for now".
- **INV-SEAM-05 — Staged, not partial.** A stage that cannot pass its gate is
  reverted whole. Half a backend behind the seam is worse than none.

## Rejected Alternatives & Negative Knowledge

- **Option A — One-shot replacement of the canvas rasterizer.** *Why considered:*
  the target architecture is coherent, and a staged plan risks landing stages
  1–2 and stopping. *Why rejected:* the change spans the seam, both backends, the
  display-list replay path, effects that sample the canvas, and every consumer's
  pixel tests. There is no intermediate state to judge, and the resulting diff
  could not be reviewed against a reference that does not yet exist.
- **Option B — Use the analytic branch as the acceptance reference.** *Why
  considered:* it is exact for the geometry class it covers, and already
  measured. *Why rejected:* **measured wrong at corners** (0.25 where the true
  area coverage is 0.125). Adopting it as ground truth would bake a known error
  into every future gate — the ADR-0112 failure mode with a test suite behind it.
- **Option C — Golden images as the gate.** *Why considered:* cheapest to
  produce; the repo already has Prism material goldens. *Why rejected:* goldens
  cannot distinguish "different and better" from "different and broken", which
  is precisely the judgement the migration needs; and they carry no statement
  about the coverage contract, so INV-COV-02 would be unverified.
- **Option D — Migrate the GPU first (higher visibility).** *Why considered:* the
  quality complaint that motivated ADR-0116 is on screen. *Why rejected:* the CPU
  backend is the portable oracle; migrating the device first leaves nothing
  trustworthy to compare it against on platforms without a driver, and the
  Windows/macOS lanes would lose their portability proof for the duration.
- **Option E — Keep MSAA as an opt-in alternative long-term.** *Why considered:*
  lets a consumer keep the cheap path. *Why rejected:* two anti-aliasing
  authorities are two semantics (ADR-0089), the multisample attachment's
  bandwidth is the cost being removed, and a working MSAA path left in place is
  how a retired mechanism gets silently resurrected (ADR-0112's postmortem).

## Consequences

Positive:

- Every stage has a falsifiable gate and a stated kill criterion, so the work can
  be abandoned at any point with the tree still consistent.
- The coverage reference is reusable: it is what makes INV-COV-02 checkable for
  any future backend, and it outlives this migration.
- The trap measurements from this session become permanent guards rather than
  folklore.

Negative & trade-offs:

- Stages 1–2 add a third coverage path for a period; the seam must therefore
  admit three strategies, not two, until S4.
- The area reference is slow (exact-area accumulation plus convergence checks), so
  it belongs in the unit suite and the CI gates, not in a frame loop.
- S3 is the only stage that cannot be judged on this machine alone: it needs both
  a real device and the Windows/macOS lanes, so its gate is inherently
  cross-platform and slower to satisfy.
- The plan does not shorten the work; it only makes each increment refuse-able.

## Verification

This record's own verification is that its gates are *checkable with what the
tree already has*:

- S0's convergence check reuses `flux_curve_cover_coverage` (landed, 191
  assertions in `test_curve`, including the cross-route agreement and the
  coverage-level count).
- S1's "does not disturb anything" check reuses `test_canvas_consistency`
  (GPU↔CPU, now including rotated analytic shapes) and the lens icon pixel tests.
- S2's oracle reuses `test_canvas_stroke_region`'s self-calibrating
  single-coverage technique, which needs no golden image.
- S3's legacy baseline is the existing `bench_frame_loop`, with the
  zoom-animation scene ADR-0116 names.

Only S0 is implemented. The current tree still has ADR-0071's MSAA, ADR-0114's
tessellator, ADR-0014's stencil fallback and ADR-0112's icon bake as live code,
exactly as
[the implementation matrix](../dev/architecture-implementation-status.md)
records. S0 changed no rendering code: it adds a test-tree harness and the test
that proves it, and it is deliberately outside libflux so that no backend can
validate itself against its own algorithm.

## References

- [ADR-0116](0116-coverage-contract-canvas-rasterization.md) — the decision this
  plan executes.
- [ADR-0115](0115-analytic-curve-kernel-single-geometry-authority.md) — the strip,
  the coverage primitives, and the measurements cited in Context.
- [ADR-0118](0118-coverage-strategy-placement-distance-fields-and-curve-kernel.md)
  — the analytic branch, and the source of the corner-accuracy caveat.
- [ADR-0089](0089-rendering-program-and-execution-plan.md) — no forwarders, one
  semantic authority.
- [ADR-0095](0095-flux-public-contract-convergence.md) — explicit failure, sticky
  errors, move-together migration.
- [ADR-0112](0112-device-resolution-icon-coverage-masks.md) — the postmortem whose
  failure mode the harness-first ordering exists to avoid.
- Implementation: S0 landed — `tests/flux/coverage_harness.{h,c}`,
  `tests/flux/unit/test_cover_reference.c` (60 assertions). S1 is the next
  stage and must not start before S0's numbers are accepted as the baseline.
