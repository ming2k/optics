# RenderPlan 1B-2A — effect passes join the plan

Status: **landed**. Deferred-effect liveness now lives in `flux_render_plan`;
`flux_plan` carries only execution-side state (arena, tokens, pool, counters).

This document is the atomic change boundary for 1B-2A. It exists so the next
change does not leave two liveness tables behind.

## Problem

ADR-0147's deferred effect plan keeps its own pass array, provider/requires
edges, tokens, and `required`/`built`/`executed` state in
`libs/flux/src/core/plan.c`. ADR-0089's RenderPlan now has the frame's target
contract and node/dependency table. A second liveness authority is exactly the
architecture rot this program is removing.

1B-2A must make `flux_render_plan` the only graph/liveness table. `flux_plan`
may survive only as the execution-side owner of the per-frame arena, command
buffer pool, token epochs, and counters.

## Target shape

`flux_render_node` gains an effect payload, used only by
`FLUX_RENDER_NODE_EFFECT`:

- `record(params, VkCommandBuffer)`
- arena-copied `params`
- output image handles
- `effect_segment`
- `effect_required`, `effect_built`, `effect_executed`

The node's access array is the graph edge set:

- each input image is a READ access;
- each output image is a WRITE access;
- the provider node is an explicit dependency;
- token-only prerequisites become dependencies without an access.

`flux_plan` keeps:

- `flux_render_plan *render_plan`
- parameter arena
- token epochs
- command pool / device
- `pass_count`, `executed_total`, `fallback_total`

It loses `passes[]`, `provider[]`, `requires[]`, and the duplicated
`required/built/executed` arrays.

## Migration order (one commit)

1. Extend `render_plan.h` with the effect node payload and access/dependency
   construction helpers.
2. Rewrite `flux_plan_register` to resolve tokens/providers into RenderPlan
   dependencies and add one `EFFECT` node.
3. Rewrite `require_index`, `require_image`, `flush`, `finalize`, and
   `image_pending` to walk RenderPlan nodes.
4. Change `flux_plan_reset` to receive the frame's RenderPlan and reset only
   execution-side state.
5. Delete every `flux_plan_pass` field and every parallel provider/requires
   array from `plan.c`/`plan.h`.
6. Keep `effect.c` call sites unchanged; their public contract is already the
   deferred-pass descriptor.

## Mandatory tests

- `test_plan_prune`: an unconsumed effect is pruned, a consumed one executes,
  and a chained blur -> material still builds the producer first.
- Effect parameter-arena exhaustion still fails with no published pass.
- A token from a previous frame is rejected by epoch.
- `flux_plan_image_pending` answers from the node's output set.
- Canvas pass validation accepts Canvas nodes and effect nodes in one plan.
- Full effect/material integration suite and Canvas pixel tests.

## Deletion gate

1B-2A is complete only when `grep` finds no second pass array in `plan.c`, no
`flux_plan_pass` type, and no provider/requires table outside
`flux_render_plan`. If any of those remain, the change is incomplete and must
not be merged.

## Exit criteria

- One liveness graph: `flux_render_plan`.
- `flux_plan` is execution state only.
- Existing effect pruning and ordering tests pass unchanged semantically.
- No new public API.
- ADR-0089 remains `In progress` until the 1B-2B/3/4/5 migration completes.
