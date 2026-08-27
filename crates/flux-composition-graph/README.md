# flux-composition-graph

Policy-free planning for explicit offscreen image DAGs above Flux operators.

The crate validates dependencies and cycles, computes a stable topological
schedule, propagates required regions backwards and damage forwards, and
assigns compatible non-overlapping transient outputs to reusable target ids.
It owns no Flux images or command buffers; the host executes the plan with
raster, copy, compute, or material passes.

See the
[Composition Graph Reference](../../docs/reference/composition-graph.md)
and [ADR-0080](../../docs/adr/0080-explicit-offscreen-composition-graph.md).
