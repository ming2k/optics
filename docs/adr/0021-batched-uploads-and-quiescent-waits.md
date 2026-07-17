# ADR-0021: Batched uploads, surface-scoped quiescent waits, prefers-dedicated floor

- Status: Accepted
- Date: 2026-07-17

## Context

ADR-0020 hardened the allocator but left three design-level debts it
explicitly deferred:

1. **Synchronous one-shot uploads.** Every `flux_vk_upload_to_buffer` /
   `flux_vk_upload_to_image` did staging → record → submit → fence-wait
   → destroy. Bulk asset loading (N images/meshes) therefore cost N
   queue submissions and N full pipeline stalls — functionally correct
   but a throughput bottleneck no production engine accepts.
2. **Device-wide stalls at surface teardown.** `flux_surface_release`
   and `flux_surface_resize` called `vkDeviceWaitIdle`, freezing
   unrelated surfaces and queues behind the one being torn down.
3. **`prefersDedicated` honoured unconditionally** (ADR-0020 item 5).
   Some drivers set the preference liberally; dedicating every small
   allocation would burn the per-process `vkAllocateMemory` count the
   pool exists to protect (the 4096 `maxMemoryAllocationCount` floor
   from ADR-0007).

## Decision

1. **Public batched uploads: `flux_uploads_begin` /
   `flux_uploads_flush`.** While a batch is open, the upload helpers
   (`flux_image_create`, `flux_image_update_region`,
   `flux_mesh_create`, `flux_buffer_create` with `initial_data`, and
   layout transitions) record their barriers and copies into one
   device-global command buffer instead of submitting individually;
   flush ends and submits it once, waits the fence, and returns every
   checked-out staging buffer to the staging cache (ADR-0020 item 7).
   Semantics:
   - A resource created inside a batch is usable once flush returns.
   - `flux_surface_begin_frame` auto-flushes any open batch before
     recording, so an unflushed batch can never be sampled by a frame.
     Non-frame consumers (compute dispatch, readback) flush explicitly.
   - `flux_device_release` flushes a leaked-open batch during teardown.
   - Recording is serialised by `upload_lock`; the staging memcpy stays
     outside the lock so parallel loader threads still overlap.
   - Batches always record on the **graphics queue**: same-queue
     implicit ordering against in-flight frames is what makes
     live-image updates safe (see the comment in
     `flux_vk_upload_to_image`), and it avoids QFOT plumbing. The
     dedicated-transfer-queue path remains for synchronous
     (batch-less) initial uploads.
   - Without a batch, uploads stay synchronous exactly as before.
2. **Surface-scoped quiescent waits.** Teardown now waits only what
   the surface's own destruction requires:
   - *Offscreen*: the per-slot `in_flight` fences. Offscreen images
     and the transient ring are referenced only by this surface's
     graphics-queue batches (one-shot uploads and readbacks are
     synchronous and already complete), so slot fences prove quiescence
     with zero stall of unrelated work.
   - *Windowed*: `vkQueueWaitIdle` on the graphics queue under
     `queue_lock` — the presentation engine can keep reading a
     presented image after the frame fence signals, and only a queue
     wait covers that. Still narrower than the previous device-wide
     wait (transfer queue and other devices keep running).
3. **`prefersDedicated` floor.** Honoured only at or above
   `FLUX_VK_PREFER_DEDICATED_MIN` (1 MiB). `requiresDedicated` stays
   unconditional (spec-mandated). This amends ADR-0020 item 5.

## Consequences

Positive:

- Bulk loads cost one submission and one wait instead of N; loader
  threads keep parallel staging memcpy.
- Releasing or resizing one surface no longer freezes the others;
  offscreen teardown is stall-free for the rest of the device.
- Allocation-count pressure from liberal `prefersDedicated` drivers is
  bounded while large-resource preferences are still honoured.

Negative / accepted:

- The batch API relaxes create semantics from "complete on return" to
  "complete by flush"; the frame-begin auto-flush and teardown flush
  cover the realistic misuse paths, but a caller that creates an image
  in a batch and samples it without any frame or flush reads undefined
  content (documented on the API).
- Batched initial uploads forgo the dedicated transfer queue; on
  systems with one, sync uploads may still win for a single very large
  resource. Mixed use is allowed per call site.

## See also

- ADR-0007 (slab allocator), ADR-0020 (hardening pass this builds on).
- `libs/flux/src/core/oneshot.c` (batch core),
  `libs/flux/src/core/surface.c` (`surface_wait_quiescent`),
  `libs/flux/src/core/vk_allocator.c` (dedication floor).
- `tests/flux/integration/test_upload_batch.c` — pixel-asserted
  correctness, auto-flush, and steady-state accounting.
