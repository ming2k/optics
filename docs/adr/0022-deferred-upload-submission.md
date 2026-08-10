# ADR-0022: Deferred upload submission (amends ADR-0021 item 1)

- Status: Accepted
- Date: 2026-07-18

## Context

ADR-0021 item 1 introduced batched uploads but kept synchronous
semantics everywhere: `flux_uploads_flush` ended the batch with a
submit and a fence wait, and every upload outside a batch
(`flux_image_create` with `initial_data`, `flux_image_update_region`,
`flux_mesh_create`, `flux_buffer_create` with `initial_data`, one-shot
layout transitions, and the dma-buf acquire-fence transition) submitted
and waited individually.

A fence wait on the graphics queue covers everything submitted before
it, because the queue is FIFO. In a compositor driving flux — the
primary consumer — the previous frame's render batch is frequently
still in flight when a client commits new content. Each texture upload
then stalled the render thread until the prior frame finished on the
GPU, collapsing the CPU/GPU pipeline to depth one and surfacing as
periodic missed vblanks (visible jank) at exactly the moments the
screen changes: scrolling, typing, video, window transitions. The
dma-buf acquire-fence transition was worse: the wait covered the
*producer's* GPU work, so explicit-sync video frames blocked the
compositor on the client.

The waits existed to make resource lifetimes trivial: a staging buffer
could return to the cache and a transient command pool could be
destroyed as soon as the call returned.

## Decision

All upload submissions are **deferred**: the copy or transition batch
is submitted with a fence and the caller returns immediately.
Correctness comes from queue submission order, not from a host wait —
every later batch on the same queue (the frame being recorded,
readback) is ordered after the copies, so a resource is safe to sample
as soon as its creating call returns.

Lifetime safety moves to a device-level pending list. Each deferred
submission parks its fence, command pools, QFOT handoff semaphores,
checked-out staging buffers, and its graphics submission serial
(ADR-0020's retire watermark). A non-blocking sweep at the upload entry
points recycles entries whose fence already signaled and advances the
completed watermark, so retire-queue zombies keep moving outside the
frame path. `flux_device_memory_stats` and device teardown drain the
list with real waits, keeping diagnostics deterministic and teardown
complete.

Semantics that change from ADR-0021 item 1:

- `flux_uploads_flush` no longer waits; "usable once flush returns"
  becomes "ordered before any later same-queue work", which is what
  the realistic consumers (frames, readback) already relied on.
- Uploads outside a batch are no longer synchronous; the batch API now
  exists purely to coalesce many copies into one submission.
- The transfer-queue QFOT path for initial uploads submits both halves
  deferred; the single parked graphics fence covers both batches
  through the handoff semaphore.
- The dma-buf acquire-fence transition no longer blocks the caller on
  the producer's fence; the wait happens GPU-side, and later
  same-queue work is ordered after the transition.

## Alternatives

- **Keep the waits (status quo).** Rejected: the stall is proportional
  to unrelated prior GPU work, which is exactly the frame-time profile
  a compositor cannot accept during continuous client updates.
- **Record uploads into the frame's own command buffer** (a staging
  belt fed by the surface's transient ring). This removes the extra
  submissions entirely and is the likely end state for per-frame
  compositor texture updates, but it requires uploads to happen inside
  a frame on the render thread. The upload API is device-level and
  callable from worker threads outside frames; deferred submission
  keeps that contract. Deferred as a follow-up for the hot path.
- **Per-upload worker threads.** Rejected: submissions are serialized
  by `queue_lock`, so threads only move the staging memcpy, which was
  never the bottleneck.

## Consequences

Positive:

- Texture updates during continuous client redraw no longer collapse
  the render pipeline; worst-case frame time stops depending on how
  busy the GPU was when a client committed.
- Explicit-sync dma-buf clients (video) no longer block the compositor
  on the producer's fence.
- Staging buffers are retired against real completion signals, so the
  staging cache keeps working without per-upload allocation churn.

Negative / accepted:

- Resource recycling is lazy: staging buffers and command pools live
  until the next sweep after their fence signals. Sweeps run at every
  upload entry point and at frame begin, so steady-state overhead is a
  few small allocations; `flux_device_memory_stats` drains first so
  accounting stays deterministic.
- A submission failure that follows a successful transfer-queue submit
  must drain the transfer queue before recycling (error path only);
  the code comments mark the spot.

Follow-up work:

- Frame-integrated uploads (staging belt into the frame command
  buffer) for the compositor's per-frame texture refresh path.

## See also

- ADR-0021 (batched uploads; item 1 amended here), ADR-0020 (retire
  watermark), ADR-0007 (slab allocator).
- `libs/flux/src/core/oneshot.c` (deferred submit, pending list),
  `libs/flux/src/core/dmabuf.c` (acquire-fence transition; moved out of
  `src/canvas/` with the canvas-gate fix — see ADR-0052).
- `tests/flux/integration/test_upload_batch.c` — auto-flush and
  steady-state accounting under deferred submission.
