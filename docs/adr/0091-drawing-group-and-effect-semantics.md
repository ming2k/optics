# ADR-0091: Drawing, group composition, and explicit effects

- Status: Accepted
- Date: 2026-09-14
- Scope: Flux drawing semantics and Prism effect construction.
- Depends on: [ADR-0089](0089-rendering-program-and-execution-plan.md),
  [ADR-0090](0090-display-list-resource-ownership.md).
- Implementation: In progress; acceptance evidence and remaining work are tracked in [the implementation matrix](../dev/architecture-implementation-status.md).

## Context

Geometry and brush separation describes ordinary filling, but does not
describe backdrop reads, group opacity, or multi-pass effects. Treating all
of these as brushes hides execution dependencies. The current translation
in `libs/flux/src/canvas/encoder.c` also retains a second shape/paint model.

## Decision

Use one semantic vocabulary:

| Operation | Meaning |
|-----------|---------|
| Draw | Geometry, Brush, and DrawStyle with explicit transform and clip. |
| GlyphRun | Positioned glyph identities and immutable font inputs with explicit styling. |
| Group | Ordered children rendered in isolation, then composed once. |
| Effect | Named inputs, parameters, coordinate mappings, sampling footprints, and output. |

Geometry describes coverage. Brush describes source color or image sampling.
DrawStyle specifies fill rule or stroke parameters, opacity, and blend mode.
Stroke parameters include width, caps, joins, miter limit, and dash pattern;
they do not live in Geometry. Distinct corner radii and Squircle curvature
are real shape parameters, never aliases for an ordinary rounded rectangle.

### Coordinates and color

Recorder coordinates use logical units with x right and y down. Transforms
map local to parent coordinates; target compilation supplies the final
logical-to-device mapping. Clips capture their transform when recorded.
Bounds used for culling conservatively include stroke, antialiasing, and
effect sampling expansion. Unknown bounds disable the relevant optimization.
Non-finite geometry and invalid style parameters fail recording. Singular
transforms cannot trigger undefined inverse calculations.

Preserve the linear working-space and explicit output-transform decisions
of [ADR-0069](0069-color-management.md) and
[ADR-0070](0070-icc-in-tree-parser.md). Inputs carry color interpretation;
internal compositing uses premultiplied linear values. Opacity scales both
premultiplied color and alpha. Output encoding happens at the target edge.
Blend-mode formulas, gradient interpolation, degenerate geometry, sampling
edge behavior, and numeric tolerances require a normative semantic reference
before the corresponding operation is declared implemented.

### Groups and background effects

A group begins with transparent content. Its children render in order; the
group opacity is applied once to the result before composition into its
parent. It is not multiplied independently into each descendant draw.

Group bounds are allocation hints, not implicit clips. A separate clip
defines content truncation. Missing bounds mean conservative inference or
the enclosing clip, not an empty group. The compiler may eliminate an
offscreen allocation only when the resulting composition is equivalent.

A backdrop effect names the parent-content version immediately before its
placement, or another explicit effect output. It cannot read an ambient
"current framebuffer" or its own unfinished output. Filtering expands the
required input region; output clipping does not erase that dependency.
Nested glass therefore becomes ordinary explicit effect edges.

Prism builds these effect descriptions. Custom effects declare inputs,
access, coordinate mappings, output coverage, and supported implementations.
Unknown mappings force conservative regions. Unsupported effects fail
compilation rather than silently using a visually different brush.

### Optimization boundary

Painter order is authoritative. Reordering requires a documented equivalence
condition covering overlap, blend, clip, group boundaries, backdrop reads,
and resource writes. Material equality alone is insufficient. Adjacent
compatible work may batch without changing its semantic order.

## Alternatives Considered

- **Everything is a brush.** Hides background and intermediate dependencies.
- **Every group is permanently offscreen.** Correct but unnecessarily fixes
  an allocation strategy into the semantic contract.
- **Sort globally by material.** Changes ordered alpha composition and
  background sampling.

## Consequences

Backend-specific geometry and effect fast paths remain possible, but they
lower from this vocabulary. The old shape/paint dispatch and private Prism
submission paths are removed. Complete numeric semantics are separate
reference material, not inferred from shader implementation details.

## Acceptance Criteria

- Pixel tests cover overlapping children, nested opacity, explicit clips,
  transformed bounds, stroke joins, independent radii, and Squircle curvature.
- Backdrop chains and branches sample the specified content versions and
  preserve filter halos at allocation boundaries.
- CPU/Vulkan comparisons use declared feature coverage and tolerances.
- Optimization on/off comparisons cover blending, clipping, group opacity,
  and effect dependencies; unsupported semantics return errors.
