---
id: ADR-0168
title: "Exported dma-buf images carry a lease, not a caller-discipline lifetime"
status: accepted
date: 2026-11-18
scope: gfx/core, gfx/bindings
superseded_by: null
negative_knowledge: true
---

# 0168. Exported dma-buf images carry a lease, not a caller-discipline lifetime

- Status: Accepted
- Date: 2026-11-18

## Context

An exportable offscreen surface owns a small ring of Vulkan images. Exporting
one image transfers queue-family ownership to the foreign dma-buf consumer. The
C API previously documented that the host must not call `begin_frame` for that
slot until the consumer released the buffer, but the library had no way to
enforce or observe that contract. A compositor that flips one union image to
multiple CRTCs at different rates can therefore reuse an image while a slow
CRTC still scans it, or wait globally for every CRTC even though only one slot
is still externally owned.

This is a Flux responsibility: only Flux knows which ring slot an exported
image occupies and when the slot can be reacquired. KMS, Wayland buffer
release, and output pacing stay host responsibilities.

## Decision

`flux_surface_export_dmabuf` and `flux_surface_export_dmabuf_explicit` return an
opaque `flux_surface_dmabuf_lease` together with the descriptors. The lease:

- retains its surface;
- keeps the image slot out of the ring until
  `flux_surface_dmabuf_lease_release`;
- makes `flux_surface_begin_frame` for that slot return
  `FLUX_ERROR_BUSY` while held;
- makes `flux_surface_resize` return `FLUX_ERROR_BUSY` while any lease is
  outstanding.

`flux_surface_dmabuf_lease_release` is called by the external consumer's
completion path: a DRM page-flip event, a Wayland `wl_buffer.release`, or an
explicit-sync release fence. The host may hold one lease per exported image and
share it across several planes; the last reference releases the ring slot.

A persistent slot-table consumer (the compositor's ScreenCast transport) has a
different contract: it publishes one fd per ring slot and orders each use with
the per-frame explicit sync fence. `flux_surface_export_dmabuf_pooled` and
`_pooled_explicit` expose that contract explicitly **without** a lease; they are
not a loophole in the leased path.

The host-facing Rust binding exposes `DmabufLease` and `ExportedDmabuf`; the
lease is `Drop`-released. A compositor can therefore put one lease in an `Arc`
shared by every CRTC scanning the same union image and release it only when the
last `PlaneSlot` retires.

## Consequences

- Image lifetime is correct without a global "wait for every CRTC" gate.
- Export is now fallible with `FLUX_ERROR_BUSY`; a compositor retries on its
  backend event instead of blocking.
- Persistent stream slot tables that intentionally export a ring once and reuse
  it must explicitly release the one-shot lease after taking the fd; their
  synchronization remains the per-frame explicit fence.
- The old caller-discipline contract is removed (breaking C ABI and Rust
  binding change); callers must pass and eventually release a lease.
