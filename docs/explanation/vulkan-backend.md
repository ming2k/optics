# Vulkan Backend

`flux-core` is implemented exclusively against Vulkan 1.3. There is no
abstraction layer waiting to be filled in; the public API speaks
Vulkan types directly at the seams. This document explains *how* the
backend is structured, *why* it requires dynamic rendering, and *what*
the per-frame lifecycle looks like.

## Why Vulkan 1.3 specifically

`VK_KHR_dynamic_rendering` was promoted to core in Vulkan 1.3. flux uses
it everywhere so that modules (canvas, scene, compute) own their own
attachments rather than declaring them up front in a shared render
pass. See [ADR-0001](../adr/0001-project-foundations.md).

Vulkan 1.3 also brings:

- `VK_KHR_synchronization2` (used throughout the frame implementation), which collapses
  the old `vkCmdPipelineBarrier` access-mask gymnastics into a single
  `VkImageMemoryBarrier2` struct.
- `vkQueueSubmit2`, with cleaner semaphore submit info.

Devices reporting only Vulkan 1.2 are rejected at `flux_device_create`.

## The object graph

```
                          flux_device
                    (allocator + logger + Vulkan device)
                                │
              ┌─────────────────┴─────────────────┐
              │                                   │
              ▼                                   ▼
       flux_surface                    flux_surface
     (window 1)                      (window 2)
              │                                   │
      ┌───────┴───────┐               ┌───────┴───────┐
      ▼               ▼               ▼               ▼
  frame slot 0   frame slot 1     frame slot 0   frame slot 1
     │                                  │
     └─ command buffer                  └─ command buffer
        semaphores                         semaphores
        fence                                fence
        transient ring slice                 transient ring slice
```

- `flux_device` owns CPU-side state (allocator, logger) *and* the
  `VkInstance` / `VkDevice` / queue. One per process in the typical case.
  Owns the pipeline cache and the bindless descriptor heap.
- `flux_surface` wraps one `VkSurfaceKHR` (one window). Owns the
  swapchain and `FLUX_FRAMES_IN_FLIGHT` per-frame state slots.
- A `flux_frame` is a thin handle returned by `flux_surface_begin_frame`
  identifying the active slot — no separate allocation.

## Per-frame lifecycle

```
            flux_surface_begin_frame
            ├─ wait on slot's in_flight fence
            ├─ acquire next swapchain image
            ├─ reset transient ring slice
            ├─ begin command buffer
            └─ return flux_frame handle

                       │
                       ▼ (caller, or a module like canvas/scene)

            flux_frame_begin_pass(desc)
            ├─ barrier swapchain image to COLOR_ATTACHMENT_OPTIMAL
            └─ vkCmdBeginRendering(VkRenderingInfo built from desc)

                       │
                       ▼ (module records draws)

            flux_frame_end_pass
            ├─ vkCmdEndRendering
            └─ barrier swapchain image to PRESENT_SRC_KHR

                       │
                       ▼

            flux_frame_submit
            ├─ vkEndCommandBuffer
            └─ vkQueueSubmit2 (wait on image_acquired, signal render_finished)

            flux_frame_present
            └─ vkQueuePresentKHR (wait on render_finished)
```

Swapchain `OUT_OF_DATE` and `SUBOPTIMAL` are surfaced to the caller as
`FLUX_ERROR_SURFACE_LOST` from either `flux_surface_begin_frame` or
`flux_frame_present`. flux does **not** silently rebuild the swapchain
— the caller must invoke `flux_surface_resize(s, w, h)` with the new
framebuffer extent (typically pulled from
`glfwGetFramebufferSize`) before the next frame.

Teardown waits are surface-scoped (`surface_wait_quiescent`): offscreen
release/resize waits only the surface's own per-slot `in_flight`
fences, since nothing outside those batches can reference its images or
transient ring; windowed teardown waits the graphics queue
(`vkQueueWaitIdle` under `queue_lock`), which is the only wait that
also covers the presentation engine still reading a presented image.
Neither path stalls the whole device the way the old
`vkDeviceWaitIdle` did.

## Transient memory ring

Each surface owns one host-visible, host-coherent buffer. It is split
into `FLUX_FRAMES_IN_FLIGHT` slices, one per frame slot.
Per-frame, `flux_frame_alloc_transient` bumps a cursor inside the
active slot's slice. The previous slot's slice is implicitly recycled
when the in-flight fence signals.

The buffer is created with combined usage (vertex | index | uniform |
storage | transfer-src | buffer-device-address) so a module can use it
for any binding without a second allocation. Default per-frame size:
16 MiB. Out-of-space allocations record
`FLUX_ERROR_OUT_OF_RANGE` via `flux_get_last_error` — the module's
policy decides whether that is fatal.

## Bindless descriptor heap

A single device-wide bindless descriptor heap replaces per-frame
descriptor pools. It is allocated at `flux_device_create` as one
large `VkDescriptorPool` backed by descriptor indexing
(`VK_EXT_descriptor_indexing`).

Slots in the heap are stable for the lifetime of the device:

- `flux_bindless_register_image` writes an image view + layout into the
  next free slot and returns a `flux_bindless_handle`.
- `flux_bindless_register_sampler` does the same for samplers.
- `flux_bindless_release` returns the slot to the heap.

The heap is bound once per pipeline as set 0. Shaders reference
resources by handle (a plain `uint32_t`) rather than by descriptor set
and binding. Both `flux_canvas` and `flux_scene` use this path for
textures and uniform data.

## Deferred resource destruction (retire queue)

A released `flux_image`, `flux_buffer`, `flux_mesh`, or `flux_target`
may still be referenced by batches in flight on the graphics queue —
frames recorded before the release. Destroying the view/image/buffer
or freeing its memory at release time can fault the engine mid-batch
(observed on i915 as a GPU hang and context reset, surfacing to hosts
as a fence timeout followed by `VK_ERROR_DEVICE_LOST`).

Release therefore never destroys Vulkan objects inline. It parks the
pieces (view, image, buffer, allocation, imported memory, bindless
slots) on a device-wide retire queue tagged with one past the current
graphics submission serial. Every graphics-queue submission is
assigned a monotonically increasing serial at `vkQueueSubmit2`; every
event that proves a batch complete (the per-slot fence wait in
`flux_surface_begin_frame`, a deferred upload's fence observed by the
pending-upload sweep, `flux_device_wait_idle`) raises a completed
watermark. Because the queue is FIFO, a zombie is destroyed as soon
as the watermark covers its tag. The tag is one past the current
counter so a frame that is still recording when the release happens
is covered too.

Two resource families are exempt because slot fencing already covers
them: transient-ring slices (a slot's fence is awaited before its
cursor resets) and per-frame-slot canvas attachments (destroyed only
when rebuilding for a slot whose frame already completed).

Host consequence: releasing a resource is always safe, at any point in
the frame. Destruction (and bindless slot recycling) trails the
release by at most a couple of frame periods, and
`flux_device_release` drains everything once the device is idle.

A related threading rule: `vkDeviceWaitIdle` / `vkQueueWaitIdle` count
as host access to the queues, so every wait-idle in the library goes
through `flux_vk_wait_idle`, which holds the same `queue_lock` that
serialises `vkQueueSubmit2`. An unsynchronised wait racing a
concurrent submit was observed to double-free inside the Intel ANV
driver.

## Pipeline cache

`flux_device` creates a single `VkPipelineCache` on startup and shares
it across every module that calls `vkCreateGraphicsPipelines` or
`vkCreateComputePipelines`. Within a single process the cache lets a
duplicate pipeline build (e.g. recreating a canvas on the same surface
format) hit Vulkan's own dedup path.

Cross-session persistence is **consumer-owned**, modelled on Skia's
`PersistentCache`. flux never touches the filesystem: it owns the
in-memory `VkPipelineCache`, and the consumer owns the storage
strategy. Wire two callbacks into `flux_device_desc`:

```c
flux_pipeline_cache_file cache = FLUX_PIPELINE_CACHE_FILE_INIT;
flux_pipeline_cache_file_set_default_path(&cache, "myapp.bin");

flux_device_desc ddesc = FLUX_DEVICE_DESC_INIT;
ddesc.pipeline_cache_load     = flux_pipeline_cache_file_load;
ddesc.pipeline_cache_save     = flux_pipeline_cache_file_save;
ddesc.pipeline_cache_userdata = &cache;
```

- `pipeline_cache_load` is called once at `flux_device_create`. It
  returns a `malloc`'d seed blob (freed by the library) or NULL to
  start cold.
- `pipeline_cache_save` is called once at `flux_device_release`,
  after all GPU work has drained, with the blob from
  `vkGetPipelineCacheData`. The library owns `data` for the call.

Leave all three NULL and the cache lives only for the device lifetime
— the default. The file-backed helper above ships in
`examples/pipeline_cache.h` for copy-paste; the env-var cascade
(`$FLUX_PIPELINE_CACHE`, `$XDG_CACHE_HOME`, `$HOME`) lives in that
example helper, not in the library. This restores the contract
ADR-0001 set out: *"the consumer persists; flux provides the in/out
hooks."*

## GPU memory allocator

Every `VkDeviceMemory` flux hands out goes through
`src/core/vk_allocator.c`. Pools are keyed by
`(memory_type, is_image_pool, has_dev_addr)`; each pool is a linked
list of fixed-size blocks (default 64 MiB) with a per-block free
list. Allocation walks the free list of the matching pool, splits
on first-fit, and coalesces with adjacent ranges on free. Requests
≥ 16 MiB (the default dedicated threshold) skip pooling entirely and
get a standalone `VkDeviceMemory` — as does any resource the driver
flags `requiresDedicatedAllocation` or `prefersDedicatedAllocation`
(via `VkMemoryDedicatedRequirements`), with
`VkMemoryDedicatedAllocateInfo` chained onto the resource. The
allocator is thread-safe behind one mutex; mappings are established
once per host-visible block at creation time. See ADR-0007 for the
design trade-offs and ADR-0020 for the hardening pass.

OOM behaviour: when a pool block allocation fails — or when the
`VK_EXT_memory_budget` heap query (enabled automatically when
advertised) says a new block would cross the heap budget — the
allocator first reclaims every empty block back to the driver and
retries once before reporting `FLUX_ERROR_OUT_OF_MEMORY`
(`VK_ERROR_OUT_OF_DEVICE_MEMORY` and `VK_ERROR_OUT_OF_HOST_MEMORY`
map to it uniformly). Empty blocks are also returned eagerly when a
sibling block of the same pool key exists, and on demand via
`flux_vk_allocator_reclaim`.

Observability: `flux_device_memory_stats` returns a point-in-time
snapshot (bytes in use, bytes reserved, live allocations/blocks,
lost ranges); dma-buf imports and exports are counted too, so the
teardown leak warning sees them. When `VK_EXT_debug_utils` is
available (enabled whenever the instance advertises it), pool
blocks, dedicated allocations, staging buffers, the transient ring,
and public buffers/images carry debug names into RenderDoc and
validation output.

## Staging cache

One-shot upload and readback helpers
(`flux_vk_upload_to_buffer/image`, `flux_surface_read_pixels`) copy
through host-visible staging buffers drawn from a per-device cache
rather than a fresh `vkCreateBuffer` + `vkAllocateMemory` per call.
Upload submissions are deferred (see ADR-0022): a staging buffer stays
checked out on the pending list until the copy's fence signals, then
the non-blocking sweep returns it to the cache's idle list. Entries
match by usage and smallest-fit capacity; the cache is capped at
64 MiB per device and drained at device teardown.

## Batched uploads

For bulk asset loading, `flux_uploads_begin` / `flux_uploads_flush`
collapse any number of uploads into a single graphics-queue
submission. While a batch is open, `flux_image_create`,
`flux_image_update_region`, `flux_mesh_create`,
`flux_buffer_create` with `initial_data`, and layout transitions
record their barriers and copies into one device-global command
buffer; flush submits it once — without waiting — and the checked-out
staging buffers recycle once the batch's fence signals. Recording is
serialised by `upload_lock` while the staging memcpy runs outside it,
so loader threads still overlap.

A resource created inside a batch is ordered before any later
same-queue work once flush returns.
`flux_surface_begin_frame` auto-flushes any open batch before
recording, so a frame can never sample unsubmitted data; non-frame
consumers (compute dispatch, readback) flush explicitly, and
`flux_device_release` flushes a leaked-open batch at teardown.
Batches always record on the graphics queue — same-queue implicit
ordering against in-flight frames is what makes live-image updates
safe, and it needs no QFOT plumbing. Uploads outside a batch are
deferred exactly the same way (and keep the dedicated transfer queue
when one exists). See ADR-0021 and ADR-0022.

## Async transfer queue

When the physical device exposes a queue family with `TRANSFER` but
not `GRAPHICS`/`COMPUTE`, `flux_device_create` opens a queue from it
and the one-shot upload helpers (`flux_vk_upload_to_buffer`,
`flux_vk_upload_to_image`) route the copy through the transfer
queue. The destination resource is acquired on the graphics queue
with a queue-family-ownership barrier so subsequent graphics-side
use sees fully visible data. On adapters without a dedicated
transfer family the helpers fall back to a single submit on the
graphics queue. Both paths are deferred from the caller's view —
the helper returns once the batches are submitted, and their
resources recycle when the parked graphics fence signals.

## See also

- [ADR-0001 — project foundations](../adr/0001-project-foundations.md)
- [ADR-0020 — GPU memory production hardening](../adr/0020-gpu-memory-production-hardening.md)
- [ADR-0021 — batched uploads and quiescent waits](../adr/0021-batched-uploads-and-quiescent-waits.md)
- [ADR-0022 — deferred upload submission](../adr/0022-deferred-upload-submission.md)
- [ADR-0002 — per-module device state](../adr/0002-per-module-device-state.md)
- [ADR-0003 — bindless handle packing](../adr/0003-bindless-handle-packing.md)
- [Application architecture](application-architecture.md) — where the
  modules fit in the diagram above.
