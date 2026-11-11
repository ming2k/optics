---
id: ADR-0127
title: "Decoupled Host Canvas Recording and Hardware-Locked Presentation Pacing"
status: accepted
date: 2026-11-01
scope: libs/iris, libs/flux, libs/lens, architecture
superseded_by: null
negative_knowledge: true
---

# ADR-0127: Decoupled Host Canvas Recording and Hardware-Locked Presentation Pacing

- Status: Accepted
- Date: 2026-11-01
- Deciders: Optics Architecture Working Group, Engine Core, Platform Teams
- Amends: [ADR-0122](0122-compositor-driven-presentation-pacing.md), [ADR-0123](0123-layered-compositing-subsurfaces-and-damage-tracking.md), [ADR-0124](0124-dual-thread-ui-and-presentation-pipeline.md)

---

## Context and Problem Statement

ADR-0124 established the decoupled dual-thread pipeline (UI Event Thread 0 vs Compositor Presentation Thread 1) and proved the lockless triple-buffered snapshot handoff channel (`iris_frame_mailbox`). However, two foundational bottlenecks remained in the production stack:

1. **Host Canvas Single-Thread Fallback Paradox**: The legacy host drawing callback `iris_paint_fn(flux_canvas *canvas, ...)` exposed a live Vulkan swapchain canvas synchronously inside an open render pass. Because calling Vulkan queue submissions on Thread 0 violates `[INV-THREAD-03] Device Presentation Affinity`, applications supplying `cfg->paint` were forced to fall back to a single-threaded synchronous loop on Win32 and Cocoa, defeating dual-thread smoothness for hybrid applications.
2. **Software Push vs Hardware Pull Jitter**: The presenter worker thread relied primarily on software event signals from the UI thread with heuristic sleep timeouts (`refresh_ns * 1.5`). In Windows and macOS environments, this failed to lock step with the hardware display controller's vertical blank (VBlank) interrupt, introducing phase-beat micro-stutter and display refresh rate misalignments (e.g. 120 Hz ProMotion displays falling back to 60 Hz).

---

## Architectural Decisions

1. **Decoupled Host Canvas Recording Seam (`iris_record_fn` & `flux_display_list`)**:
   - Replaced synchronous swapchain canvas drawing with a clean-break decoupled recording contract:
     ```c
     typedef void (*iris_record_fn)(flux_encoder *enc, float scale, void *user);
     ```
   - On the UI thread (Thread 0), the host records arbitrary vector paths, glyph runs, brushes, and layers into a CPU-bound `flux_encoder` without acquiring GPU swapchains or touching Vulkan queues.
   - The encoder freezes into an immutable `flux_display_list` handle, attached to `iris_frame_packet.host_list`, and handed off across `iris_frame_mailbox` without locks.
   - The presentation worker thread (Thread 1) submits both `host_list` and `snapshot` inside a single unified Vulkan canvas pass, guaranteeing zero Vulkan commands on the UI thread and full dual-thread decoupled execution across all platforms (Wayland, Win32, Cocoa).

2. **DWM Hardware VBlank Lockstep Synchronization (Win32)**:
   - Linked `dwmapi` natively in `libs/iris/meson.build`.
   - Replaced coarse integer display refresh queries with `DwmGetCompositionTimingInfo`, deriving exact rational frame durations (nanosecond precision) from hardware timing registers.
   - Presenter thread invokes `DwmFlush()` upon successful swapchain presentation, locking step with the hardware DWM composition cycle.

3. **ProMotion 120Hz Hardware Refresh Discovery (Cocoa)**:
   - In `cocoa_query_refresh_ns`, integrated dynamic `maximumFramesPerSecond` detection on modern Apple displays, automatically unlocking 120 Hz (8.33 ms cadence) pacing.

4. **Universal Swapchain Dynamic Resize Affinity**:
   - Delegated all `flux_surface_resize` and `FLUX_ERROR_SURFACE_LOST` recovery exclusively to the presentation worker thread, upholding `[INV-THREAD-03]` across all backends.

---

## Invariants & Behavioral Boundaries

- **`[INV-THREAD-05]` Zero Vulkan Execution in Host Recording:** Calling `cfg->record(enc, scale, user)` must remain 100% CPU-bound; `flux_encoder` commands never invoke Vulkan RHI calls, swapchain acquires, or driver locks.
- **`[INV-PACE-05]` Nanosecond Rational Pacing:** Pacer refresh durations must derive from native compositor timing registers (`DWM_TIMING_INFO`, `maximumFramesPerSecond`, `wp_presentation_feedback`), never hard-coded integer approximations.

---

## Consequences

- **Positive:** Hybrid applications combining custom vector graphics with Lens UI chrome achieve 100% decoupled dual-threaded presentation across Wayland, Windows, and macOS without swapchain stalls or single-thread fallbacks.
- **Verification:** All guardrails (`./tools/optics check`), MinGW/Zig cross-compilation checks, and all 151 unit/integration tests pass cleanly.

## Amendment — 2026-11-10

The replacement is now complete rather than optional: `iris_paint_fn` and the
`iris_app_opts.paint` field are removed, `iris_record_fn` is the only
host-canvas path, and the fallback path (used when `prepare` requires the main
thread) submits the recorded display list through the Canvas. There is no
second synchronous host-canvas route to keep the two implementations aligned.
