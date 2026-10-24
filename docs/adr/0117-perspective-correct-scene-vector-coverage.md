---
id: ADR-0117
title: "Perspective-correct vector coverage in the scene material system"
status: accepted
date: 2026-10-25
superseded_by: null
scope: libs/flux, bindings
negative_knowledge: true
---

# ADR-0117: Perspective-correct vector coverage in the scene material system

- Status: Accepted
- Date: 2026-10-25
- Scope: `libs/flux/src/scene/` (a new coverage material and its shaders),
  `libs/flux/include/flux/scene.h`, and consumers that place vector content in
  a 3D scene (`libs/lens` icon/mark producers, examples).
- Depends on [ADR-0115](0115-analytic-curve-kernel-single-geometry-authority.md)
  (the strip) and the `ANALYTIC_FRAGMENT` coverage math defined in
  [ADR-0116](0116-coverage-contract-canvas-rasterization.md).
- Amends [ADR-0010](0010-glyph-blit-primitive.md)'s atlas-only model for
  in-scene text and [ADR-0016](0016-pure-rhi-and-draw-primitives.md)'s module
  boundary (the shared kernel is what lets `scene` and `canvas` agree without
  seeing each other).
- Implementation: not started; tracked in
  [the implementation matrix](../dev/architecture-implementation-status.md).

## Context

`flux` has two different answers for vector content depending on where it is
drawn:

- **On a canvas (2D):** outlines become triangles (ADR-0115's authority),
  anti-aliased by a sample count (ADR-0016's subject).
- **In a scene (3D):** there is no vector answer at all. Text and glyphs go
  through the glyph-atlas blit primitive (ADR-0010) — an R8 coverage atlas
  sampled as a textured quad — and any other vector artwork has to be
  rasterised on the CPU and uploaded as an image.

An atlas or an uploaded image is a **screen-resolution bake**. It is exactly
the artifact ADR-0112 fought in 2D, with worse consequences in 3D:

- **Under magnification** (a label moved toward the camera, or a UI zoomed in
  perspective) the bake blurs: there is no more resolution in the texture than
  was baked, and bilinear filtering cannot invent edge detail.
- **Under minification** (a large sign receding) the bake aliases: a single
  non-mipmapped bake cannot be filtered correctly, and mipmaps of a coverage
  mask destroy thin strokes.
- **Under an animated transform** the bake is either re-generated per frame
  (a CPU rasterisation + upload per node per frame — the cost ADR-0112 measured
  at 3–4 ms cold) or reused and visibly stale.
- **Under perspective** a texture mapped onto an affine quad has no way to
  carry the correct screen-space coverage; edge crispness varies with depth
  across a single primitive.

The result is that the one place curves are most visible — a word or an icon
rendered *in* a scene, at whatever scale and orientation the scene puts it — is
the one place `flux` cannot render a curve. Downstream, `lens` icons are
authored as 24×24 SVG command streams and, when a host wants them in a scene,
must be baked.

The mathematics needed to fix this exists and is the `ANALYTIC_FRAGMENT`
strategy of ADR-0116: reconstruct a primitive's curve coverage in screen space
by intersecting the pixel with the curve. Doing so *under an arbitrary
object→clip transform* is the specific contribution the Slug-class technique
makes: the curve is carried in object space (not interpolated per vertex, which
cannot represent a projective transform correctly) and the fragment stage
reconstructs screen-space control points and the coverage width from the
transform and its screen-space Jacobian.

## Decision

**The scene gains a vector coverage material.** It renders `flux_curve_cover`
strips under an arbitrary object→clip transform with analytic, resolution-
independent coverage, and with no bake.

1. **Object-space curves, screen-space coverage.** The strip (control points,
   clip spans, bounds) is supplied in **object space** and indexed per
   primitive — not interpolated per vertex. The fragment stage reconstructs the
   screen-space curve by transforming the control points with the material's
   object→clip matrix and its screen-space Jacobian, then computes coverage by
   quadratic root-finding against the pixel. This is precisely why per-vertex
   interpolation is rejected (below).

2. **Perspective is a correctness contract.** Coverage must be correct under
   non-uniform scale, shear, and perspective foreshortening, and must be stable
   frame to frame under an animated transform (no popping as the matrix
   changes). A primitive whose coverage is only correct for affine transforms
   is not acceptable, because the scene is where projective transforms live.

3. **One coverage math, one strip.** The material consumes the same strip and
   the same quadratic coverage formulation as the canvas's `ANALYTIC_FRAGMENT`
   backend; it differs only in that its transform is a 4×4 object→clip matrix
   plus perspective divide, rather than a 2D screen transform. There is no
   second vector semantics.

4. **It is a scene material, and it obeys the module rule.** The material lives
   in `scene` and depends on `curve` (a leaf, like `math`) and `core` — never
   on `canvas`. This is achievable *because* the strip is a neutral value
   (ADR-0115): `scene` and `canvas` agree on curve geometry without either
   seeing the other, satisfying ADR-0016.

5. **No multisample requirement.** Coverage is analytic; the material works on
   a 1× scene target. Any MSAA a scene producer wants remains that producer's
   choice and attachment, not a requirement of this material.

6. **Text is a consumer.** In-scene text supplies glyph outlines as strips to
   this material. The glyph atlas remains for on-canvas text at fixed scale;
   it stops being the mechanism for text that must survive magnification,
   minification, or an animated transform.

### Invariants & Behavioral Boundaries

- **INV-SCV-01 — No bake.** In-scene vector coverage must not depend on a
  pre-rasterised texture, atlas, or SDF sampled by screen-space UV. A returned
  quality that changes with camera distance means a bake was reintroduced.
- **INV-SCV-02 — Object-space input.** Curve control points cross into the
  material in object space; the material must not require a per-frame CPU
  transform of control points (which would defeat the resolution independence
  under animated transforms).
- **INV-SCV-03 — Projective correctness.** Coverage is asserted correct under a
  test set that includes non-uniform scale, shear, and perspective
  foreshortening, at multiple depths within a single primitive.
- **INV-SCV-04 — Shared semantics.** The material evaluates the same coverage
  contract as ADR-0116's `ANALYTIC_FRAGMENT`; under an affine transform the two
  must agree within the coverage tolerance.
- **INV-SCV-05 — Module isolation.** `scene` must not include any `canvas`
  header; the shared currency is `curve` (ADR-0016).

## Rejected Alternatives & Negative Knowledge

- **Option A — Keep the glyph atlas / uploaded image for in-scene vector.**
  *Why considered:* it exists and works at a fixed scale. *Why rejected:* it is
  a resolution bake. It blurs under magnification, aliases under minification,
  goes stale or costs a per-frame CPU rasterisation under an animated
  transform, and cannot carry correct coverage under perspective. This is the
  ADR-0112 failure mode, in a setting where it is unavoidable rather than
  occasional.
- **Option B — Signed distance field (SDF/MSDF) textures for in-scene vector.**
  *Why considered:* excellent magnification quality, cheap sampling, no MSAA.
  *Why rejected:* it is a baked *approximation* of the outline, not the outline.
  Minification aliases (a distance field is not linearly mipmappable in a way
  that preserves thin strokes), sharp corners and cusps are lost at the
  resolution of the field, and the memory/hierarchy cost grows with fidelity.
  The "uncompromising" constraint — crisp curves at any scale and any
  perspective — excludes an approximation at the outset. It also cannot be
  made correct for a primitive whose coverage varies with depth (INV-SCV-03).
- **Option C — Tessellate on the CPU and draw triangles in the scene**
  (reuse the canvas path). *Why considered:* reuses existing code. *Why
  rejected:* this is precisely the legacy burden ADR-0115 removes; it imports
  the entire CPU geometry machine into the 3D path, inherits the polyline and
  offset artifacts, needs a re-tessellation per transform change (the same
  staleness as a bake), and still cannot express perspective-correct coverage
  across a primitive.
- **Option D — Transform control points on the CPU per draw and rasterise
  affinely.** *Why considered:* simplest way to reuse the 2D analytic fragment
  backend; the transform is already known on the CPU. *Why rejected:* it
  reintroduces per-frame CPU work proportional to geometry (the cost ADR-0115
  exists to remove), and it is wrong under perspective — a projective transform
  is not affine, so a control point transformed at the vertices and interpolated
  does not lie on the transformed curve (INV-SCV-02).
- **Option E — Loop-Blinn implicit-form fragment shader.**
  *Why considered:* elegant, exact for a single curve, one pass, well known.
  *Why rejected for this use:* the implicit form is reconstructed from the
  control points and is fragile under strong perspective foreshortening and
  non-uniform scale, and the classic formulation relies on multisampling for
  anti-aliasing (a sample-count dependency this ADR is specifically removing).
  The quadratic ray-cast formulation used here is robust under perspective and
  yields analytic coverage directly — the reason the Slug-class technique
  exists as a step beyond Loop-Blinn.
- **Option F — Render curves to a texture once per transform change, then
  sample.** *Why considered:* decouples coverage cost from draw count.
  *Why rejected:* it is a bake with a cache key of the transform; an animated
  transform invalidates it every frame, so it degrades to per-frame texture
  generation with a readback-or-bind lifecycle, and it reintroduces the ADR-0112
  "cache beside the live path" drift.

## Consequences

Positive:

- Vector content in a scene is crisp at any scale, orientation, and depth,
  including an animated camera — the case no bake can serve.
- In-scene text and icons no longer need CPU rasterisation or an upload; a
  scene host can place the same `lens` icon/SVG outline data it places on a
  canvas, because both consume `flux_curve_cover`.
- The geometry cost is paid once (the strip) and the coverage cost is
  per-pixel on the GPU, so scene complexity scales with visible coverage, not
  with re-baked resolution.
- Correctness under perspective is a stated, tested contract rather than an
  emergent property of the flattening density.

Negative & trade-offs:

- A new material, its pipeline variants, and their shader surface are added to
  `scene`; per-primitive control-point storage becomes a resource with the
  usual lifetime and budget obligations (ADR-0090/0092).
- Coverage depends on the transform's Jacobian, so a degenerate or
  extreme-ratio transform must be handled explicitly (an ill-conditioned
  Jacobian is an error, not a silent smear).
- Thin strokes and sub-pixel features are genuinely hard under minification;
  the material must state and enforce a minimum coverage behaviour rather than
  pretend aliasing is solved by sampling.
- Rust bindings and any `scene` consumer that wants the material migrate in the
  same change (ADR-0095).

## Verification

- **Projective area reference.** A test set of transforms — identity, uniform
  scale, non-uniform scale, shear, rotation, perspective with several focal
  ratios and depths — rasterised against a 1024× supersampled reference; signed
  coverage error bounded per transform, including within a single primitive at
  multiple depths (INV-SCV-03).
- **Animated-transform stability.** A frame sequence applying a continuously
  changing projective matrix to a fixed strip must show no coverage popping:
  bounded frame-to-frame coverage delta away from imposed motion.
- **Cross-path agreement.** Under an affine transform the material's output
  agrees with ADR-0116's `ANALYTIC_FRAGMENT` canvas backend within the coverage
  tolerance (INV-SCV-04) — this is the test that there is one semantics, not
  two.
- **No-bake gate.** A structural assertion that no texture/atlas/SDF is
  sampled for the material's coverage (INV-SCV-01), and that quality does not
  degrade with camera distance.
- **Module gate.** An include-graph assertion that `scene` does not depend on
  `canvas` (INV-SCV-05).
- **Magnification/minification corpus.** A stress corpus (a glyph, a 12-px
  icon, a hairline stroke) rendered across a range of scales and perspectives,
  compared against the area reference, to make the "any scale, any perspective"
  claim falsifiable.

## References

- [ADR-0115](0115-analytic-curve-kernel-single-geometry-authority.md) — the
  strip, and why the primitive is a neutral value.
- [ADR-0116](0116-coverage-contract-canvas-rasterization.md) — the coverage
  contract this material shares with the canvas.
- [ADR-0010](0010-glyph-blit-primitive.md) — the atlas model amended for
  in-scene text.
- [ADR-0016](0016-pure-rhi-and-draw-primitives.md) — module boundary preserved
  via the shared leaf kernel.
- [ADR-0112](0112-device-resolution-icon-coverage-masks.md) — the 2D bake whose
  failure mode motivated removing the 3D bake.
- [ADR-0090](0090-display-list-resource-ownership.md),
  [ADR-0092](0092-resource-planning-and-retirement.md),
  [ADR-0095](0095-flux-public-contract-convergence.md) — resource ownership,
  synchronization, and explicit-failure contracts.
- Prior art: Slug (Lengyel) — quadratic-Bézier GPU text "at any scale or from
  any perspective"; Loop & Blinn (2005) implicit curve rendering; Vello /
  Sparse Strips for the shared coverage model.
- Implementation: `libs/flux/src/scene/` (coverage material + shaders),
  `libs/flux/include/flux/scene.h`.
