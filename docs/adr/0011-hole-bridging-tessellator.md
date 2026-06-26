# ADR-0011: Hole-bridging ear-clipping tessellator

- Status: Accepted
- Date: Stage 4.3

## Context

ADR-0005 shipped an ear-clipping tessellator that handles concave,
non-self-intersecting polygons. True holes (CW contour inside a CCW
outer) were explicitly out of scope — each contour was ear-clipped
independently, so a CW hole was simply filled as a positive area.

Three approaches to hole support were evaluated:

| Algorithm | Handles | GPU requirement | Implementation cost |
|---|---|---|---|
| Stencil-then-cover | Everything (self-intersecting, holes, fill rules) | Stencil attachment + two-pass draw | High |
| Hole bridging (bridge-edge) | Properly-nested holes via CW-in-CCW | None (single-pass, same pipeline) | Moderate |
| libtess2 (vendored) | Full tessellation including self-intersecting | None | 4 KLoC dependency |

## Decision

**Hole bridging via bridge edges.** The algorithm:

1. Classify each contour by signed area: positive → CW (hole),
   negative → CCW (solid).
2. For each hole, find the rightmost vertex.
3. Cast a ray from that vertex towards the nearest vertex on the
   containing solid contour that does not intersect any edge.
   This produces a "bridge edge" — a pair of zero-area diagonal
   insertions that connect the hole to the outer contour.
4. Merge the bridged contours into a single polygon and ear-clip
   in one pass.

This avoids any stencil buffer, additional render passes, or pipeline
changes. The single-pass solid-fill pipeline continues to work
unchanged.

Additionally, the fixed `FLUX_PATH_MAX_SEGMENTS = 1024` cap on the
path builder has been removed. Paths now grow dynamically from the
arena, eliminating silent segment drops for large paths.

## Consequences

Positive:

- True holes (CW inside CCW) render correctly without stencil or
  multi-pass infrastructure. Fonts with O, e, a shapes render
  correctly.
- Path builder has no fixed capacity limit — arena size is the only
  constraint. `flux_path_dropped_count` now reports arena exhaustion
  rather than a hardcoded cap.
- Zero GPU-side changes: same shaders, same pipeline, same single
  render pass.

Negative:

- Bridge-edge computation is O(n·m) where n = hole vertices, m =
  outer vertices. Acceptable for UI/SVG paths (< 200 vertices per
  contour).
- Self-intersecting polygons still bail (same guard as before).
  Stencil-then-cover remains the correct primitive for those.
- Bridge insertion requires in-place polygon mutation within the
  scratch buffer. The merged polygon must fit within
  `FLUX_CANVAS_PATH_SCRATCH_CAP`; very complex paths with many holes
  may still require fallback.

## When to revisit

Replace with stencil-then-cover when *any* of:

- Self-intersecting user paths are required (SVG renderers, arbitrary
  user input).
- The bridge-edge O(n·m) computation shows up in profiles.
- Fill-rule-aware rendering (EVEN_ODD / NON_ZERO distinction for
  overlapping contours of unknown nesting) is needed.

## See also

- ADR-0005 — the original ear-clipping decision this supersedes.
- `src/canvas/geometry_tess.c` — bridge-edge implementation and
  ear-clip entry point.
- `src/canvas/path.c` — dynamic path growth from arena.
