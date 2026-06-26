# ADR-0005: Ear-clipping tessellator with stencil-then-cover deferred

- Status: Superseded by ADR-0011
- Date: Stage 4.2.2

## Context

`flux_canvas_fill_path` must turn a closed contour (or set of
contours) into triangles. Three plausible algorithms:

| Algorithm | Handles | Implementation cost | Output quality |
|---|---|---|---|
| Fan from p[0] | Convex only | Trivial (~10 lines) | Wrong for concave |
| Ear-clipping | Concave non-self-intersecting | Moderate (~80 lines) | Correct |
| Stencil-then-cover | Everything including self-intersecting + holes via fill rule | High (stencil image, render-pass dance, two-pass draw) | Correct |

Stage 4.1 shipped fan triangulation because it was zero cost and
unblocked the rest of the canvas plumbing. Stage 4.2 needs to fix
concave fills (stars, L-shapes, glyphs) without committing to the
full stencil-then-cover pipeline yet.

## Decision

Ear-clipping per contour, with deliberate scope cuts:

1. **Non-self-intersecting concave polygons**: handled correctly.
   The ear-clipping loop finds a convex vertex whose triangle
   contains no other live vertex, emits it, splices, repeats.
   O(n²) worst case; sub-millisecond for typical UI/SVG paths
   (< 100 vertices/contour).
2. **Multi-contour disjoint shapes**: handled. Each contour is
   ear-clipped independently. `flatten_path_to_contours` yields
   per-contour spans; the fill loop processes them sequentially.
3. **Self-intersecting polygons**: skipped (no fill, no crash).
   The ear-clip loop bails after a full pass with no ear found —
   that signals self-intersection or degenerate input. Caller
   sees an empty fill, no error.
4. **True holes (CW contour inside CCW)**: **not supported here.**
   Requires a fill-rule-aware algorithm (non-zero or even-odd
   winding determined at the rasterisation stage). The natural
   primitive for that is stencil-then-cover: render contours
   into a stencil buffer with fill-rule logic, then cover with a
   single quad. Punted to a future stage.
5. **Fill rule field**: `flux_paint.fill_rule` is accepted but
   ignored — for disjoint contours both `NON_ZERO` and `EVEN_ODD`
   produce the same output.

CCW orientation is enforced by reversing in-place when signed
area is negative. Caller doesn't have to track winding.

## Consequences

Positive:

- Stars, L-shapes, glyphs (font outlines without holes), and most
  UI primitives now render correctly. Stage 4.1's broken-for-
  concave path is gone.
- Bounded complexity: ~80 lines of algorithm; no extra render
  passes, no stencil image, no two-pass draw. Stage 4.2 stays
  within the existing solid-fill pipeline.
- Self-intersecting input fails closed (empty fill rather than
  geometry explosion). The bail-after-one-pass guard avoids
  infinite loops on degenerate inputs.

Negative:

- True holes don't render. A path that draws a CCW outer contour
  followed by a CW inner contour will fill both as positive
  contours, not as outer-with-hole. Documented limitation; the
  workaround is to compose the shape from disjoint contours.
- O(n²) means a 10K-vertex contour takes ~100M operations.
  In practice paths are flattened from curves at depth 5 (max
  32 segments/curve), so even a complex glyph stays under 200
  vertices. Stencil-then-cover would be O(n) but require
  infrastructure we haven't built.

## When to revisit

Replace with stencil-then-cover (or libtess2) when *any* of:

- A consumer demonstrably hits the self-intersecting case in
  production (e.g. an SVG renderer fed arbitrary user paths).
- Fonts with holes (`O`, `e`, `a`) need to render correctly. This
  will happen the moment text shaping lands.
- A profile shows ear-clipping consuming meaningful frame time
  on a real workload.

The replacement adds a stencil image (allocated and resized with
the surface) plus a two-pass draw inside `fill_path`. The
flattener and contour-splitting code stays unchanged.

## Alternatives considered

- **libtess2** (vendored). Rejected: a 4 KLoC dependency for a
  feature we can ship in 80 lines, and the stencil-then-cover
  path is the correct primitive for the cases libtess2 would
  cover too. Buying a partial solution.
- **Monotone polygon decomposition** (Garey-Johnson-Preparata-
  Tarjan). Faster than ear-clipping (O(n log n)), but the
  decomposition step is itself ~300 lines and the speedup
  matters only at scales we don't hit.
- **Compute-shader tessellator**. Premature; we'd need a real
  workload to drive the design.

## See also

- `src/canvas/canvas.c` — `ear_clip_contour`,
  `flatten_path_to_contours`.
- `docs/adr/0004-paint-kind-drives-pipeline.md` — pipeline
  selection model the tess plugs into.
