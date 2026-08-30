# Platform Backend & OS Integration Governance (Iris)

This document details the governance criteria for platform windowing, event loops, and host services in `libs/iris` (formalizing ADR-0043, ADR-0044, ADR-0052, ADR-0056).

---

## 1. Tripartite Separation Model

Platform integration is strictly decomposed into three non-overlapping responsibilities:

1. **Window & Physical Lifecycle**:
   - Manages native window handles, Vulkan swapchains, and DPI scale notifications across Linux (Wayland/X11), macOS (Cocoa/MoltenVK), and Windows (Win32).
2. **Unified Event Pump**:
   - Translates raw OS window/keyboard/pointer events into uniform `iris_event` snapshots. No business logic or widget interpretation is performed inside the event loop.
3. **Host Service Bridges**:
   - Completely modular interfaces for system clipboard, text-input (IME text-input-v3), and assistive technology (AT-SPI, UIA).

---

## 2. Invariants

- **Compile-Time Backend Selection**: Target platforms are resolved at configure/build time (ADR-0044); no runtime abstraction overhead or dynamic driver switching.
- **Single Opts Entry Point**: `iris_app_run(&(iris_app_opts){ ... })` serves as the canonical application entry point.
