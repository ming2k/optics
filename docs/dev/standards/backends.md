# Platform Backend & OS Integration Governance (Iris)

This document details the governance criteria for platform windowing, event loops, and host services in `libs/iris` (formalizing ADR-0043, ADR-0044, ADR-0052, ADR-0056, ADR-0107, ADR-0122, ADR-0124).

---

## 1. Tripartite Separation Model

Platform integration is strictly decomposed into three non-overlapping responsibilities:

1. **Window & Physical Lifecycle**:
   - Manages native OS window handles, Vulkan swapchains, and DPI scale notifications across Linux (Wayland / X11), macOS (Cocoa / MoltenVK), and Windows (Win32).
   - Presentation pacing (ADR-0122) is hardware-locked to compositor display refreshes via Wayland presentation-time, Win32 DwmFlush, or Cocoa CVDisplayLink.
2. **Unified Event Pump**:
   - Translates raw OS window, keyboard, and pointer events into uniform `iris_event` snapshots.
   - Zero business logic or UI widget interpretation is performed inside the event pump.
   - **The Pre-Lens Event Seam (ADR-0107)**: Each event passes the single pre-fold host hook (`iris_app_opts.on_event`) — the only point where an application host may observe, filter, or intercept raw input before surviving events are folded into `lens_input`.
3. **Host Service Bridges**:
   - Completely modular interfaces for system clipboard, text-input / IME (Wayland text-input-v3, Win32 IMM32, macOS NSTextInputClient), and assistive technology (AT-SPI, UIA).

---

## 2. Invariants

1. **Compile-Time Backend Selection (ADR-0044)**:
   - Target platforms are resolved at configure/build time via Meson. No runtime driver branching or dynamic abstraction overhead.
2. **Single Opts Entry Point**:
   - `iris_app_run(&(iris_app_opts){ ... })` serves as the canonical application entry point.
3. **Decoupled Dual-Thread Pipeline (ADR-0124)**:
   - UI layout and event processing are decoupled from the render presentation pipeline via `iris_frame_mailbox`. The UI thread never blocks on GPU vsync fences.
