---
id: ADR-0118
title: "Coverage strategy placement — closed-form distance fields as a peer of the curve kernel"
status: accepted
date: 2026-10-25
superseded_by: null
scope: libs/flux, libs/prism, libs/lens
negative_knowledge: true
---

# ADR-0118: Coverage strategy placement — closed-form distance fields as a peer of the curve kernel

- Status: Accepted
- Date: 2026-10-25
- Scope: new public surface `libs/flux/include/flux/sdf.h` and implementation
  `libs/flux/src/sdf/`; consumers `libs/flux/src/canvas/` (dispatch, SDF
  pipeline, CPU host port), `libs/prism/src/shaders/`, `libs/lens`.
- Amends [ADR-0112](0112-device-resolution-icon-coverage-masks.md) (the coverage
  *bake* is reclassified from "removed" to "deposited, explicit opt-in").
- Refines [ADR-0116](0116-coverage-contract-canvas-rasterization.md) (the
  analytic branch is named as a peer strategy of the one coverage contract).
- Depends on [ADR-0083](0083-flux-orthogonal-shape-paint-model-and-c23-baseline.md)
  (the geometry classes) and [ADR-0115](0115-analytic-curve-kernel-single-geometry-authority.md)
  (the curve kernel it is the peer of).
- Implementation: **primitives landed** (`flux_sdf_circle`,
  `flux_sdf_round_rect`, `flux_sdf_squircle`, `flux_sdf_ring`,
  `flux_sdf_coverage`, tested by `tests/flux/unit/test_sdf.c`), the **CPU
  backend host port consumes them**, the **GLSL definition is consolidated**
  into one `libs/flux/include/flux/shaders/flux_sdf.glsl` included by
  `canvas_sdf.frag`, `canvas_image.frag` and the five Prism materials, the
  **squircle has one shape** (`flux_path_add_squircle` fits the field's corner
  arc; INV-SDF-06 closed), and **`FLUX_GEOM_SQUIRCLE` now takes the analytic
  path** in `canvas.c`'s dispatch (INV-SDF-03). Non-uniform transforms on
  rounded rects and circles still fall back to tessellation, and the coverage
  bake is not yet exposed as an explicit opt-in; both are tracked in
  [the implementation matrix](../dev/architecture-implementation-status.md).

## Context

ADR-0115 through ADR-0117 name the *curve* path — reduce, offset, wind,
rasterize analytic curve coverage — but never place the **distance-field** path
that has shipped all along. Read on its own, that line looks like the analytic
branch is being folded away. It is not, and the two are not rivals:

| Geometry | Closed-form distance? | Correct strategy |
|---|---|---|
| circle, ellipse, box, rounded rect | yes | distance field — exact |
| superellipse / squircle | yes (with an exponent parametrisation) | distance field |
| arbitrary Bézier path, glyph outline | **no** | curve kernel |

A curved path has no closed-form distance, which is why the kernel exists. An
analytic primitive *does*, which is why the field exists. Naming only one of
them is an omission in the architecture record, not a decision.

Three facts in the current tree show the omission has a cost:

1. **The canvas's analytic branch is under-deployed.** `canvas.c` routes only
   `FLUX_GEOM_RRECT` and `FLUX_GEOM_CIRCLE` to the SDF pipeline, and only when
   the paint is solid, the blend is SRC_OVER and the transform is axis-uniform
   (`canvas.c:1238`, `canvas.c:1268`). `FLUX_GEOM_SQUIRCLE` — analytically
   expressible, and already expressible *in this tree* — is always converted to
   cubics and pushed through flattening, tessellation and sampled
   anti-aliasing (`canvas.c:1256`). A rotated rounded rect loses its analytic
   field entirely and falls back to the same path.
2. **The field is defined more than once.** The rounded-rect expression existed
   as `canvas_sdf.frag`, again as the host port in `backend_cpu.c`, again in
   four Prism materials (`frosted`, `acrylic`, `mica`, `backdrop_frost` — byte
   identical), again inline in `canvas_image.frag`, and a squircle variant in
   `liquid_glass.comp`. Eight sites, one idea. That is exactly the
   duplicated-definition drift ADR-0112's postmortem identifies as the way a
   fixed bug comes back.
3. **"Squircle" meant two things, and now means one.** The corner arc's *field*
   is `flux_sdf_squircle` (`curvature` 0 = circular arc, 1 = L4 superellipse),
   and `flux_path_add_squircle` now draws **that arc**: two cubics per corner,
   fitted to the field's zero set. The tiebreaker is not taste — **a distance
   field can only serve a shape with a closed-form corner**, and the previous
   path used an Apple-style construction (extending the corner transition to
   `L = r·(1 + 0.52c)` and skewing the control tensions) whose corner has no
   field. Since the analytic branch is the primary consumer, the field defines
   the shape and the path follows it. The corner radius is therefore `radius`
   for every curvature, and curvature reshapes the arc on that radius instead
   of enlarging the corner.
4. **A *baked* field is a third thing, and it was being conflated with the
   first two.** An SDF/MSDF texture and a coverage mask are approximations
   whose quality is bounded by a precomputed resolution. ADR-0117 rejects them
   for in-scene vector content; ADR-0116 supersedes ADR-0112's icon mask bake.
   Neither of those statements is a judgement about *closed-form* fields, but
   neither says so either.

## Decision

**Distance fields and the curve kernel are two first-class, peer coverage
algorithms, one per geometry representation. A bake is a deposited, explicit
opt-in — never a default and never a silent fallback.**

1. **Two representations, two peers.** `<flux/sdf.h>` / `libs/flux/src/sdf/`
   sit at the same level as `<flux/curve.h>` / `libs/flux/src/curve/`. Both are
   leaf modules, public value surfaces, device- and backend-free.

2. **One definition per field.** The distance expressions live in `flux_sdf_*`.
   Backends and shaders evaluate *that* definition; a shader may keep a local
   evaluation of the same expression, but the parity is pinned by
   `tests/flux/unit/test_sdf.c` (a differential check against the shader's
   expression), so a change to either side trips the test. The CPU host port
   consumes the primitive directly.

3. **Strategy follows representation, not convenience.** A shape whose class
   admits a closed-form distance takes the analytic path whenever its paint,
   blend and transform permit. Falling back to tessellation for a
   *representable* shape is a defect to fix, not a tolerated path. The squircle
   is the named instance; a non-uniform (rotated/sheared) transform on a
   rounded rect is the second.

4. **A bake is deposited, not deleted, and never implicit.** A distance-field
   texture, a coverage mask, an atlas — any strategy whose quality is bounded
   by a precomputed resolution — is retained as a *named* strategy with its
   failure modes recorded (resolution-bounded edges, corner rounding,
   minification aliasing, invalidation on scale/transform change), and is
   selected only when explicitly requested by the caller. It must never be
   reached by falling through from the exact path (ADR-0116 INV-COV-04,
   ADR-0095).

5. **Exactness is always obtainable.** For every geometry class that admits a
   closed-form distance, a caller can obtain exact analytic coverage. No
   optimisation may silently replace it with an approximation.

### Invariants & Behavioral Boundaries

- **INV-SDF-01 — Peer, not fallback.** The analytic branch is a peer strategy
  of the one coverage contract. It is never described, encoded, or scheduled as
  a degraded substitute for curve coverage, and curve coverage is never a
  substitute for it.
- **INV-SDF-02 — One definition per field.** A distance field is defined once,
  in `flux_sdf_*`. Any other evaluation is a projection of that definition and
  is pinned by a differential test.
- **INV-SDF-03 — Representation picks the strategy.** Analytic classes take the
  analytic path whenever paint/blend/transform allow, and "allow" means the
  transform is a *similarity* — rotation and uniform scale preserve distance, so
  the field is exact there; shear and anisotropic scale do not, so they
  tessellate rather than approximate.
- **INV-SDF-04 — A bake is opt-in and loud.** No implicit transition from exact
  to baked coverage; requesting a bake is an explicit API act.
- **INV-SDF-05 — Leaf module.** `sdf` depends only on `math`/`core`; no device,
  backend, or raster type (same rule as INV-CRV-02).
- **INV-SDF-06 — One shape, one definition.** Where two subsystems name the
  same shape, they must agree by construction and the agreement must be
  testable. `flux_sdf_squircle` is the squircle; `flux_path_add_squircle` is an
  approximation of its zero set, and `test_canvas_squircle` asserts the
  approximation stays inside a stated bound (measured worst case 0.296 % of the
  corner radius, bound 1 %). A second, unreconciled definition is a defect, not
  a licence for two answers.

## Rejected Alternatives & Negative Knowledge

- **Option A — Unify everything on the curve kernel; convert analytic
  primitives to curves.** *Why considered:* one algorithm, one module, less to
  explain. *Why rejected:* it **discards exactness**. A rounded rect becomes
  cubics, is flattened to a polyline, tessellated, and anti-aliased by sampling
  — losing the exact distance, the exact border width, and the analytic stroke
  ring, and paying flattening + tessellation for a shape with a closed form.
  This is a quality regression dressed as simplification.
- **Option B — Unify everything on distance fields, fitting or baking the
  Bézier case.** *Why considered:* symmetry with Option A. *Why rejected:* the
  distance to a cubic has no closed form; a fit or a bake is an approximation.
  ADR-0117 rejects it for in-scene vector content, and INV-SDF-05 rejects it
  here for the same reason: a fit's error is bounded by the fit, not by the
  geometry.
- **Option C — Keep the four copies of the expression.** *Why considered:*
  zero churn; each shader is self-contained. *Why rejected:* duplicated
  definitions drift silently. ADR-0112's postmortem is exactly this failure —
  a helper and its inlined twin computed different halos. One definition plus a
  parity test is the structural fix.
- **Option D — Delete the SDF branch now that curve coverage is being built.**
  *Why considered:* reads as "one coverage algorithm is simpler than two".
  *Why rejected:* they cover disjoint domains. Deleting the analytic branch
  would make every rounded rect and circle pay flatten + tessellate + sampled
  AA, and would break the analytic border and analytic stroke ring that
  `canvas_sdf.frag` implements today. Removing an exact method to make the
  architecture look uniform is the wrong trade.
- **Option E — Make the bake a silent fallback for "expensive" geometry.**
  *Why considered:* it is the cheapest way to bound worst-case cost.
  *Why rejected:* a fallback that changes quality without saying so is the
  defect, not the remedy (ADR-0112, INV-COV-04). If a bake is wanted, the
  caller asks for it.

## Consequences

Positive:

- The two representations are named, so "which algorithm covers this shape" has
  a written answer, and the analytic branch stops looking like an accident of
  `canvas.c`.
- Rounded rects and circles keep exact coverage, exact borders and analytic
  stroking; the shared primitive makes the host port and the shader provably
  the same function.
- The known under-deployment (squircle, non-uniform transforms) becomes a
  tracked defect with a stated fix rather than an unexamined special case.
- The bake's status is unambiguous: available, documented, opt-in, with its
  failure modes on record — so it is neither silently reachable nor lost.

Negative & trade-offs:

- Two public modules with parallel structure (`curve`, `sdf`) is more surface
  than one; justified because they answer different questions.
- Consolidating the shader copies was real work and required declaring the
  snippet as a build dependency, since a shader include that is not tracked
  leaves stale SPIR-V behind.
- Routing the squircle analytically has landed (INV-SDF-03): a solid,
  axis-aligned, SRC_OVER `FLUX_GEOM_SQUIRCLE` no longer flattens, tessellates or
  samples — it evaluates the field. Non-uniform (rotated/sheared) transforms on
  rounded rects and squircles still fall back to tessellation.
- **Analytic coverage carries a small systematic surplus** against sampled
  coverage: `clamp(0.5 − d, 0, 1)` ramps across the boundary from half a pixel
  inside to half a pixel outside, while a triangle rasteriser classifies whole
  samples. Measured at 1.2–1.4 % of ink mass on a 96×72 shape, matching the
  predicted (perimeter × 0.25 px). Both are correct; code comparing the two must
  expect the offset rather than treat it as an error.
- **A shape change, deliberately taken.** `flux_path_add_squircle`'s corner
  radius is now `radius` for every curvature (the arc reshapes in place)
  instead of growing to `radius·(1 + 0.52c)`. This is required for the field to
  describe the drawn shape, and the only in-tree consumer is a showcase example
  whose comments name the old model; downstream artwork drawn against the old
  geometry will look tighter at high curvature.
- `flux_sdf_*` values are float like the rest of the public math surface; a
  caller working in double (SPIR-V `float64`, compute) projects, which is the
  same limitation the canvas already has.

## Verification

Landed in `tests/flux/unit/test_sdf.c` (31056 assertions): sign and magnitude
at analytically known points (circle centre/edge/outside, rounded-rect centre,
edge midpoints, the cut corner at `r·(√2−1)`, plain box); `flux_sdf_ring`
including the zero-width identity; `flux_sdf_coverage` monotonicity, saturation
and the degenerate-`aa` hard edge; the **Eikonal property** `|∇d| = 1` across
the edge region (the property that licenses `coverage = 0.5 − d`), probed away
from the axes and the medial axis where a central difference is not a
derivative; **differential parity with the canvas SDF shader's expression**
over a 121×121 grid including the border path; and finiteness on degenerate
input.

Consumer evidence: `backend_cpu.c`'s `frag_sdf` calls the shared primitives, and
the whole canvas suite (`canvas_consistency` GPU↔CPU pixel oracle,
`canvas_coverage`, `canvas_shapes`, `canvas_render`) passes unmodified. This
paragraph was **wrong when first written**: the host-port edit had silently not
been applied, so the file still held a private copy of the formula while this
record claimed otherwise. Nothing caught it because `test_sdf`'s parity check
compares the *shader's* expression against the primitive, not the backend's
evaluation. `test_canvas_squircle`'s dispatch test now closes that gap by
drawing the same shape twice through the CPU canvas — once analytically, once
by tessellation — and comparing the results.

Consolidation evidence: the round-rectangle field now has exactly **one**
definition per language — `libs/flux/include/flux/shaders/flux_sdf.glsl` on the
GLSL side (included by `canvas_sdf.frag`, `canvas_image.frag` and the five
Prism materials) and `flux_sdf_round_rect` on the host side. Six GLSL copies
and one inline copy became one file; the host port had already been converged.
Prism's materials keep a four-line *convention adapter* (`bounds` = (x, y, w, h)
with a non-zero half-extent floor) and `liquid_glass.comp` keeps its L4 blend,
but neither restates the field. A grep for the field body now returns the
shared snippet and nothing else. Every shader still compiles
(`glslangValidator` at build time, with the snippet declared as a `depend_files`
input so changing it rebuilds the SPIR-V instead of leaving a stale copy), and
the full suite — including the Prism material goldens — passes unmodified, i.e.
the unification is numerically identical.

Squircle evidence: `flux_sdf_squircle` has exactly one definition per language
too, and `liquid_glass.comp` now delegates to it (its field body is gone).
`test_sdf` asserts three things: that `curvature == 0` reproduces
`flux_sdf_round_rect` **bit-for-bit** (so the family contains the exact field as
its degenerate case rather than approximating it), that the field matches the
expression Prism shipped over a 81×81 grid at five curvatures, and that raising
curvature pushes the corner arc outward while leaving the straight edges
untouched. Prism's rendered output is unchanged (its `curvature` semantics are
preserved exactly; no in-tree test or example output moves).

Dispatch evidence: `test_canvas_squircle` draws the same squircle twice through
the CPU canvas — once as `FLUX_GEOM_SQUIRCLE`, which now takes the analytic SDF
path, and once as the equivalent `flux_path`, which goes through flatten +
tessellate + sampled AA — at curvature 0, 0.5 and 1. **No pixel is emphatically
on the wrong side** (inside-vs-outside agreement is exact), both paths
anti-alias, and ink mass agrees within the ~1.3% systematic surplus explained
next. That test is what turns "the host port consumes the shared primitive"
from an assertion into a measurement.

Transform evidence: the same comparison runs under a rigid rotation, and under
rotation composed with a 1.35× uniform scale about an off-centre pivot. Both
report `disagree == 0`, so the shape-space field is exact for similarities
rather than merely close. A sheared case is included as the negative control:
there the two drawings are **bit-identical**, which is the expected signature of
both taking the tessellation path — an approximated field would have differed.
Finally, `test_canvas_consistency`'s GPU↔CPU scene now includes a rotated
rounded rect and a rotated squircle, so the *device* shader's shape-space
mapping and its inverse-linear push constants are compared against the oracle on
real hardware (lavapipe in CI), not merely compiled.

**Finishing INV-SDF-06.** `flux_path_add_squircle` now draws the field's corner
arc as two cubics per corner. The numerics were settled before the code:

- One cubic per corner cannot carry it: a minimax fit leaves **3.1 % of `r`** of
  deviation at `n = 4`, against the 0.027 % that the classic `4/3·tan(θ/4)`
  gives a circular arc.
- Two cubics per corner suffice. The offsets come from **Schneider's algorithm**
  (least squares alternated with geometric reparameterisation), tabulated at
  nine curvatures and linearly interpolated. Two properties make the table
  trustworthy rather than merely plausible: at curvature 0 it recovers the
  classically known two-cubic circle constant to five decimals (0.2652022 vs
  0.265216), which validates the algorithm itself; and the sequence is smooth
  and monotone, unlike the ad-hoc optimiser tried first (which reproduced the
  circle constant yet returned non-monotone, contradictory values for
  `n ∈ (2,4]` — numbers nobody could defend).
- The **diagonal radius is exactly linear** in curvature (verified: identical
  increments to seven decimals, ending at `2^(1/4) = 1.1892071`), so it is
  computed rather than tabulated.
- `test_canvas_squircle` (66 assertions) asserts the drawn path lies on the
  field's zero set: measured worst case **0.296 % of the corner radius**
  (0.095 px at r = 32), against a 1 % bound. It also pins endpoint continuity
  (curvature 0 is the rounded rectangle), the diagonal bulge against the
  field's diagonal radius at nine curvatures (agreeing to 4 decimals), and that
  the straight edges are untouched.

One residual is recorded rather than hidden: a cubic fitted to a convex arc can
leave the shape's bounding box by a fraction of a pixel — measured 0.075 px at
curvature 1, because one control point sits about 1 px outside, and the test
allows 0.5 px. This is benign for rendering (bounds are inflated for
anti-aliasing) but it is a real property of a cubic approximation.

## References

- [ADR-0115](0115-analytic-curve-kernel-single-geometry-authority.md) — the
  curve kernel this is the peer of.
- [ADR-0116](0116-coverage-contract-canvas-rasterization.md) — the one coverage
  contract both strategies feed; INV-COV-04 (no silent degradation).
- [ADR-0112](0112-device-resolution-icon-coverage-masks.md) — the bake,
  reclassified here from "removed" to "deposited opt-in"; its postmortem is the
  reason for INV-SDF-02.
- [ADR-0117](0117-perspective-correct-scene-vector-coverage.md) — why a *baked*
  field is rejected for in-scene vector content.
- [ADR-0083](0083-flux-orthogonal-shape-paint-model-and-c23-baseline.md) — the
  geometry classes the dispatch switches on.
- Prior art: the classic rounded-box field (Quilez), distance-function
  literature; MSDF (Chlumský) for the baked case; Slug/Vello for the curve case.
- Implementation: `libs/flux/src/sdf/sdf.c`, `libs/flux/include/flux/sdf.h`,
  `libs/flux/include/flux/shaders/flux_sdf.glsl`,
  `libs/flux/src/canvas/backend_cpu.c` (`frag_sdf`),
  `libs/flux/src/canvas/shaders/canvas_sdf.frag`, the Prism material shaders.ial shaders.as_sdf.frag`, the Prism material shaders.ial shaders.