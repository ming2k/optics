---
id: ADR-0124
title: "Decoupled dual-thread UI and presentation engine"
status: accepted
date: 2026-10-28
superseded_by: null
scope: libs/iris, libs/flux, architecture
negative_knowledge: true
---

# ADR-0124: Decoupled dual-thread UI and presentation engine

- Status: Accepted
- Date: 2026-10-28
- Scope: `libs/iris/include/iris/app.h`, `libs/iris/src/app.c`, `libs/iris/src/app_wayland.c`, `libs/iris/src/app_win32.c`, `libs/iris/src/app_cocoa.m`, `libs/flux/`.
- Implementation: **Landed on Wayland with active presentation worker thread.** Landed: `libs/iris/src/frame_mailbox.{h,c}` implementing the wait-free triple-buffered snapshot handoff channel (`iris_frame_mailbox`), proven by multithreaded concurrent producer-consumer stress tests (`tests/iris/test_frame_mailbox.c`); and dedicated presentation worker thread (`wp_presenter_thread_main`) in `app_wayland.c` consuming immutable snapshots asynchronously without blocking the UI main event pump (`[INV-THREAD-01]`, `[INV-THREAD-02]`, `[INV-THREAD-03]`, `[INV-THREAD-04]`). Remaining: worker thread dispatch integration for Win32 and Cocoa.
- Depends on: [ADR-0002](0002-per-module-device-state.md), [ADR-0043](0043-iris-foundations.md), [ADR-0089](0089-rendering-program-and-execution-plan.md), [ADR-0090](0090-display-list-resource-ownership.md), [ADR-0122](0122-compositor-driven-presentation-pacing.md), [ADR-0123](0123-layered-compositing-subsurfaces-and-damage-tracking.md).

## Context

Historically, Optics centralized all execution into a single-threaded event loop (the "God Loop" in `app_wayland.c`, `app_win32.c`, and `app_cocoa.m`). Within a single `while (running)` loop, the engine sequentially:
1. Pumped OS platform socket events and native window messages;
2. Processed raw keyboard, pointer, IME, and clipboard inputs;
3. Updated accessibility trees via D-Bus / platform bridges;
4. Executed user logic and immediate-mode layout (`lens_begin` / `cfg->build` / `lens_end`);
5. Performed host canvas rendering (`cfg->paint`);
6. Acquired Vulkan swapchain images via `vkAcquireNextImageKHR`;
7. Recorded command buffers and submitted them to the GPU queue;
8. Called `vkQueuePresentKHR`.

This monolithic single-threaded architecture created an unresolvable concurrency paradox:
- If the loop blocks inside `vkAcquireNextImageKHR` or `vkQueuePresentKHR` waiting for display VBlank (standard FIFO presentation), input processing is completely frozen during that interval. High-frequency mouse movement and text IME pre-edits stutter, and the window fails to answer compositor ping requests.
- To avoid this freezing, Optics disabled VSync (`.vsync = false`) and resorted to client-side timer polling, which caused the severe phase drift, micro-stutter, and 100% CPU busy-spin failures documented in ADR-0122.
- Furthermore, if user application layout or host build logic takes longer than expected (e.g. 5 ms for a complex document or table re-layout), presentation is delayed, causing immediate frame drops even though the GPU was completely idle.

Industry-standard production graphics engines (Chromium's Viz, Flutter's Shell, Android's Choreographer/RenderThread, macOS CoreAnimation) resolve this by decoupling the application/UI lifecycle from the presentation/rasterization lifecycle.

## Decision

Optics adopts a **Decoupled Dual-Thread Architecture**, establishing a strict boundary between the **UI Event Thread** and the **Compositor / Presentation Thread**:

```text
┌─────────────────────────────────────────────────────────────────────────────┐
│                           UI EVENT THREAD (Thread 0)                        │
│  - Native OS Event Pump (Wayland poll, Win32 GetMessage, Cocoa RunLoop)      │
│  - High-frequency pointer smoothing & IME state tracking                    │
│  - Accessibility tree reconciliation (A11y D-Bus / UIA / NSAccessibility)   │
│  - Host Application logic & Lens Immediate-Mode UI Build                    │
│  - Produces: Immutable Scene Snapshot (flux_display_list / flux_layer_tree) │
└──────────────────────────────────────┬──────────────────────────────────────┘
                                       │ Lockless Snapshot Handoff
                                       │ (Atomic Pointer Exchange / Mailbox)
                                       ▼
┌─────────────────────────────────────────────────────────────────────────────┐
│                   COMPOSITOR / PRESENTATION THREAD (Thread 1)                │
│  - Display Pacing Synchronization (wl_surface.frame, CAMetalDisplayLink)    │
│  - Owns Vulkan Swapchain (VkSwapchainKHR, VkSurfaceKHR)                     │
│  - Non-blocking vkAcquireNextImageKHR with Timeline Semaphores              │
│  - Compiles RenderPlans & dispatches GPU Command Buffers                    │
│  - Executes vkQueuePresentKHR locked to Hardware VBlank                     │
└─────────────────────────────────────────────────────────────────────────────┘
```

### 1. Concurrency Contract & Ownership Separation

- **Thread 0: UI / Event Thread (Platform Main Thread):**
  - Affinity: Bound strictly to the operating system's main thread (required by Cocoa's `NSApplication`, Win32 window message queues, and Wayland display handles).
  - Responsibilities: Owns `lens_context`, widget states, accessibility bridges, and host callbacks (`start`, `build`, `stop`).
  - Output: Compiles the resolved UI into an immutable `flux_display_list` and submits it to the thread-safe handoff channel.
  - Non-blocking Guarantee: The UI thread **never touches `VkQueue`, `VkSwapchainKHR`, or driver presentation primitives**. It can never be stalled by GPU backpressure or display VBlank.

- **Thread 1: Compositor / Presentation Thread:**
  - Affinity: High-priority worker thread created during `iris_app_run`.
  - Responsibilities: Owns the Vulkan presentation queue, swapchain creation/recreation, image acquisition, command buffer submission, and display-link synchronizers.
  - Input: Consumes the latest available snapshot from the handoff channel. If the UI thread hasn't produced a new snapshot in a given refresh cycle, the compositor presents the existing cached scene or skips presentation cleanly without stalling the display.
  - Synchronization: Operates under strict hardware VSync cadence without impacting user interaction responsiveness.

### 2. Lockless Snapshot Handoff Channel

Communication between the UI thread and Presentation thread operates without mutex contention using a triple-buffered atomic mailbox:
```c
typedef struct iris_frame_mailbox {
    _Atomic(flux_layer_tree *) ready_tree; /* Written by UI, read by Compositor */
    _Atomic(flux_layer_tree *) recycle_tree;
    uint64_t latest_sequence;
} iris_frame_mailbox;
```
- When the UI thread finishes building a frame, it atomically publishes the new `flux_layer_tree` snapshot via `atomic_exchange_explicit(..., memory_order_release)`.
- The Compositor thread claims the latest snapshot via `atomic_exchange_explicit(..., memory_order_acquire)`.
- Old snapshots are recycled back to the UI thread's per-frame scratch arena, achieving zero heap allocation and zero mutex overhead in the presentation hot path.

### 3. Timeline Semaphore Synchronization

GPU synchronization leverages Vulkan 1.3 `VK_KHR_timeline_semaphore`:
- Swapchain image acquisition signals an acquisition timeline value:
  ```c
  VkAcquireNextImageInfoKHR acquire_info = {
      .sType = VK_STRUCTURE_TYPE_ACQUIRE_NEXT_IMAGE_INFO_KHR,
      .swapchain = surface->vk_swapchain,
      .timeout = UINT64_MAX,
      .semaphore = surface->timeline_acquire_sem,
      ...
  };
  ```
- Command buffer submission automatically waits on the acquisition timeline without halting host CPU execution.
- Command buffer completion signals the present timeline value, allowing `vkQueuePresentKHR` to proceed asynchronously.

### 4. Deterministic Teardown Handshake

Window destruction and application exit adhere to a fail-soft four-stage teardown protocol:
1. **Signal Stop:** The UI thread signals `running = false` and posts an eventfd/wakeup to the Compositor display link.
2. **Drain Presentation:** The Compositor thread finishes the current presenting cycle, calls `vkQueueWaitIdle` on the presentation queue, and signals completion.
3. **Join Thread:** The UI thread joins the Compositor thread.
4. **Host Release:** Host resources (`cfg->stop`), Lens contexts, and Vulkan devices are destroyed sequentially on the main thread in reverse order of initialization, guaranteeing zero use-after-free or dangling GPU references.

## Invariants & Behavioral Boundaries

- **`[INV-THREAD-01]` Non-Blocking UI Event Loop:** User input dispatch (mouse, keyboard, touch, IME), layout reconciliation, and accessibility tree updates must never block on GPU presentation queues, swapchain image acquisition (`vkAcquireNextImageKHR`), or display vertical blank waits.
- **`[INV-THREAD-02]` Lockless Immutable Snapshot Exchange:** Communication between the UI thread and the Presentation thread must proceed via double-buffered/triple-buffered lockless ownership transfer of immutable `flux_display_list` / `flux_layer_tree` handles.
- **`[INV-THREAD-03]` Device Presentation Affinity:** Only the Compositor thread is permitted to invoke `vkAcquireNextImageKHR`, `vkQueueSubmit`, and `vkQueuePresentKHR` on swapchain surfaces. The UI thread must never issue Vulkan presentation commands.
- **`[INV-THREAD-04]` Fail-Soft Teardown Protocol:** Surface and device destruction must execute an explicit thread-join and idle-drain handshake, guaranteeing zero in-flight GPU executions or dangling device memory accesses upon process termination.

## Rejected Alternatives & Negative Knowledge

- **Option A — Retaining a single-threaded loop with non-blocking `vkAcquireNextImageKHR(timeout=0)`.**
  *Why considered:* Avoids introducing threading and synchronization complexity.
  *Why rejected:* `timeout=0` immediately returns `VK_NOT_READY` if an image is not instantly available. The loop is forced to poll repeatedly or back off with heuristic sleeps, reintroducing the exact phase-beat micro-stutter and high CPU consumption that plagued the original architecture.
- **Option B — Running the OS Event Pump on a secondary background thread.**
  *Why considered:* Offloads OS message handling while keeping rendering on the main thread.
  *Why rejected:* Fundamentally rejected by OS windowing contracts. macOS Cocoa strictly requires `NSApplication` and all window handling to occur on the process main thread (`Thread 0`). Win32 window message queues have strict thread affinity. Wayland's `wl_display` and xdg-shell protocols require single-thread dispatch. Moving the event pump to a background thread causes instant crashes or undefined behavior across all target operating systems.
- **Option C — Coarse-grained Mutex locking around the Swapchain.**
  *Why considered:* Simplifies data sharing between threads.
  *Why rejected:* Mutex contention between a 1000 Hz input pump and a 60/120 Hz presentation loop causes priority inversions and thread convoying. Any momentary lock contention freezes user interaction. Lockless atomic snapshot exchange is mandatory.
