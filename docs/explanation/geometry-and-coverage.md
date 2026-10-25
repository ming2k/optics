# Geometry representations and coverage algorithms

This document is the map of how 2D geometry in this repository becomes pixels:
which representation each shape uses, which algorithm turns that representation
into coverage, and where each algorithm lives. It exists so that "which
algorithm covers this shape" has a written answer instead of being inferred
from a dispatch switch.

The governing decision is [ADR-0118](../adr/0118-coverage-strategy-placement-distance-fields-and-curve-kernel.md).

## The two representations

A 2D renderer serves two kinds of shape, and they need two different algorithms
because of one mathematical fact — whether the shape has a **closed-form
distance to its boundary**:

| Representation | Closed-form distance? | Correct algorithm |
|---|---|---|
| Analytic primitive: box, rounded rect, circle, ellipse | **yes** | distance field — exact, resolution-independent |
| Parametric curve: Bézier path, glyph outline, squircle-as-cubic | **no** | curve kernel — reduce, offset, wind |

Neither is a fallback for the other. A third technique — a **baked** field,
mask or atlas — is an approximation bounded by a precomputed resolution. It is
available, documented, and opt-in, and it is never reached by falling through
from an exact path.

## Algorithm inventory

| Stage | Algorithm | Location |
|---|---|---|
| Curve algebra (kernel) | Cubic → quadratic reduction with an exact error bound `|E|/(6·2√3)`; closed-form inflection and cusp splitting; Wang segment count; analytic parallel offset; closed-form retrograde interval; ray crossings and signed winding | `libs/flux/include/flux/curve.h`, `libs/flux/src/curve/curve.c` |
| Path → polyline bridge | Uniform sampling at the kernel's Wang count (no recursion, no flatness search) | `libs/flux/src/canvas/curve_flatten.c` |
| Fill tessellation | Ear clipping with hole bridging, convex fan fast path, incremental reflex candidate list, LRU tessellation cache, stencil-then-cover fallback for non-simple input | `libs/flux/src/canvas/geometry_tess.c` |
| Stroke expansion | Per-vertex mitre frames; inner offset clamped at the local radius of curvature; miter/bevel/round joins; butt/square/round caps | `libs/flux/src/canvas/geometry_stroke.c` |
| Path building | `MOVE`/`LINE`/`QUAD`/`CUBIC`/`CLOSE` verbs; rect / round-rect / circle constructors; squircle constructed as two cubics per corner fitted to `flux_sdf_squircle`'s zero set | `libs/flux/src/canvas/path.c` |
| Distance fields (primitives) | Circle, rounded rect, squircle (circular↔L4 corner blend), border ring, analytic coverage | `libs/flux/include/flux/sdf.h`, `libs/flux/src/sdf/sdf.c` |
| Distance field (device) | Rounded-rect field, border ring adjustment, one-pixel analytic coverage | `libs/flux/src/canvas/shaders/canvas_sdf.frag` |
| Distance field (host) | The same field via the shared primitive | `libs/flux/src/canvas/backend_cpu.c` (`frag_sdf`) |
| Strategy dispatch | Which geometry class takes the analytic path and under which paint/blend/transform. Rounded rect, circle and squircle are analytic when solid + SRC_OVER + axis-uniform | `libs/flux/src/canvas/canvas.c` (`canvas_draw_geometry`, `draw_sdf_shape`) |
| Material distance fields | Rounded-rect field via the shared snippet; squircle field local to the material | `libs/flux/include/flux/shaders/flux_sdf.glsl` (included by `prism/src/shaders/frosted.comp`, `liquid_glass.comp`, `acrylic.comp`, `mica.comp`, `backdrop_frost.comp`) |
| Baked coverage (opt-in) | Device-resolution icon coverage mask, supersampled at bake time | `libs/lens/src/render/icon_mask.c` |
| Glyph coverage | FreeType-rasterised coverage atlas (host-resident or device) | `libs/glyph/src/atlas.c` |
| Icon vector source | SVG path parsing; elliptical arcs converted to cubic Béziers | `libs/lens/src/icon_svg.c` |

## The curve pipeline, in order

1. **Build.** A caller appends `MOVE`/`LINE`/`QUAD`/`CUBIC`/`CLOSE` verbs, or a
   constructor emits them: `flux_path_add_round_rect` and
   `flux_path_add_circle` use the standard `k ≈ 0.5522847498` circular-arc
   control offset; `flux_path_add_squircle` uses a continuous-curvature
   approximation.
2. **Flatten.** `curve_flatten.c` computes each curve's segment count from the
   kernel's Wang formula and samples uniformly, in object space, against a
   screen-space tolerance divided by the transform's operator norm.
3. **Fill.** `geometry_tess.c` ear-clips the closed contour, bridging true holes
   and taking a convex fan fast path where it can; non-simple input routes to
   the stencil-then-cover fallback rather than producing wrong geometry.
4. **Stroke.** `geometry_stroke.c` offsets the polyline, mitring at vertices and
   emitting join and cap geometry where the mitre is clamped. Where the
   half-width exceeds the local radius of curvature the inner offset is clamped
   so the band cannot overlap itself.
5. **Rasterise.** Triangles go to the GPU pipeline or to the software backend,
   anti-aliased by the pass's sample-count policy.

## The analytic branch

Solid **and gradient** path fills also take the analytic branch (ADR-0119
S2 plus its gradient extension): the backend's optional `submit_cover` op receives
the flattened contours in physical pixels together with the draw's brush, returns
per-pixel coverage, and evaluates the brush (flat colour or gradient) per pixel —
so a fill needs neither ear clipping nor a stencil attachment for either fill
rule. Images, glyph runs, strokes and non-SRC_OVER blends still take steps 2–5
below.

Analytic primitives bypass steps 2–4 entirely: the fragment stage maps the
fragment back into the **shape's own space** (through the inverse of the
transform's linear part) and evaluates the closed-form distance there, then
derives coverage as `clamp(0.5 − d / aa, 0, 1)` where `aa` is one screen pixel
expressed in shape units. Because the field is Eikonal (`|∇d| = 1`) away from the
medial axis, that expression is a correct one-pixel anti-aliasing rule; because
the mapping is exact for any similarity, rotation and uniform scale need no
special case. Shear and anisotropic scale distort distance and so keep
tessellating.

The host backend evaluates the same primitives from `<flux/sdf.h>`, so host and
device coverage agree by construction; `test_sdf` additionally pins the
primitive against the shader's expression so the two cannot drift.

## What is not yet done

Recorded so the gaps are visible rather than inferred:

- **Squircle.** One shape, one path, one dispatch: the field defines the corner
  arc, `flux_path_add_squircle` fits it, and `FLUX_GEOM_SQUIRCLE` (solid,
  SRC_OVER, similarity transform) evaluates the field instead of tessellating.
- **Shear and anisotropic scale.** The field is evaluated in shape space, which
  is exact for rotation and uniform scale (a similarity preserves distance).
  Shear and anisotropic scale distort distance, so those still fall back to
  tessellation rather than being drawn with an approximated field.
- **Stroke on curves.** The stroker offsets a flattened polyline, so its
  curvature is a per-vertex estimate rather than the kernel's closed form.
- **Coverage backends.** The canvas still consumes triangles. Replacing that seam
  is sequenced by
  [ADR-0119](../adr/0119-coverage-seam-migration-staged-plan.md), whose first
  stage has landed: `tests/flux/coverage_harness.c` is the independent ground
  truth (exact-area reference, supersampled reference, conformance report) and
  `test_cover_reference` verifies it. No backend work starts before that harness
  exists, so that a wrong reference cannot make every later gate meaningless.
