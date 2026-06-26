# ADR-0007: Hand-rolled Vulkan slab allocator (no VMA)

- Status: Accepted
- Date: 2026-05-19

## Context

Vulkan implementations impose a per-process cap on the number of
`vkAllocateMemory` calls a program may make. The Vulkan spec sets the
floor at `maxMemoryAllocationCount = 4096` and most desktop drivers
report exactly that. A real application with hundreds of textures, a
few dozen meshes, descriptor staging buffers, and one
`flux_transient_ring` per surface will burn through that budget fast.

Through stage 7.3 every flux GPU resource had its own
`VkDeviceMemory`: `flux_image_create` allocated one, `flux_mesh_create`
allocated two (vertex + index), `flux_transient_ring_init` allocated
one, and every `flux_vk_upload_to_*` helper allocated one for staging.
At ~50 images and ~10 meshes plus per-surface state, this was already
≈70 allocations; not catastrophic, but visibly heading in the wrong
direction and a hard barrier for production use cases.

The standard answer is AMD's
[Vulkan Memory Allocator (VMA)](https://gpuopen.com/vulkan-memory-allocator/):
a single-header C++ library that handles pooling, sub-allocation,
defragmentation, and dedicated-allocation policy. Every serious
Vulkan project uses it.

The tension: ADR-0001 mandates pure C23 ("C23. No C++."). VMA is C++.
Dropping it in means either:

1. **Vendor VMA + add one C++ TU** with a C facade. Meson compiles
   that TU as C++, the rest stays C. Introduces a C++ toolchain
   dependency, a 17 KLoC external file, and a soft amendment to
   ADR-0001.
2. **Write a small allocator in pure C** that covers the cases flux
   actually has today: per-resource sub-allocation, pooling by
   memory type, dedicated fallback, and basic free-list coalescing.

## Decision

**Hand-roll a small slab allocator** in pure C. ~400 lines, no C++
dependency, no vendored 17 KLoC. The allocator's surface (`alloc`,
`dealloc`, `init`, `destroy`) is plain C and lives in
`src/core/vk_allocator.c`. The implementation is described in the
top-of-file comment.

Trade-offs accepted explicitly:

- No defragmentation. The allocator can't relocate live objects.
  Long-running applications with churning resource sets may
  fragment; today's targets (UI, scenes with stable assets, compute
  workloads) don't churn heavily.
- No buffer-image granularity arithmetic. Instead, buffers and
  images live in disjoint pools (the `is_image_pool` bit) so adjacent
  placements never need the granularity check.
- No memory-budget tracking against `VK_EXT_memory_budget`. The
  allocator counts what it has handed out, but doesn't query the
  driver for live pressure.
- The `(memory_type, is_image_pool, has_dev_addr)` key over-segments
  blocks. A device-local buffer pool and a host-visible buffer pool
  with the same memory type on a UMA driver (lavapipe, integrated
  GPUs) end up as separate blocks. Memory waste is bounded by one
  partially-filled block per category. Acceptable.

The threshold between pooled and dedicated allocations is
`FLUX_VK_DEDICATED_THRESH = 16 MiB`. Larger requests skip the pool
entirely and get their own `VkDeviceMemory` — this matters most for
`flux_transient_ring` (32+ MiB) and high-resolution textures.

## Consequences

Positive:

- Hundreds of small resources cost a handful of `VkDeviceMemory`
  objects. The 4096-allocation cap is no longer a ceiling on app
  scale.
- Pure C codebase preserved. No C++ TU, no PIC issues, no
  C++ ABI to worry about on macOS / Windows when those land.
- The implementation is small enough to read, audit, and modify
  in-tree. Bugs are local.
- Thread-safety lives at the allocator level (one mutex), not at
  every call site.

Negative:

- No defragmentation. If a future workload demonstrably fragments
  and frees can't recover the space, we'll either add a compaction
  pass or revisit VMA.
- No memory-budget feedback. The allocator can over-commit if every
  application on the system is doing the same. Today's targets
  haven't hit this.
- 400 lines of memory code to maintain. Mitigated by allocator
  test coverage (`tests/test_allocator.c`) and the small surface
  area.

## When to revisit

Replace with VMA (or a dedicated compaction pass) when *any* of:

- A real consumer demonstrably fragments their working set such
  that free space exists in aggregate but no single block can
  satisfy an alloc.
- We start needing `VK_EXT_memory_budget` to back off under pressure
  from other GPU processes.
- Multi-platform pressure makes the "no C++" rule less load-bearing
  than the cost of writing the second-system features (defrag,
  cross-vendor heuristics) ourselves.

ADR-0001's no-C++ tenet stands; this ADR refines its scope: the
public API stays C, internal modules stay C, and pulling in C++ for
a *specific* dependency (e.g. VMA) is reserved for a future ADR that
demonstrates concrete need.

## See also

- `src/core/vk_allocator.c` — implementation.
- `src/core/internal.h` — `flux_vk_allocator`, `flux_vk_alloc`,
  `flux_vk_allocate`, `flux_vk_deallocate` definitions.
- `tests/test_allocator.c` — stress tests against the public API.
- ADR-0001 — the original "no C++" tenet this ADR refines.
