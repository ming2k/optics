# ADR-0080: Explicit offscreen composition graph above flux

- Status: Accepted
- Date: 2026-08-25
- Scope: `flux-composition-graph`; extends ADR-0008 and supersedes
  ADR-0079's glass-over-glass scope consequence

## Context

ADR-0008 kept effect chaining caller-side until a real consumer chained
three or more operators per frame. ADR-0079 then fused one recurring
frost-to-glass relation inside a Prism material, while leaving
glass-over-glass out of scope.

Aegis now has the concrete deeper chain: capture a live cover, blur and
materialize it, evaluate liquid glass against that resolved result, then
evaluate another glass layer against the preceding result. More named
material variants do not solve this generally. The dependency is between
images produced by passes, not an identity rule belonging to blur, glass,
or any other operator.

Naive caller-side chaining also misses information available only when the
complete structure is known: reverse sampling footprints, forward damage,
cycle validation, persistent-cache validity, and non-overlapping transient
image lifetimes. Implicit UI-tree nesting would recover some structure but
would make layout hierarchy control GPU allocation and pass order.

## Decision

Add the Rust companion crate `flux-composition-graph` as a policy-free
composition planner above Flux image operators.

1. A graph contains external image sources and image-producing passes.
   Every pass declares explicit input edges; UI ancestry never creates an
   edge implicitly.
2. Compilation validates budgets, missing inputs, and cycles, then emits a
   stable topological schedule for the subgraph reachable from named
   outputs.
3. Every edge declares a conservative coordinate map. Reverse planning
   expands requested output regions into required input regions; forward
   planning expands source damage into affected output regions. Unknown
   mappings degrade to full-input/full-output propagation.
4. Pass outputs declare transient or persistent storage. Compilation
   assigns compatible transient outputs to the same target only when their
   lifetimes do not overlap. Persistent outputs receive unique assignments
   and frame planning skips them only when the caller marks them valid and
   no damage or invalidation reaches them.
5. The planner records structure and regions only. It does not own
   `flux_image`, command buffers, barriers, materials, animation clocks, or
   product policy. An executor maps scheduled nodes to raster, copy,
   compute, or material operations and realizes the target assignments.
6. Graph size, edge count, and region fragmentation are bounded. Excessive
   region fragmentation collapses conservatively to a bounding rectangle
   instead of growing without limit.
7. Operator fusion remains an execution optimization, not a semantic
   requirement. `prism_backdrop_layer` continues to fuse frost beneath
   glass inside one node. A later glass node samples that node's resolved
   image through an ordinary graph edge.

The first implementation is Rust because the motivating compositor and the
safe Optics operators are Rust consumers, and the planner contains no native
resource or ABI surface. A C ABI is deferred until a C consumer needs the
same planner; it must preserve these semantics rather than introduce a
second graph model.

## Alternatives Considered

- **Put a frame graph inside `libflux`.** Rejected: ADR-0001 explicitly
  keeps render/frame graphs above the foundational rendering library.
- **Add nested-input modes to Prism.** Rejected: blur, glass, and other
  materials would each acquire graph policy, and arbitrary depth would
  still require caller scheduling and resource analysis.
- **Infer layers from canvas save/restore or the UI tree.** Rejected:
  visual hierarchy is not a sampling dependency. Small layout changes
  would silently alter pass topology and memory cost.
- **Keep raw caller-side vectors of effects.** Rejected: a linear vector
  cannot represent branches, validate cycles, propagate per-edge regions,
  or safely alias targets.
- **Fuse every compatible chain automatically.** Rejected as the semantic
  model. Fusion may change precision, damage granularity, cache boundaries,
  and material identity; executors may select it only when equivalence is
  proven.

## Consequences

- Arbitrary finite offscreen depth and branching use one mechanism. A
  cover-to-glass-to-glass stack is three nodes, not a new Prism material.
- A fully changing chain still costs approximately one operator pass per
  level over its affected pixels. The graph does not make cumulative work
  free; reverse ROI, damage propagation, persistent caches, target aliasing,
  and proven fusion prevent unrelated pixels and unchanged levels from
  multiplying that cost.
- Intermediate resolved images are required only for nodes with downstream
  consumers. Final-only nodes may keep transparent composites without a
  second resolved target.
- Executors must honor topological order, target compatibility, frame-slot
  ownership, and cache validity. A `TargetId` is a lifetime assignment, not
  a GPU object.
- `prism_backdrop_layer` retains its local frost-to-glass contract. The old
  statement that glass-over-glass needs another named material is replaced
  by explicit composition edges.
- The Rust workspace gains one pure planning crate and its own unit-test
  surface. Native library link lines and Flux's C ABI do not change.

## References

- [ADR-0001: Project foundations](0001-project-foundations.md)
- [ADR-0008: Image-effect pipeline](0008-image-effect-pipeline.md)
- [ADR-0017: Canvas render-target capture](0017-canvas-render-target-capture.md)
- [ADR-0074: Effect intake path](0074-effect-intake-path.md)
- [ADR-0079: Layered backdrop material](0079-layered-backdrop-material.md)
- [Composition Graph Reference](../reference/composition-graph.md)
