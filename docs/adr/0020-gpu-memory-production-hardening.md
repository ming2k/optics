# ADR-0020: GPU memory production hardening (amends ADR-0007)

- Status: Accepted — item 5 amended by [ADR-0021](0021-batched-uploads-and-quiescent-waits.md)
  (`prefersDedicated` is now honoured only at or above 1 MiB)
- Date: 2026-07-17

## Context

ADR-0007 accepted a hand-rolled slab allocator with three explicitly
deferred capabilities: no memory-budget feedback, no defragmentation,
and no use of `VK_EXT_memory_budget`. A production-readiness review of
the VRAM stack found the core design sound but the surrounding
machinery short of production grade:

- **Deferred destruction covered images only.** `flux_buffer`,
  `flux_mesh`, and `flux_target` destroyed Vulkan handles and freed
  memory inline at refcount zero — the same in-flight-reference hazard
  that motivated the image retire queue (GPU hang → `DEVICE_LOST` on
  i915) was open on the buffer path.
- **Wait-idle calls violated Vulkan host-synchronisation rules.**
  `vkDeviceWaitIdle` was called from `flux_canvas_destroy`,
  `flux_surface_release`, and `flux_surface_resize` with no
  synchronisation against concurrent `vkQueueSubmit2` on other threads
  (the spec counts wait-idle as host access to the queues). Observed
  as an ANV driver double-free (`anv_async_submit_fini`) under
  parallel load.
- **`VK_EXT_memory_budget` was detected but never enabled**, so
  `flux_device_memory_budget` chained a pNext struct whose extension
  was off — a spec violation — and nothing consumed the numbers.
- **OOM handling stopped at error propagation.** No reclaim-retry, and
  the dedicated path reported `FLUX_ERROR_BACKEND_FAILURE` where the
  pool path reported `FLUX_ERROR_OUT_OF_MEMORY`.
- **`VkMemoryDedicatedRequirements` was never queried**, so a resource
  the driver *requires* a dedicated allocation for would have been
  incorrectly pool-sub-allocated (a spec violation on such drivers).
- **Allocator statistics were internal-only**, dma-buf imported memory
  was invisible to them, and no test could assert leaks.
- **Staging was create-use-destroy per upload**, and there was no
  fault-injection coverage of the OOM paths.

Defragmentation remains deliberately out of scope; ADR-0007's
revisit triggers are unchanged.

## Decision

One hardening pass over the VRAM stack, keeping the ADR-0007 design:

1. **Retire queue for every GPU resource.** The image zombie
   mechanism is generalised (`flux_retire_zombie`) to also carry
   buffers; `flux_buffer_release`, `flux_mesh_release`, and
   `flux_target_release` park their pieces exactly as images do.
   Transient-ring slices and per-frame-slot canvas attachments were
   audited and need no deferral — slot fencing already covers them.
2. **All wait-idle calls funnel through `queue_lock`.**
   `flux_vk_wait_idle` holds the same mutex that serialises
   `vkQueueSubmit2`, satisfying the host-synchronisation rule with no
   new lock ordering (the lock never nests with `retire_lock`).
3. **`VK_EXT_memory_budget` enabled when advertised** at device
   creation. Before a new pool block is allocated, the heap's
   usage/budget is queried and empty blocks are reclaimed if the
   request would cross the budget; the definitive failure still comes
   from `vkAllocateMemory` (budget is a hint, not a hard limit).
4. **Reclaim-and-retry on allocation failure.** Both the pool and
   dedicated paths call the (previously dead)
   `flux_vk_allocator_reclaim` once and retry before declaring OOM.
   `VK_ERROR_OUT_OF_DEVICE_MEMORY` and `VK_ERROR_OUT_OF_HOST_MEMORY`
   uniformly map to `FLUX_ERROR_OUT_OF_MEMORY` on every path.
5. **Driver dedication hints honoured.** `flux_vk_alloc_buffer/image`
   chain `VkMemoryDedicatedRequirements`; `requiresDedicated` (hard
   requirement) and `prefersDedicated` both route to a dedicated
   allocation with `VkMemoryDedicatedAllocateInfo` chained.
6. **Public statistics + leak gates.** `flux_device_memory_stats`
   exposes `bytes_in_use` / `bytes_reserved` / `live_allocations` /
   `live_blocks` / `lost_ranges_bytes`. dma-buf imports and exports
   are counted via `flux_vk_allocator_note_external`, balanced at
   retire/zombie time. Tests assert steady-state counts after churn.
7. **Staging cache.** One-shot upload/readback helpers recycle
   host-visible staging buffers from a per-device, lock-guarded idle
   list (smallest-fit by capacity, 64 MiB cap), replacing
   create-destroy per call. Safe because every one-shot helper is
   synchronous (submit + fence wait, with queue-idle fallback).
8. **Fault injection.** `tests/flux/integration/test_alloc_fault.c`
   interposes `vkAllocateMemory` (executable symbol precedence) to
   drive pool failure, dedicated failure, and reclaim-retry recovery
   end to end — no production-code hooks.
9. **Debug object naming.** `VK_EXT_debug_utils` is enabled whenever
   the instance advertises it (not just under validation); pool
   blocks, dedicated allocations, staging buffers, the transient
   ring, `flux_buffer`, and `flux_image` get
   `vkSetDebugUtilsObjectNameEXT` names for RenderDoc and validation
   output.

## Consequences

Positive:

- The `DEVICE_LOST`-class hazard is closed uniformly for buffers,
  meshes, and targets, and the wait-idle/submit race is gone by
  construction.
- OOM is survivable: budget-aware reclaim plus retry before any
  failure is reported, with a single coherent error code.
- Production triage is possible: stats API, teardown leak warning
  covering external memory, and named objects in captures.
- The OOM paths are now regression-tested, not aspirational.

Negative / accepted:

- Defragmentation is still absent (ADR-0007's position stands).
- The staging cache pins up to 64 MiB of host-visible memory per
  device until teardown or cap eviction.
- Budget consumption is advisory (reclaim-first, proceed-with-driver's-
  answer), so a pathological neighbour can still push the driver into
  paging; a hard budget-enforcement knob is future work if a consumer
  needs it.

## See also

- ADR-0007 — the slab-allocator decision this ADR amends.
- `libs/flux/src/core/vk_allocator.c`, `device.c` (retire queue,
  `flux_vk_wait_idle`), `oneshot.c` (staging cache), `memory.c`
  (dedication hints).
- `tests/flux/integration/test_alloc_fault.c` — fault injection.
- `tests/flux/integration/test_buffer_retire.c`,
  `test_mesh_retire.c`, `test_memory_budget.c` — regression coverage.
