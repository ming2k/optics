---
id: ADR-0148
title: "A distance field owns its derivative — analytic normals, pow-free fields, one evaluation"
status: accepted
date: 2026-11-04
superseded_by: null
scope: [flux, prism, shaders, architecture]
negative_knowledge: true
---

# ADR-0148: A distance field owns its derivative — analytic normals, pow-free fields, one evaluation

- Status: Accepted
- Date: 2026-11-04
- Scope: `libs/flux/include/flux/shaders/flux_sdf.glsl`, `libs/flux/include/flux/sdf.h`,
  `libs/flux/src/sdf/sdf.c`, `libs/prism/src/shaders/liquid_glass.comp`,
  `tests/flux/unit/test_sdf.c`, `tests/flux/integration/test_sdf_parity.c` and its
  `shaders/sdf_parity.comp` harness, `tools/guardrails/symbols.py`.
- Amends: [ADR-0118](0118-coverage-strategy-placement-distance-fields-and-curve-kernel.md)
  (extends `[INV-SDF-02]` from "one definition per field" to "one definition per field
  *and* its derivative").
- Depends on: [ADR-0046](0046-liquid-glass-convex-lens-model.md) (the convex-lens rim that
  consumes the normal), [ADR-0063](0063-liquid-glass-material-library.md) (flux owns
  mechanism, prism owns material identity).

---

## Context

`[INV-SDF-02]` made a distance field exist exactly once in each language, so that host and
device cannot drift. It said nothing about the field's *derivative*, and the derivative is
where the drift happened.

`liquid_glass.comp` needed a surface normal to drive refraction, chromatic dispersion, rim
lighting and contact AO. It got one the way a shader author reaches for first — by
differencing the field:

```glsl
float dx = glass_sdf(p + vec2(1.5, 0.0), b) - glass_sdf(p - vec2(1.5, 0.0), b);
float dy = glass_sdf(p + vec2(0.0, 1.5), b) - glass_sdf(p - vec2(0.0, 1.5), b);
rim_normal = normalize(vec2(dx, dy) + vec2(1e-6));
```

Four extra field evaluations per rim pixel. On a real UI chrome body — a pill, a chip, a
card — the rim band is not a thin edge: `size_scale` clamps it to `body_min * 0.35`, so the
band is 53% of a 240x32 pill's body pixels, 76% of a 64x64 card's, and 18% of a large panel's.
And each of those evaluations carried six `pow()` calls, because the squircle's L4 corner
was written as `pow(pow(x,4)+pow(y,4), 0.25)` plus `sqrt(pow(x,6)+pow(y,6))/pow(q4,3)`:

| per rim pixel, single-shape body | before |
|---|---|
| squircle field evaluations | 6 (1 coverage + 1 shadow + 4 taps) |
| `pow()` | **36** |
| two-shape fused body (the metaball merge) | 12 evaluations, **72** `pow()` |

Three defects came with that shape, and they are the reason this is an ADR rather than a
patch:

1. **The normal was a neighbour of the field, not a property of it.** A differenced normal
   is first-order accurate and its output depends on the differencing span, so "the normal"
   was really "the normal at a 1.5 px scale". It also had seven significant digits of
   agreement with nothing in particular: `flux_sdf_squircle` returning `d` and the material
   computing `d`'s gradient were two independent readings of one shape.
2. **The field had no derivative anywhere.** `flux/sdf.h` exposed distance only. Any
   consumer needing a normal had no option but to invent one — which is exactly the failure
   mode `[INV-SDF-02]` exists to prevent, moved one level up.
3. **`pow()` was being paid for arithmetic.** Every exponent in the field is a small integer
   or `1/4`. `x^4 + y^4` is two multiplies, `^(1/4)` is two square roots,
   `sqrt(x^6+y^6)/q4^3` reuses both. Six transcendentals became zero.

## Decision

**A distance field owns its derivative.** `flux_sdf.glsl` and `<flux/sdf.h>` expose, for the
rounded-rectangle family, one decomposition that produces the field *and* its analytic
gradient from a single evaluation, and every consumer obtains its normal from that:

1. **`flux_squircle_corners(p, half_extent, radius, with_l4)`** — the corner decomposition
   (signed corner coordinate, interior term, L2 field + gradient, L4 field + gradient).
   `flux_sdf_round_rect`, `flux_sdf_squircle` and both derivative entry points are projections
   of it (with the measured exception recorded under Consequences: the four L2-only materials
   keep a struct-free spelling because sharing the struct cost them more than it saved).
   `curvature` is a compile-time constant at every call site, so the superellipse arc is
   compiled out of every plain-rounded-rectangle material — those four SPIR-V modules carry a
   single `Sqrt`, which is the L2 arc, and no `Pow`.
2. **No `pow()` in the field family.** `x^1.25` in the rim lighting is likewise
   `x * sqrt(sqrt(x))`. SPIR-V `Pow` count in `liquid_glass.comp`: **10 → 0**.
3. **Two derivative entry points, because there are genuinely two answers.**
   - `flux_sdf_squircle_gradient` / `flux_sdf_squircle_gradient()` is the *continuum*
     gradient. Inside the interior region (`q <= 0` on both axes) the field degenerates to
     `max(q.x, q.y) - r`, two planes meeting on a diagonal; the continuum gradient picks the
     dominant edge and flips by 90 degrees across zero width.
   - `flux_sdf_squircle_sample` / `flux_sdf_squircle_sample()` returns `(d, nx, ny)` from
     **one** evaluation, using the identical analytic gradient everywhere it exists and, in
     the interior only, a linear ramp `h = clamp(0.5 + (qx - qy)/(2*aa))` of the two edge
     directions across the caller's pixel footprint `aa`. That ramp is the continuous limit
     of what a central difference of the same span measures, so a consumer switching from
     taps to this call keeps its look. Measured: the ramp rotates 0.000 degrees per sample
     step where a hard pick flips 1.41 (90°), and the `aa`-independence outside the interior
     is asserted exactly.
   A caller must not shade with the continuum variant; the headers say so at the type, at
   each entry point, and `test_sdf` asserts the distinction so it cannot rot.
4. **`liquid_glass.comp` consumes it.** `glass_normal()` is one `flux_sdf_squircle_sample`
   per shape, composed by `flux_smooth_union_gradient`. The composition lives in
   `flux_sdf.glsl` too, not in the material: a fused body is one field, and ADR-0063 puts
   field mechanism in `flux`. The derivation is worth recording — the polynomial
   smooth-minimum `mix(d.y, d.x, h) - k*h*(1-h)` has a second gradient term through `dh/dp`,
   and that term is **identically zero** because solving the blend's own `h` gives
   `d.x - d.y == k*(1 - 2h)` exactly, which is the factor the extra term multiplies. The
   gradient of a polynomial smooth-min is exactly linear in `h`; no approximation, and the
   four multiplies the "full" form would cost buy nothing. Measured: the full form scored
   *worse* against a finite-difference reference (p50 11.0° vs 0.00°), which is what sent me
   back to the algebra.

Net per rim pixel, single-shape body: **6 squircle evaluations → 2**, **36 `pow()` → 0**,
and the normal is now the derivative of the distance that was drawn.

### Measured, not inferred

Static SPIR-V instruction count is the wrong metric here and it moves the wrong way:
`liquid_glass.comp` grew 1068 → 1519 instructions. What a pixel executes fell 3x, and the
only honest way to report that is to time the dispatch. `tests/prism/integration/
bench_liquid_glass_gpu.c` renders 64 overlapping bodies on a 1024x1024 surface for 60 frames
with a device fence per frame, and subtracts its own `group_count == 0` floor so the number
is glass work rather than capture + blur + present. Median of three interleaved rounds,
Intel ARL (Mesa ANV):

| variant | glass work | vs HEAD |
|---|---|---|
| HEAD (4-tap, `pow` field) | 4.336 ms | — |
| pow-free field only (4-tap kept) | 4.097 ms | **−5.5%** |
| ADR-0148 (pow-free + analytic normal) | 3.841 ms | **−11.4%** |

Monotonic in every round. The two stages are separable, which is why the benchmark takes a
body count: the split confirms the analysis rather than merely the conclusion.

The `prism.liquid_glass_apply` Tracy zone could not answer this. It is CPU recording latency
— the dispatch's execution is not inside it — and on the 64x64 test target there is nothing
to contain. On that zone the same three variants measure within ±20% of each other in both
orderings, i.e. noise. That result is why the GPU harness exists.

## Alternatives Considered

- **Keep the 4-tap normal and only remove `pow()`.** Halves the transcendental cost and
  keeps three of the four taps. Rejected: it leaves the normal a function of a differencing
  span, leaves `flux/sdf.h` unable to answer "what is the normal", and leaves every future
  consumer to invent its own. The field/derivative split is the defect; `pow()` was the
  symptom that made it expensive.
- **Expose only the continuum gradient.** Simplest possible surface. Rejected: it flips 90°
  across the interior diagonal, and the interior is where a glass body's refraction
  actually samples. Shading would gain a hard seam exactly where the old 4-tap version had
  a smooth ramp — a visual regression traded for an API saving.
- **Expose only the `aa`-resolved entry point.** One function instead of two. Rejected: it
  hides a real mathematical distinction behind a sampling parameter and would invite a
  caller to pass `aa = 0` expecting the exact gradient. Two names whose difference is
  asserted by a test is honest; one name that means different things on different pixels is
  not.
- **Bake the gradient into a texture.** Rejected under `[INV-SDF-04]`: a bake is an explicit
  opt-in optimisation whose quality is bounded by resolution. Here it would also be slower —
  the analytic gradient is arithmetic on values the field already computed.
- **Ship a `fp16` arithmetic variant.** Rejected *after building and measuring one*, which is
  the only way this question has an honest answer. The work went far enough to know exactly
  why it fails:
  - The bound is real. `nx^4 + ny^4 == 1` forces `g = sqrt(nx^6 + ny^6) in [2^-1/4, 1]`, so
    the L4 normalisation carries at 16-bit, and a 16-bit build measured **0.05 degrees** of
    normal error across a six-shape sweep.
  - But `q_pos` is pixel-magnitude, and `x^4` overflows binary16 (max 65504) at a 24 px half
    extent. A naively narrowed field measured **0.92 px** of distance error on a 512x320
    panel — a quarter-pixel of coverage error — because binary16's ULP at 464 is 0.25 px.
    Making it correct requires re-expressing the whole decomposition in the normalised frame,
    i.e. a second spelling of the field.
  - And the ceiling does not justify that: the field + gradient path is **337 of
    `shade_body`'s 708 SPIR-V instructions**. Excluding texture fetches, the best case for
    16-bit is roughly a 14% kernel reduction, in exchange for the duplicated definition that
    ADR-0118 exists to prevent.
  The decision is recorded with its numbers so the attempt is not repeated from scratch. The
  bounded-intermediate property is asserted in `test_sdf`, so a future binary16 effort starts
  from a proven precondition. The device capability (`shaderFloat16`), the width aliases and
  the measurement harness were all **removed** rather than left dormant: an unused
  capability query is scaffolding, and scaffolding rots.
- **Tile-memory (TBDR) backdrop reads.** Not applicable: prism materials are compute
  dispatches (ADR-0063) reading bindless textures, and Vulkan's tile-local reads exist only
  inside a fragment pass. Recorded as negative knowledge so it is not proposed again.

## Consequences

- **`[INV-SDF-02]` is extended**: a distance field is defined once *together with its
  derivative*. A new analytic primitive must land its gradient in `flux_sdf.glsl` and
  `<flux/sdf.h>` in the same change, and `tests/flux/integration/test_sdf_parity.c` fails if
  the two disagree.
- **The device/host agreement is now mechanically enforced, not asserted.** That test
  dispatches the real `flux_sdf.glsl` on the GPU and compares distance and normal against
  `<flux/sdf.h>` over 6 shapes × 4096 samples × both entry points. It was verified to fail in
  every direction that matters: perturbing the field distance, the shading normal, and the
  continuum gradient each fails it independently, and perturbing *either* language alone
  fails it. Tolerances are derived from the expression's conditioning (see Negative
  Knowledge), not tuned to observed output.
  Two exclusions are explicit rather than silent, because both are mathematics rather than
  noise: the continuum gradient is discontinuous across the interior medial axis by
  definition (a hard pick flips 90° there), so samples within 1e-3 of that axis are counted
  and reported, not compared. The test asserts `compared + skipped == SAMPLE_COUNT` and
  `compared > 0`, so a run cannot pass vacuously by excusing everything.
  This closes a gap that predated this ADR: `test_sdf` compared the host against a *hand
  transcribed* reference of the shader expression, so it could not detect shader drift at
  all — the mechanical enforcement `[INV-SDF-02]` claimed existed in prose only.
- **New public API in `<flux/sdf.h>`**: `flux_sdf_sample` (a `float distance` plus a unit
  `flux_point normal`), `flux_sdf_squircle_gradient`, `flux_sdf_squircle_sample`,
  `flux_sdf_smooth_union` and `flux_sdf_smooth_union_gradient`. The header is registered in
  `tools/guardrails/symbols.py`, so `./tools/gate fix` lists them in
  `docs/reference/symbols.md`.
- **The metaball composition moved out of the material.** `smooth_union` was a private
  function inside `liquid_glass.comp`. A fused body is one field, and a material carrying its
  own union is a second definition of it (ADR-0063). It now lives in `flux_sdf.glsl` /
  `<flux/sdf.h>` — which is also what makes it verifiable: the fused path had no pixel or
  numeric coverage at all before, so a wrong union gradient would have shipped silently. It is
  now one of the three modes the GPU parity harness sweeps.
- **The shipped render moves by at most 2/255, not zero.** `test_prism_golden` measures
  `max_delta = 2` with 0 outliers against the committed reference (tolerance 12, cap 36), and
  `test_liquid_glass` passes all 303 assertions including its hard invariants (transparent
  outside the silhouette, opaque flat interior, lit-side rim brighter than plate,
  bit-identical re-render of the *same* code). The change is dither-level invisible, not
  bit-identical: the normal is now a different expression, so identical output was never the
  claim to make.
- **The interior crease rule is measured, not asserted in prose**: 0.000°/step on a
  footprint-wide ramp, against 1.41 for the continuum pick, and exact `aa`-independence
  outside the interior over >20k sampled pixels.
- **Half precision is answered, not deferred.** The L4 normalisation factors are bounded in
  [0, 1] by construction (`nx^4 + ny^4 == 1` implies `g in [2^-1/4, 1]`), which is why a
  16-bit build measures 0.05 degrees of normal error. It is still not shipped: `q_pos`'s
  pixel magnitude makes `x^4` overflow binary16 unless the decomposition is restated in the
  normalised frame, and the payoff is bounded at ~14% of a kernel whose remainder is texture
  fetch. See Alternatives. This bullet used to say "the algebraic blocker is gone"; the
  measurement says the blocker is the field's representation, not its algebra.
- **`flux_sdf.glsl` is now the largest single GLSL file in the repository** and carries the
  gradient derivations inline. That is the intended trade: the derivations must sit next to
  the expressions they differentiate, or the next reader re-derives them and gets the
  smooth-union term wrong (the term everyone writes first is the extra one that turns out to
  be zero).
- **Four `Prism` materials and the canvas SDF shader did not get cheaper, and the first
  attempt made them worse.** They include the same header and pass `curvature == 0`, so the
  L4 arc is compiled out — but it was *also* compiled out before, because they call
  `flux_sdf_round_rect`, which never had an arc to remove. Routing them through a shared
  corner struct cost 30–54% more SPIR-V (measured: frosted 418 → 583, backdrop frost
  303 → 468) for arithmetic they discard. The shipped form keeps the field-only entry
  points expanding the decomposition into locals and confines the shared path to the
  derivative entries: frosted 421, acrylic 459, mica 553, backdrop frost 306, against 418 /
  456 / 550 / 303 — within 0.7% of before. The canvas SDF shader is the one real trade: 193
  → 216 instructions, in exchange for 6 `pow()` → 0 `sqrt`s on the 0.25 and no transcendental
  on the critical path.
- **`distance` is not shared between the field and derivative paths** — they are separate
  expressions whose agreement is asserted bit for bit by `test_sdf`, rather than one
  expression forced to carry both. glslang does not eliminate gradient arithmetic behind a
  run-time flag (verified: a literal-`false` flag still emits `Sin`/`Exp2`), so the
  alternative was paying for a gradient every coverage-only consumer discards.
- **The gradient path is a second definition of the field**, which `[INV-SDF-02]` normally
  forbids. It is admitted here under the invariant's own escape clause — "any other
  evaluation is a projection of that definition and is pinned by a differential test" — and
  `test_gradient_is_the_derivative` asserts `sample().distance == squircle()` exactly, over
  >10k pixels and five curvature values.

- **Never accept a hand-transcribed reference as a differential test.** The pre-existing
  `test_sdf` restated the shader expression in C and compared the host against *that*. It
  proved the host was self-consistent and nothing else: a change to `flux_sdf.glsl` could not
  fail it, so the "pinned by a differential test" claim in `[INV-SDF-02]` was prose. Two C
  copies of one expression drift exactly as readily as C and GLSL do.
- **Never tune a parity tolerance to observed output.** The interior crease ramp is
  `h = 0.5 + (q.x-q.y)/(2*aa)`, so float noise in the coordinate difference arrives in the
  normal amplified by `eps*extent/(2*aa)`. At `aa = 1e-3` over a 20-unit shape that is
  ~9.5e-3 — larger than any hand-picked constant would have admitted, and it is arithmetic
  conditioning, not a defect. The test derives the bound from that expression, which is why
  it can still fail on a real disagreement while surviving an ill-conditioned stress case.
- **Never describe a change as bit-identical when the expression changed.** This ADR
  originally claimed the render was "bit-stable"; the measured delta is 2/255. A different
  arithmetic expression producing different last bits is the expected outcome, and claiming
  otherwise misleads the next reader into treating a golden refresh as a regression.
- **Never "verify" a parity test by perturbing something that normalisation erases.** The
  first attempt to prove the harness could fail scaled a gradient by 1.01 *before*
  normalising it — a no-op that left the test green and looked like a passing check. The
  same applied to `>` → `>=` at the interior tie-break, which only differs on the exact
  diagonal the test already excludes. A perturbation must be shown to change the compared
  quantity, not merely to change the source: `g.y += 0.05` before normalise fails the test;
  `* 1.01` before normalise cannot.
- **Never compare a mode-1 device result against a mode-0 host result.** One silent ternary
  patch failed to apply, so the harness spent a round comparing the GPU's continuum gradient
  against the host's *shading* normal and reported the interior crease rule — the exact
  difference the two entry points exist to express — as a defect. A drift report whose
  values disagree by a designed 90° is a harness bug until proven otherwise.

- **Never compare a device readback without ordering the shader writes before the host
  read.** Host-coherent memory makes the *view* consistent; it does not order the GPU's
  stores before a host load. This harness read a mapped buffer with no
  `SHADER_STORAGE_WRITE -> HOST_READ` barrier and produced an occasional all-zero page — a
  1-in-40 flake under `MALLOC_PERTURB_` that first looked like a field disagreement in one
  shape. The barrier is required and was verified by removing it and re-measuring (1/40
  failures without, 0/40 with). A flaky parity test is worse than no parity test: it teaches
  the next reader to re-run failures instead of investigating them.
- **Never carry one interior rule in two copies inside one language.** `sdf.c` held the
  crease tie-break twice — once in the shared body for the shading normal, once copied
  verbatim into `flux_sdf_squircle_gradient`. That is the `[INV-SDF-02]` defect *inside* one
  language: a fix to the boundary predicate can land in one copy. `sdf_crease { CONTINUUM,
  SHADING }` is now a parameter on the single `sdf_squircle_eval`, and the three public entry
  points are one-line projections. Verified that the merge cost no detection power:
  perturbing the shared body fails `test_sdf` and `test_sdf_parity` independently.

## References

- [ADR-0118](0118-coverage-strategy-placement-distance-fields-and-curve-kernel.md) —
  `[INV-SDF-02]` (one definition per field), the placement rule this extends.
- [ADR-0046](0046-liquid-glass-convex-lens-model.md) — the convex-lens rim whose refraction,
  dispersion and rim light all consume this normal.
- [ADR-0112](0112-device-resolution-icon-coverage-masks.md) — the duplicated-definition
  postmortem that motivated single-owner fields.
- [ADR-0120](0120-gpu-analytic-coverage-tile-compute-plan.md) — analytic coverage over
  sample-counted coverage; the same "exact expression instead of sampled approximation"
  argument applied to coverage.
- [ADR-0063](0063-liquid-glass-material-library.md) — why the derivative lives in `flux` and
  not in a material shader.
- Prior art: Quilez, *analytic normals of 2D distance fields* (the gradient of a known field
  is a closed form); Inigo Quilez &loop smooth-minimum gradient identity.
