# Thread Safety

## TL;DR

- **`flux_device` is free-threaded** after `flux_device_create` returns.
  Any combination of public calls against the device — resource
  creation, refcount ops, Vulkan handle accessors — is safe from any
  thread. The device-owned bindless heap, memory allocator, and queue
  submissions are internally locked.
- **`flux_surface`, `flux_frame`, `flux_canvas`** are thread-confined.
  The thread that begins a frame must drive that frame through to
  submit + present.
- **No parallel command-buffer recording.** flux does not expose
  secondary command buffers. Within one frame, all draw recording
  happens on the thread that called `flux_surface_begin_frame`.
  Multi-threaded scene building belongs in caller code that writes
  to per-thread arenas, with all `flux_canvas_*` / `flux_scene_draw_*`
  calls funnelled to the recording thread.

## Per-type matrix

| Concept                  | Thread-safe?                                                      |
|--------------------------|-------------------------------------------------------------------|
| `flux_device_*`          | Free-threaded. Refcount is atomic. The bindless heap, GPU memory allocator, queue submission path, and lazy default sampler are all internally locked. Vulkan handle accessors are lock-free reads of immutable post-init fields. |
| `flux_surface_*`         | Refcount is atomic. **Surface operations are not thread-safe**: `begin_frame`, `resize`, etc. must be serialised by the caller. |
| `flux_frame_*`           | Single-threaded only. A `flux_frame` returned by `begin_frame` must be consumed (submit + present) on the same thread that began it. |
| `flux_image_*`           | Refcount is atomic. `_create` performs a deferred one-shot upload (returns after submission; ordered before later same-queue work); the queue submit is internally locked, so concurrent creates from worker threads are safe. |
| `flux_canvas_*`          | Single-threaded only. Tied to one surface and one frame at a time. Per-canvas geometry scratch is isolated, so two canvases on two threads is supported as long as each thread drives its own canvas + its own frame. There is no API for parallel command-buffer recording into a single frame. |
| `flux_path_*`            | Value type owned by a `flux_arena`. Not refcounted, not thread-safe — the owning arena is single-threaded. |
| `flux_paint`             | Plain POD value. Pass it by `const flux_paint *`. No lifecycle. |
| `flux_mesh_*`            | Refcount is atomic. `_create` performs a deferred one-shot upload; same queue lock as `flux_image`. Safe to call concurrently from worker threads. |
| `flux_material_*`        | Refcount is atomic. Immutable after creation. |
| `flux_scene_draw_mesh*`  | Single-threaded only. The draw records into the active frame's command buffer; mix only with the thread that began the frame. |
| `flux_buffer_*`          | Refcount is atomic. Creation is safe to call concurrently from worker threads (uses the device queue lock for any staging upload). Once created, GPU-side reads/writes follow Vulkan's external-synchronisation rules. |
| `flux_sampler_*`         | Refcount is atomic. Creation and release are safe to call concurrently. |
| `flux_graphics_pipeline_*`| Refcount is atomic. Creation is safe to call concurrently (relies on the device's `VkPipelineCache` and bindless layout, both immutable post-init). Binding is single-threaded per frame. |
| `flux_compute_pipeline_*`| Refcount is atomic. Creation is thread-safe; dispatches are single-threaded per command buffer. |
| `flux_arena_*`           | Single-threaded only. No internal locking. |
| `flux_log_fn`            | May be invoked from any thread that calls into flux. Callback must be reentrant-safe. |
| `flux_get_last_error`    | Per-thread storage; each thread sees its own last error. |

## What "single-threaded only" means

The state behind the handle is mutated without locks. Two threads
making calls against the same handle is undefined behaviour even if the
calls are sequential by wall-clock time.

The caller can still use flux from multiple threads, with these rules:

| You want to...                              | Approach                                          |
|---------------------------------------------|---------------------------------------------------|
| Render on a background thread               | Create the surface on that thread; only call frame/canvas functions from there. |
| Build paths on multiple threads             | Give each thread its own `flux_arena` and `flux_path`. |
| Upload images concurrently                  | Safe: uploads check out staging buffers from a locked pool and the queue submit is internally locked (ADR-0022). Worker threads may upload in parallel on one device. |
| Drive two windows                           | One surface per window. Submits from different threads are safe: `vkQueueSubmit2`/`vkQueuePresentKHR` are serialised by the device's queue lock (the same lock that makes worker-thread uploads sound). |

## Vulkan external synchronisation

The Vulkan handles flux returns inherit Vulkan's "externally
synchronised" rules. In particular, `VkQueue` is externally
synchronised, so two threads calling `flux_frame_submit` against
different surfaces backed by the same device must serialise the submits
themselves.
