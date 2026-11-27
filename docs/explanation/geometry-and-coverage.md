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
| Curve algebra (kernel) | Cubic → quadratic reduction with an exact error bound `|E|/(6·2√3)`; closed-form inflection and cusp splitting; Wang segment count; analytic parallel offset; closed-form retrograde interval; ray crossings and signed winding | `libs/gfx/include/optics/gfx/curve.h`, `libs/gfx/src/curve/curve.c` |
| Path → polyline bridge | Uniform sampling at the kernel's Wang count (no recursion, no flatness search) | `libs/gfx/src/canvas/curve_flatten.c` |
| Fill dispatch | Flattened contours in physical pixels go to the backend's `submit_cover`, for either fill rule | `libs/gfx/src/canvas/canvas_dispatch.c` |
| Analytic coverage (host) | Exact per-pixel area by sub-band integration over an active edge table | `libs/gfx/src/canvas/coverage_raster.c`, `backend_cpu.c` |
| Analytic coverage (device) | 16×16 tile binning, prefix scan, per-row winding backdrop, per-pixel area | `libs/gfx/src/canvas/canvas_tile.c`, `shaders/canvas_tile_*.slang`, `backend_vk.c` |
| Stroke expansion | Per-vertex mitre frames; inner offset clamped at the local radius of curvature; miter/bevel/round joins; butt/square/round caps; each body, join and cap emitted as one positively oriented convex contour | `libs/gfx/src/canvas/geometry_stroke.c` |
| Path building | `MOVE`/`LINE`/`QUAD`/`CUBIC`/`CLOSE` verbs; rect / circle / per-corner rrect & squircle constructors (`optics_gfx_path_add_rrect`, two cubics per corner fitted to `optics_gfx_sdf_rrect`'s zero set) | `libs/gfx/src/canvas/path.c` |
| Distance fields (primitives) | Circle, per-corner rrect/squircle (circular↔L4 corner blend) + analytic derivatives, border ring, analytic coverage | `libs/gfx/include/optics/gfx/sdf.h`, `libs/gfx/src/sdf/sdf.c` |
| Distance field (device) | Rounded-rect field, border ring adjustment, one-pixel analytic coverage | `libs/gfx/src/canvas/shaders/canvas_sdf.frag.slang` |
| Distance field (host) | The same field via the shared primitive | `libs/gfx/src/canvas/backend_cpu.c` (`frag_sdf`) |
| Strategy dispatch | Which geometry class takes the analytic path and under which paint/blend/transform. Rounded rect, circle and squircle are analytic when solid + SRC_OVER + axis-uniform | `libs/gfx/src/canvas/canvas.c` (`canvas_draw_geometry`, `draw_sdf_shape`) |
| Shared SDF shader snippet | Analytic rounded-rect and squircle signed distance + derivatives for custom Slang consumers | `libs/gfx/include/optics/gfx/shaders/optics_gfx_sdf.slang` |
| Baked coverage (opt-in) | Device-resolution icon coverage mask, rasterized once per icon size by the CPU canvas | `libs/ui/src/render/icon_mask.c` |
| Glyph coverage | FreeType-rasterised coverage atlas (host-resident or device) | `libs/text/src/atlas.c` |
| Icon vector source | Format-agnostic per-context vector command streams (`optics_ui_icon_desc`) | `libs/ui/src/icon_runtime.c` |

## The curve pipeline, in order

1. **Build.** A caller appends `MOVE`/`LINE`/`QUAD`/`CUBIC`/`CLOSE` verbs, or a
   constructor emits them: `optics_gfx_path_add_circle` and `optics_gfx_path_add_rrect`
   (at `curvature == 0`) use the standard `k ≈ 0.5522847498` circular-arc control offset;
   `optics_gfx_path_add_rrect` (at `curvature > 0`) emits two fitted cubics per corner.
2. **Flatten.** `curve_flatten.c` computes each curve's segment count from the
   kernel's Wang formula and samples uniformly, in object space, against a
   screen-space tolerance divided by the transform's operator norm.
3. **Fill.** The flattened contours go to the backend's `submit_cover` with the
   path's fill rule. Self-intersecting and holed contours need no special case:
   the coverage rasterizer integrates the winding number directly.
4. **Stroke.** `geometry_stroke.c` offsets the polyline into pieces — one quad
   per segment body, a wedge, quad or fan per join where the mitre is clamped,
   and one per cap — each a positively oriented convex contour (a body quad
   whose inner offsets crossed is split into triangles). Their non-zero union is
   exactly the stroke, so overlapping pieces never double-blend. The pieces take
   the same `submit_cover` route as a fill.
5. **Rasterise.** Both backends compute exact per-pixel area coverage at 1x; there
   is no sample count, no stencil attachment and no triangulation.

## The analytic branch

Every path fill and stroke reaches the backend through `submit_cover`
(ADR-0119): it receives the contours in physical pixels with the draw's brush,
computes per-pixel coverage and evaluates the brush (flat colour or gradient)
per pixel. Glyph runs sample their atlas instead (`libs/text/src/atlas.c`).

Analytic primitives bypass steps 2–4 entirely: the fragment stage maps the
fragment back into the **shape's own space** (through the inverse of the
transform's linear part) and evaluates the closed-form distance there, then
derives coverage as `clamp(0.5 − d / aa, 0, 1)` where `aa` is one screen pixel
expressed in shape units. Because the field is Eikonal (`|∇d| = 1`) away from the
medial axis, that expression is a correct one-pixel anti-aliasing rule; because
the mapping is exact for any similarity, rotation and uniform scale need no
special case. Shear and anisotropic scale distort distance and so go through
the path-coverage route instead.

The host backend evaluates the same primitives from `<optics/gfx/sdf.h>`, so host and
device coverage agree by construction; `test_sdf` additionally pins the
primitive against the shader's expression so the two cannot drift.

## What is not yet done

Recorded so the gaps are visible rather than inferred:

- **Unified RRect / Squircle.** One shape, one path, one dispatch: `optics_gfx_sdf_rrect`
  defines the per-corner circular↔L4 corner arc, `optics_gfx_path_add_rrect` fits it,
  and `OPTICS_GFX_GEOM_RRECT` (solid, SRC_OVER, similarity transform) evaluates the
  field directly instead of taking the path route (ADR-0175).
- **Shear and anisotropic scale.** The field is evaluated in shape space, which
  is exact for rotation and uniform scale (a similarity preserves distance).
  Shear and anisotropic scale distort distance, so those take the
  path-coverage route rather than being drawn with an approximated field.
- **Stroke on curves.** The stroker offsets a flattened polyline, so its
  curvature is a per-vertex estimate rather than the kernel's closed form.
