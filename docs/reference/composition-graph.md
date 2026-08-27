# Composition Graph Reference

`flux-composition-graph` plans explicit image dependencies above Flux. It
does not allocate images or record GPU commands.

## Graph Model

| Type | Contract |
|------|----------|
| `CompositionGraph` | Mutable declaration of sources, passes, and dependencies. |
| `NodeId` | Stable node index within one graph. |
| `ImageDesc` | Width, height, and opaque compatibility class. The class must encode every property that prevents target reuse. |
| `Storage::Transient` | Output lives through its last consumer in the current schedule and may alias a compatible non-overlapping output. |
| `Storage::Persistent` | Output may be cached across frames and receives a unique target assignment. |
| `RegionMap::Scale` | Maps coordinates between image extents. `sample` padding expands reverse ROI; `damage` padding expands forward influence. |
| `RegionMap::Full` | Conservative mapping for an operator without a tighter bound. |
| `GraphLimits` | Bounds nodes, edges, and rectangles retained per node. |

Call `add_source` and `add_pass`, then connect every pass with
`add_dependency`. `compile(outputs)` rejects empty pass inputs, unknown
nodes, duplicate edges, and cycles.

## Frame Planning

`FrameRequest` supplies dynamic state:

| Method | Meaning |
|--------|---------|
| `request_output` | Restrict one compiled output to requested regions. Omitted outputs request their complete domain. |
| `damage_source` | Declare changed pixels in an external source. |
| `mark_persistent_valid` | State that a frame-slot-local persistent output already contains usable pixels. |
| `invalidate` | Rewrite an operator because a shader, material, geometry, or other non-image input changed. |

`CompiledGraph::plan_frame` first propagates requested regions backwards,
then propagates damage forwards. Each `ExecutionStep` contains the output
target assignment, logical required region, damaged region, actual work
region, and an `execute` decision. Persistent passes work only on invalid or
damaged requested pixels. Transient work propagates backwards only from an
executing consumer or a requested transient graph output, so a valid cached
sink does not wake its transient ancestors.

## Target Assignments

`TargetId` is an allocation-equivalence label, not a Flux image. The
executor creates or leases an image for each target id using its associated
`ImageDesc`. Two nodes share an id only when their descriptors match and
the earlier output's last read precedes the later output's write.

External sources have no target assignment. Persistent outputs never alias.

## Nested Offscreen Example

```rust
use flux_composition_graph::{
    CompositionGraph, FrameRequest, ImageDesc, Padding, Rect, RegionMap,
    RegionSet, Storage,
};

let image = ImageDesc::new(1920, 1080, 1);
let mut graph = CompositionGraph::default();
let scene = graph.add_source("scene", image)?;
let cover = graph.add_pass("cover", image, image.bounds(), Storage::Persistent)?;
let glass = graph.add_pass("glass", image, image.bounds(), Storage::Persistent)?;
let glass_2 = graph.add_pass("glass-2", image, image.bounds(), Storage::Persistent)?;

let blur = RegionMap::Scale {
    sample: Padding::uniform(36),
    damage: Padding::uniform(36),
};
graph.add_dependency(cover, scene, blur)?;
graph.add_dependency(glass, cover, RegionMap::local(24, 24))?;
graph.add_dependency(glass_2, glass, RegionMap::local(16, 16))?;

let compiled = graph.compile(&[glass_2])?;
let mut frame = FrameRequest::new();
frame.request_output(
    glass_2,
    RegionSet::from_rect(Rect::new(600, 300, 480, 320)),
);
let plan = compiled.plan_frame(&frame)?;
# Ok::<(), flux_composition_graph::GraphError>(())
```

The scene ROI includes the requested rectangle plus every edge's sampling
padding. The executor records the steps in `topological_nodes` order and
passes each resolved output to the next node.

## Cost Model

- Changing all pixels through `N` dependent levels performs approximately
  `N` levels of operator work.
- Reverse ROI limits work to pixels that can reach a requested output.
- Forward damage avoids rewriting unaffected cached regions.
- Persistent validity skips unchanged levels for each frame slot.
- Transient target aliasing reduces allocation count; it does not remove
  shader work.
- Operator fusion is valid only when it preserves output, cache, precision,
  and damage semantics.
