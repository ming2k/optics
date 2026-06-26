# ADR-0013: Offscreen rendering is a surface mode, not a new object

- Status: Accepted
- Date: Stage 9

## Context

Every render path in flux terminates in a `flux_surface`: the canvas
and scene modules draw into a `flux_frame`, and frames only come from
`flux_surface_begin_frame`. A surface requires a caller-created
`VkSurfaceKHR` — a window. Consequences:

- **No render-to-texture.** A compositor or thumbnailer that wants
  flux output in an image, not on screen, cannot get it.
- **Canvas and scene output is untestable on a headless host.** The
  compute and effect modules have GPU integration tests with byte
  read-backs; the two drawing modules — the bulk of the library —
  have only CPU-side unit tests against stubbed submission.

Two shapes were considered for the fix:

| Approach | Cost |
|---|---|
| New `flux_render_target` object + parallel frame API | Duplicates the per-frame machinery (command pools, fences, transient ring, timestamps) that `flux_surface` already owns; canvas/scene would need a second binding seam |
| Offscreen mode on `flux_surface` | Reuses all per-frame machinery; canvas/scene work unchanged; the swapchain-specific steps (acquire, present, binary semaphores) are skipped |

## Decision

**`flux_surface_desc.vk_surface_khr == NULL` selects an offscreen
surface.** `width`/`height` (both required non-zero) fix the extent.
The surface owns `frames_in_flight` color images
(`VK_FORMAT_R8G8B8A8_UNORM`, `COLOR_ATTACHMENT | TRANSFER_SRC |
TRANSFER_DST | SAMPLED`) allocated through the slab allocator; image
index == frame slot, so the existing per-slot fence already serialises
reuse.

The frame lifecycle keeps its public shape with three internal
deviations:

- `flux_surface_begin_frame` skips `vkAcquireNextImageKHR`; the image
  index is the frame slot.
- `flux_frame_submit` transitions the image to
  `TRANSFER_SRC_OPTIMAL` instead of `PRESENT_SRC_KHR` and submits
  with the fence only — no acquire/present semaphores exist to wait
  on or signal.
- `flux_frame_present` presents nothing: it advances the frame ring
  and returns `FLUX_OK`. Callers keep the begin → draw → submit →
  present loop verbatim, so code written against a window runs
  against an offscreen surface unchanged.

**Readback is synchronous and explicit:**
`flux_surface_read_pixels(s, dst, bytes)` waits for the most recently
submitted frame, copies its image into a transient staging buffer via
a one-shot command buffer, and memcpys into `dst` as tightly packed
RGBA8 (`width * height * 4` bytes). It is valid only on offscreen
surfaces; windowed surfaces return `FLUX_ERROR_UNSUPPORTED`.

`flux_surface_resize` recreates the offscreen images at the new
extent under the same device-idle contract as the swapchain path.

## Consequences

- Canvas and scene gain headless GPU integration tests with pixel
  assertions — the first time their full GPU path (pipelines,
  transient ring, vertex pulling) is exercised by the test suite
  without a window.
- The offscreen format is fixed at RGBA8 UNORM in v1. An
  HDR/float-format offscreen target is additive later (a `format`
  field on the desc) without breaking this contract.
- `flux_surface_get_info` reports `hdr = false` and the actual image
  count; `flux_surface_vk_swapchain` returns `VK_NULL_HANDLE`,
  which raw-Vulkan interop callers must already handle for a
  minimised window.
- One slot per frame means a caller that never calls
  `flux_frame_present` (the ring never advances) keeps rendering
  into the same image; that is already true of the windowed path.
