---
id: ADR-0122
title: "Compositor-driven presentation pacing and hardware-locked display synchronization"
status: accepted
date: 2026-10-28
superseded_by: null
scope: libs/iris, libs/transit, libs/lens, architecture
negative_knowledge: true
---

# ADR-0122: Compositor-driven presentation pacing and hardware-locked display synchronization

- Status: Accepted
- Date: 2026-10-28
- Scope: `libs/iris/include/iris/app.h`, `libs/iris/src/display_pacer.{h,c}`, `libs/iris/src/app_wayland.c`, `app_win32.c`, `app_cocoa.m`, `platform_internal.h`, `libs/transit`.
- Implementation: **Landed across all three backends (Wayland, Win32, Cocoa).** Landed: the platform-neutral pacing state machine (`libs/iris/src/display_pacer.{h,c}`, exercised by `tests/iris/test_display_pacer.c`); dynamic display refresh discovery across Wayland (`wl_output.mode` / `wp_presentation`), Win32 (`EnumDisplaySettings`), and Cocoa (`CGDisplayModeGetRefreshRate`); `wl_surface.frame` grants as the Wayland loop's clock; the skip-render scheduling regression (0 ms poll spin eliminated across all three backends); and the simulation/presentation timestamp split (INV-PACE-01..04). Hardcoded `ACTIVE_PERIOD_NS` and `last_render_ns` are completely eradicated from all backends. Remaining: optional waitable-swapchain and display-link async drivers for Win32/Cocoa.
- Depends on: [ADR-0043](0043-iris-foundations.md), [ADR-0044](0044-iris-backend-selection.md), [ADR-0055](0055-watch-apis-and-wakeup-seam.md), [ADR-0084](0084-iris-tripartite-architecture-and-c23-app-opts.md).
- Supersedes: Decision #2 of [ADR-0056](0056-win32-cocoa-backends-and-moltenvk.md) (the client-side software timer loop).

## Context

Optics previously adopted a client-driven presentation pacing loop across Wayland, Win32, and Cocoa. To prevent `vkAcquireNextImageKHR` from blocking the main thread during VSync waits, the engine forced `.vsync = false` (Mailbox/Immediate present mode) and paced itself using client-side monotonic timestamps (`now_ns()`) and `poll(timeout)` sleep periods, hardcoded to an assumed 60 Hz active rate (`ACTIVE_PERIOD_NS = 16666667LL`).

Production deployment on Linux Wayland (and subsequently on high-refresh-rate macOS and Windows environments) exposed three systemic failures:

1. **Phase Drift & Beat-Frequency Micro-Stutter:** The client CPU oscillator (`CLOCK_MONOTONIC`) and the display hardware VBlank timing drift continuously. Even with negligible render times (< 1 ms), frames submitted in Mailbox mode drift relative to the compositor's latching deadline. When submission misses a latch by 0.1 ms, the frame is deferred by a whole cycle; the subsequent cycle may push two frames in rapid succession, silently discarding the earlier frame in the mailbox. Users perceive this not as steady frame drops, but as erratic phase-beat jitter during smooth scrolling and animations.
2. **High-Refresh-Rate Display Degradation:** Modern displays operate at 120 Hz, 144 Hz, 240 Hz, or Variable Refresh Rate (VRR). Hardcoding active pacing to ~60 Hz artificially crippled display hardware capabilities and produced telecine judder (uneven 2:1/3:1 frame cadences) against smooth system desktop compositors.
3. **Deadlock / 0ms Poll CPU Runaway on Skip-Render:** In `app_wayland.c`, `last_render_ns` was dual-purposed as both an input throttle anchor and the deadline baseline for subsequent animation requests (`next_deadline = last_render_ns + ACTIVE_PERIOD_NS`). When downstream applications (such as media players or visualizers) declared a zero-render skip (`iris_request_frame_skip_render`), no actual GPU presentation occurred. Consequently, `last_render_ns` remained frozen in the past. On the subsequent iteration, `next_deadline` was calculated against this historical timestamp, yielding a deadline already in the past. The loop entered `poll(..., 0)` busy-spinning at 0 ms intervals, completely consuming a CPU core (100% single-core load) for an operation intended to conserve power.

## Decision

Optics formally abandons client-side software timer pacing and adopts a **Compositor-Driven, Display-Locked Presentation Architecture**:

```text
Display Hardware VBlank / Compositor Frame Feedback
  -> Platform VSync Callback (wl_surface.frame, CAMetalDisplayLink, Waitable Swapchain)
  -> iris_display_pacer: on_vsync(target_presentation_time, frame_interval)
  -> Update Transit Physics & Animations using predicted target_presentation_time
  -> Build UI & Render Plan
  -> Submit & Present locked to Hardware Refresh
```

### 1. Unified Display Pacer Interface (`iris_display_pacer`)

Replace the ad-hoc `poll(budget)` deadline calculations with a platform-abstracted VSync driver:

```c
typedef struct iris_vsync_pulse {
    uint64_t frame_index;
    int64_t target_presentation_ns; /* Projected hardware scanout time */
    int64_t refresh_interval_ns;    /* Current display refresh period */
    uint32_t display_refresh_hz;    /* e.g. 60, 120, 144, 240 */
} iris_vsync_pulse;

typedef void (*iris_vsync_fn)(const iris_vsync_pulse *pulse, void *user_data);
```

### 2. Platform-Native Display Synchronization Backends

- **Wayland (`app_wayland.c`):**
  - Bind `wl_surface.frame` on each committed frame. The compositor fires the callback when it is optimal to begin rendering the next frame.
  - Integrate `wp_presentation_feedback` (presentation-time protocol) to sample precise hardware presentation timestamps, refresh cycle timing, and zero-latency feedback.
  - Query output refresh rate dynamically via `xdg_output` or `wl_output.mode` rather than assuming 60 Hz.
- **macOS (`app_cocoa.m`):**
  - Integrate `CAMetalDisplayLink` on macOS 14+ (falling back to `CVDisplayLink` on older releases), natively synchronizing frame dispatch with Apple ProMotion (up to 120 Hz dynamic switching).
- **Windows (`app_win32.c`):**
  - Create swapchains with `DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT`.
  - Use `GetFrameLatencyWaitableObject()` with `MsgWaitForMultipleObjectsEx` to sleep until the GPU composition queue is ready for the next frame, eliminating manual sleep arithmetic.

### 3. Simulation vs. Presentation Timestamp Orthogonality

Strictly separate **Logical Animation/Simulation Ticks** from **Physical Presentation Times**:
- **Transit & Lens Animations:** Compute physics integration and spring states using `pulse->target_presentation_ns` (the projected moment the photons leave the display glass), completely eliminating animation jitter caused by intra-frame execution jitter.
- **Skip-Render State Machine Repair:** When `iris_request_frame_skip_render()` is declared, the presentation pass is skipped, but the **scheduling baseline unconditionally advances** by `refresh_interval_ns`.
  $$\text{next\_deadline} = \text{current\_vsync\_timestamp} + \text{refresh\_interval}$$
  Calculation of deadlines against stale physical render timestamps is explicitly prohibited.

### 4. Adaptive Power Management & True Static Quiescence

- **Active State:** Driven synchronously by display VSync pulses while user input or animations are in flight.
- **Idle State (Caret Blink):** When only a text cursor is blinking, register for VSync pulses at sub-multiples (e.g. 1 Hz / 2 Hz blink cadence) or unhook the display link and wait in `poll()` on eventfd/pipe wakeups until input or the blink timer arrives.
- **Quiescent State (Zero Animation):** When the UI is completely static, destroy or pause the frame callback subscription. The main loop blocks unconditionally inside platform event wait (`poll()` on Wayland, `GetMessage` on Win32, `nextEventMatchingMask` on Cocoa) with zero CPU utilization until native OS events occur.

## Invariants & Behavioral Boundaries

- **`[INV-PACE-01]` Zero Client-Timer Presentation:** Window presentation pacing must never be driven by client-side sleeping, busy polling, or hardcoded frame periods (such as `16.6ms` or `poll(budget)`). Pacing must be exclusively driven by display hardware vertical blanking and compositor feedback.
- **`[INV-PACE-02]` Physical Clock Authority:** Wayland surfaces must synchronize via `wl_surface.frame` (and `wp_presentation_feedback`), macOS surfaces via `CAMetalDisplayLink` / `CVDisplayLink`, and Windows surfaces via DXGI waitable swapchains. The physical refresh rate (60 Hz, 120 Hz, 144 Hz, VRR) must be derived from the display engine, never clamped to 60 Hz.
- **`[INV-PACE-03]` Simulation-Present Timestamp Orthogonality:** The simulation/animation tick timestamp must be strictly decoupled from the GPU swapchain presentation timestamp. Zero-render skip (`iris_request_frame_skip_render`) must monotonically advance the scheduling base clock, unconditionally prohibiting negative deadline calculations and 0ms poll spinning.
- **`[INV-PACE-04]` Target Presentation Time for Interpolation:** All continuous motion algorithms (`transit`, spring integration, scroll physics) must evaluate against projected `target_presentation_ns` rather than `clock_gettime(CLOCK_MONOTONIC)`.

## Rejected Alternatives & Negative Knowledge

- **Option A — Patching the `last_render_ns` offset in `app_wayland.c` while retaining the 16.6ms poll timer.**
  *Why considered:* Fixes the 100% CPU runaway bug with a 2-line code change without restructuring the event loop.
  *Why rejected:* Cures the symptom while leaving the root cause intact. The phase drift, beat-frequency micro-stutter, lack of high-refresh-rate display support, and frame aliasing remain fundamentally unaddressed.
- **Option B — Fixed 120 Hz client timer.**
  *Why considered:* Supports high-refresh displays simply by changing `ACTIVE_PERIOD_NS` from 16.6 ms to 8.3 ms.
  *Why rejected:* Doubles power consumption and CPU wakeups on 60 Hz displays. Causes catastrophic beat frequencies and dropped frames on 144 Hz / 165 Hz monitors. Client-side timers cannot synthesize display hardware clock synchronization.
- **Option C — Blocking `vkAcquireNextImageKHR` directly on the main event loop thread without a display link.**
  *Why considered:* Forces hardware VSync synchronization via driver-managed FIFO swapchains.
  *Why rejected:* Reintroduces the exact failure mode ADR-0056 tried to evade: when the compositor holds back buffers (e.g. during window occlusion, resize, or heavy desktop load), `vkAcquireNextImageKHR` blocks the thread indefinitely. Input events cannot be drained, IME candidate popups freeze, and the window becomes unresponsive to OS pings. True display-link pacing notifies the client when to draw without holding blocking driver locks on the event pump.
