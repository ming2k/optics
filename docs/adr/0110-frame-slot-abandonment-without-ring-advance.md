---
id: ADR-0110
title: "Frame-Slot Abandonment Without Ring Advance and Region-Clamped Backdrop Sampling"
status: accepted
date: 2026-10-15
scope: libs/flux, bindings/flux-rs, docs/reference
superseded_by: null
negative_knowledge: true
---

# ADR-0110: Frame-Slot Abandonment Without Ring Advance and Region-Clamped Backdrop Sampling

- Status: Accepted
- Date: 2026-10-15
- Scope: `libs/flux` (`core/frame.c`, `include/flux/core.h`, `effect/shaders/effect_backdrop.comp`), `bindings/flux-rs` (`crates/flux`).
- Amends: [ADR-0013](0013-offscreen-surface.md), [ADR-0091](0091-drawing-group-and-effect-semantics.md).

## Context

Two defects surfaced while Tessera (an external GPU compositor built on `flux`)
hardened its KMS scanout path against the `EBUSY` atomic-flip races documented
in its own presentation ADRs.

1. **No way to release a submitted frame without advancing the offscreen
   ring.** `flux_frame_present` was the only transition out of
   `FLUX_FRAME_STATE_SUBMITTED`. On an offscreen surface it advances
   `current_frame` to the next ring slot. Tessera cannot call it until KMS has
   *accepted* its atomic flip, because the dma-buf it exported is the buffer
   currently being flipped. When `commit_scanout` returns `EBUSY` (the CRTC
   non-blocking commit slot is occupied), Tessera has a fully recorded,
   `vkQueueSubmit2`-submitted offscreen frame whose external flip failed. The
   only two exits were:

   - call `present()` anyway → the ring rotates into the buffer the CRTC may
     still be scanning out, producing torn frames on the next retry;
   - drop the raw `flux_frame` pointer → the surface-owned borrow leaks; the
     slot's fence is never signalled, `frame_active` stays `true`, and the
     surface wedges permanently (`flux_surface_begin_frame` returns
     `FLUX_ERROR_INVALID_STATE` forever).

   The C++/Rust bindings made this worse: `SubmittedFrame::present(self)`
   consumed `self` by value, so "I might not present" had no representation at
   all — an early return or a panic between `submit` and `present` leaked the
   borrow with no diagnostic.

2. **Backdrop effect sampling bled outside the capture region.** The
   `effect_backdrop.comp` compute shader derived `uv` from the *output* image
   size only and sampled the input at that coordinate. When the capture is a
   sub-region of the output (a dock-sized glass footprint inside a 4K
   framebuffer), every tap near the region edge sampled texels that belong to
   the region *outside* the padded blur footprint, wrapping neighbouring desktop
   content into the glass edge as a bright seam.

## Decision

1. **Add an explicit abandon transition.** `flux_frame_abandon` releases the
   frame-slot borrow without presenting and without advancing
   `current_frame` (`libs/flux/src/core/frame.c`). `SubmittedFrame` in
   `bindings/flux-rs/crates/flux/src/lib.rs` gains `abandon()` and a `Drop`
   implementation so an unused, unpresented frame is always released
   deterministically:

   ```c
   void flux_frame_abandon(flux_frame *f) {
       if (!f || !f->surface || f->state != FLUX_FRAME_STATE_SUBMITTED)
           return;
       flux_surface *s = f->surface;
       if (!s->offscreen)
           s->needs_recreate = true;
       s->frame_active = false;
       f->state = FLUX_FRAME_STATE_PRESENTED;
   }
   ```

   `present()` and `abandon()` both null the raw pointer before forwarding, so
   `Drop` is a no-op after either transition.

2. **Reset the offscreen export semaphore when the previous submission
   signalled it without exporting its SYNC_FD.** An abandoned external flip
   leaves the per-slot `render_finished` binary semaphore signalled but with no
   exported handle. Re-submitting onto a signalled binary semaphore is invalid
   Vulkan. `flux_surface_begin_frame` now destroys and recreates the semaphore
   with `VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT` on the next begin for
   that slot, once `in_flight` has retired (`libs/flux/src/core/frame.c`).

3. **Clamp backdrop taps to the captured region.** `sample_input` takes
   `uv_min`/`uv_max` derived from the push-constant `origin_*` and
   `region_*` and clamps every tap, so the 4-tap `mode == 0` and 12-tap
   `mode != 0` kernels never read outside the region that was actually captured
   (`libs/flux/src/effect/shaders/effect_backdrop.comp`).

## Alternatives Considered

- **Require present(), and mark the surface for recreation on retry.** The
  pre-existing `needs_recreate` flag re-imports the scanout and rebuilds the
  swapchain-free offscreen ring. Rejected: recreating every image and
  semaphore on each `EBUSY` micro-retry (Tessera retries at 1 ms) turns a
  transient CRTC contention into a per-frame allocation storm, and the ring
  still advanced into the buffer under scanout, so the torn-frame window
  remained open.
- **Hide the frame in a `ManuallyDrop` and expose only an explicit `abandon`.**
  Rejected: any `?`, early return, or panic between `submit` and `present`
  leaks the borrow and wedges the surface with no diagnostic. Only a `Drop`
  guard makes the release unavoidable.
- **Set `needs_recreate = true` unconditionally in `flux_frame_abandon`.**
  Rejected: offscreen surfaces have no swapchain to recreate (ADR-0013); the
  flag only has meaning for swapchain surfaces. Guarding on `!s->offscreen`
  keeps the abandon path allocation-free on the offscreen ring.
- **Tighten the backdrop UV to the padded 128-pixel bucket bounds.** Rejected:
  the bucket is an allocation high-water mark, not content. Clamping to it
  still samples the padding, which is where the bleed originates; only the
  logical region extent is content.

## Consequences

- `flux_frame_abandon` is a new exported symbol and must appear in
  `docs/reference/symbols.md` (`tools/gen_symbols.py` regenerates the table
  from the installed headers). Pre-1.0 minor-revision allowances apply.
- Tessera can now order its scanout commit before `frame.present()` and abandon
  on `EBUSY`, keeping the offscreen ring slot free for the retry.
- The backdrop effect no longer wraps neighbouring content into glass edges at
  sub-region captures; the cost is one `clamp` per tap.
- `SubmittedFrame` no longer requires the caller to remember to consume it; an
  unpresented frame releases its slot at scope exit.

## Verification & Coverage Boundaries

- `tests/flux/integration/test_offscreen.c` asserts the abandon state machine
  and slot reuse. It is mutation-sensitive: making `flux_frame_abandon` a no-op
  leaves `frame_active` set and the follow-up `begin_frame` returns
  `FLUX_ERROR_INVALID_STATE`, failing the test.
- `tests/flux/integration/test_dmabuf.c` runs an abandon/retry cycle on an
  *exportable* offscreen surface. Its reachability of the semaphore-reset branch
  was confirmed by an instrumented probe (the branch is entered on this host),
  so the cycle genuinely executes the reset path and would catch a wedge or a
  crash there.
- The reset's *Vulkan-correctness* aspect (never signalling an
  already-signalled binary semaphore) is only observable with the validation
  layers active, so `tests/flux/integration/test_validation.c` runs the same
  cycle on a device that enables both validation and the dma-buf features
  (`test_helpers_make_validation_dmabuf_device`).
- **Known boundary**: that assertion is inert on hosts without
  `VK_LAYER_KHRONOS_validation` installed. `flux_device_create` proceeds
  without the layer, so `test_helpers_make_validation_device` and
  `make_validation_dmabuf_device` now probe the layer list directly and return
  `nullptr` when it is absent — which makes `test_validation` *skip honestly*
  instead of passing vacuously. On such hosts the reset is verified by
  construction and by the reachability probe only.
