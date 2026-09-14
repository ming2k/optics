# ADR-0092: Resource planning, synchronization, and retirement

- Status: Accepted
- Date: 2026-09-14
- Scope: Flux execution compiler, GPU resources, uploads, and font atlas.
- Depends on: [ADR-0089](0089-rendering-program-and-execution-plan.md),
  [ADR-0090](0090-display-list-resource-ownership.md),
  [ADR-0091](0091-drawing-group-and-effect-semantics.md).
- Implementation: Complete; verified against acceptance criteria in [the implementation matrix](../dev/architecture-review-2026-09-14.md).

## Context

Canvas layers, the external composition planner, uploads, and font atlas
management currently describe overlapping pieces of resource lifetime.
Safety of a staging ring alone does not establish safety of its destination
image or of glyph references cached across frames.

## Decision

Flux compiles one RenderPlan for all managed raster, copy, compute, effect,
and output-transform work. Each node declares resource identity/version,
subresource or buffer range, read/write mode, execution domain, initial and
required final state, and dependencies. Pixel regions used for damage and
sampling are distinct from backend synchronization granularity.

Compilation validates inputs, capabilities, cycles, access conflicts, and
budgets before execution. Ordered writes create explicit content versions;
no two writers can produce the same version. Backend lowering derives
barriers, layouts, queue transfers, and submission dependencies from access
declarations. Raw custom work is allowed only through declared nodes and
validated import/export boundaries, never through a parallel submission
route for managed resources.

Reverse sampling-footprint propagation determines required input regions;
forward damage propagation determines affected outputs. Unknown mappings
expand conservatively. Region fragmentation has a limit and collapses to a
bounding rectangle when exceeded. Cyclic feedback requires an explicit
previous-submission input; same-plan cycles are rejected.

### Allocation and completion

Transient allocations may alias only when their requirements are compatible
and execution lifetimes do not overlap. A CPU topological listing alone
does not establish non-overlap across queues. Reuse requires actual dependency
ordering and the backend's aliasing synchronization.

Submissions retain all required resources, descriptor entries, allocations,
and imported leases. Retirement waits for every relevant queue completion,
not a frame-number estimate or the final CPU reference alone. Persistent
caches cannot reclaim in-flight objects. Reusable plan structure is separate
from per-execution allocations and leases.

Budgets include transient images, pinned snapshots, caches, uploads, and
in-flight submissions. Pressure can evict recreatable completed resources,
limit in-flight work, or return a budget error. It cannot silently lower
effect quality or overwrite a live resource. No lock-free implementation or
specific number of frame slots is required by the public contract.

On submission failure, already-enqueued work retains its resources until
completion or backend teardown establishes that access has ended. A failed
submission never reports success; partial writes invalidate affected target
content. Device loss terminates dependent submissions and invalidates device
realizations, not immutable CPU content. There is no promise of GPU rollback.

### Text and atlas

Shaping and measurement use CPU font services and may maintain synchronized
CPU caches; they do not allocate atlas slots or upload to the GPU. Rasterized
glyphs are CPU cache entries. DisplayLists retain font versions and logical
glyph identities, not atlas coordinates.

Compilation resolves glyphs to physical atlas allocations and adds explicit
uploads before reads. Staging reuse, atlas destination writes, descriptor
reuse, and page eviction have separate tracked lifetimes. In-flight slots
remain pinned. Moving or evicting completed allocations does not invalidate
a cached DisplayList: the next compilation resolves its glyphs again.

## Alternatives Considered

- **Keep the Rust graph outside Flux.** Leaves resource ownership and
  synchronization in an independent executor with duplicate planning rules.
- **Use per-frame atlas copies universally.** Pays duplication without
  solving general image, descriptor, and external-resource lifetime.
- **Wait for device idle on updates.** Serializes ordinary execution and
  conceals missing dependencies.

## Consequences

Replace the scheduling responsibilities of
`crates/flux-composition-graph/`, the manual managed-resource submission
paths in `libs/flux/src/core/`, and atlas flush scheduling in
`libs/flux/text/src/atlas.c`. Preserve required ROI and budget behavior in
the single compiler before deleting the external planner.

## Acceptance Criteria

- Delayed multi-queue completion cannot trigger early image, descriptor,
  staging, atlas, or imported-resource reuse.
- Graph validation rejects cycles, missing producers, incompatible usages,
  and budget exhaustion before command emission.
- Stress tests combine cached text, glyph eviction, image version changes,
  nested effects, and multiple submissions in flight.
- Failure injection covers allocation, partial submission, and device loss;
  cleanup releases resources exactly once and reports invalid target content.
- Diagnostics explain scheduling, aliasing, barriers, memory peaks, and
  pinned allocations; backend validation accompanies semantic tests.
