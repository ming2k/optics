# ADR-0103: Platform-Agnostic Accessibility Bridge, Asynchronous Transport, and Lazy Screen-Reader Activation

- Status: Accepted
- Date: 2026-09-27
- Scope: `libs/iris` (`a11y.h`, `a11y_internal.h`, `a11y_atspi.c`, `a11y_stub.c`, backends), `libs/lens` (semantic walk contract).
- Depends on: [ADR-0035](0035-lens-accessibility-tree.md), [ADR-0043](0043-iris-foundations.md), [ADR-0055](0055-watch-apis-and-wakeup-seam.md), [ADR-0062](0062-lens-bidirectional-a11y-action-and-text-changed.md), [ADR-0075](0075-system-accessibility-preferences.md).

## Context

Accessibility (a11y) in native desktop applications links high-performance UI widget trees to operating system assistive technologies (screen readers, magnifiers, switch access, and accessibility test harnesses).

In our existing implementation, three major architectural defects and conceptual leaks emerged:

1. **Unbounded Synchronous Startup Block on the Main UI Thread:**
   During window startup before the Wayland/native event loop began, `iris_a11y_init()` executed synchronous D-Bus IPC (`sd_bus_call_method`) to register with `org.a11y.atspi.Registry`. In environments where the AT-SPI registry service was absent or non-activatable, the call blocked the main thread for the default 25-second D-Bus timeout (`SD_BUS_DEFAULT_TIMEOUT`). Consequently, surface buffers were never committed, the compositor received no initial frame, and application windows failed to appear for nearly half a minute.

2. **Platform and IPC Leaks in the Public Header (`<iris/a11y.h>`):**
   Although the C function signatures in `<iris/a11y.h>` (`iris_a11y_init`, `iris_a11y_update`, `iris_a11y_shutdown`) were syntactically generic, the header documentation and conceptual contract explicitly hardcoded Linux AT-SPI D-Bus specifics (`libsystemd`, `at-spi2-atk`, `org.a11y.atspi.Event.Object`, `"siiva{sv}"` wire signatures). A library designed for long-term cross-platform parity (Linux Wayland, Windows Win32, macOS Cocoa) must never define its public contract in terms of a single platform's IPC mechanism.

3. **Transport Coupling and Implementation Asymmetry:**
   The internal integration point in `src/a11y_internal.h` (`iris_a11y__fd`, `iris_a11y__poll_events`, `iris_a11y__pump`) assumed a pollable POSIX file descriptor. While fitting for Linux `poll()` loops, this model is an impedance mismatch for Windows UI Automation (COM callback interfaces) and macOS `NSAccessibility` (Objective-C protocols). As a result, Windows and macOS were relegated to compiling inert stubs (`a11y_stub.c`) with architectural disconnects.

4. **Unconditional Per-Frame Diff Overhead:**
   Even when no assistive technology client was active on the system, `iris_a11y_update()` was invoked every frame after `lens_end()`, walking the semantic node tree, copying strings, and computing tree diffs, wasting memory bandwidth and execution time.

## Decision

To establish an uncompromised, future-proof, and truly platform-neutral accessibility bridge, we adopt the following architectural invariants:

### 1. Complete Purification of Public `<iris/a11y.h>`

The public header `<iris/a11y.h>` is stripped of all platform-specific terminology, D-Bus wire signatures, and Linux-specific library references. The public contract represents a pure abstraction:
- **`iris_a11y_init()`**: Initializes host accessibility bridge connectivity asynchronously and fail-soft.
- **`iris_a11y_update(lens *ui)`**: Reconciles the live `lens` semantic tree with the platform's accessibility hierarchy.
- **`iris_a11y_is_active()`**: Queries whether assistive technology clients or screen readers are actively consuming the semantic tree.
- **`iris_a11y_shutdown()`**: Tears down platform accessibility resources safely.

### 2. The Zero-Blocking Invariant on Window Startup and Main Thread

No accessibility initialization or registration path may ever block synchronously on remote daemons or external IPC:
- **Direct Environment Fast-Path:** On Linux, `AT_SPI_BUS_ADDRESS` is evaluated first. If set (standard in Flatpak, containerized environments, and modern desktop sessions), session-bus querying is bypassed entirely (0 IPC roundtrips).
- **Tight Fail-Fast Session Timeouts:** Broker queries on the user session bus enforce a strict 50ms timeout (`sd_bus_set_method_call_timeout(session, 50000)`). If the session broker is unresponsive, the bridge degrades immediately to a contract-safe no-op rather than blocking UI presentation.
- **Asynchronous Service Registration:** Registration with external registries (e.g. `org.a11y.atspi.Socket.Embed`) is dispatched via asynchronous non-blocking calls (`sd_bus_call_method_async`). The message is queued and flushed via the normal event loop without pausing frame dispatch.

### 3. Lazy Activation and Zero-Cost-When-Idle Contract

Accessibility services follow a strictly gated, zero-allocation policy:
- **Desktop State Gating:** Before allocating D-Bus connections or registering polling file descriptors, the bridge evaluates system state (`org.a11y.Status.ScreenReaderEnabled` / `IsEnabled` on Linux, `SPI_GETSCREENREADER` on Windows).
- **Inert Degradation:** When neither a screen reader nor desktop accessibility is enabled (and no explicit opt-in such as `GTK_A11Y`, `AT_SPI_BUS_ADDRESS`, or `IRIS_A11Y_FORCE` is present), the bridge remains completely inert (`g_a11y_bus = NULL`, `pl.a11y_fd = -1`). Zero sockets are opened, zero background polling occurs, and zero D-Bus traffic is generated.
- **Single-Site Frame Delivery:** Per-frame reconciliation `iris_a11y_update(ui)` is executed strictly once per frame directly following `lens_end()`. Duplicate historical call sites within presentation branches (`lens_snapshot_activate`) across Wayland, Win32, and Cocoa are completely removed.
- **Dynamic Activation:** When a screen reader is active or invoked, `iris_a11y_is_active()` reports true and full semantic reconciliation proceeds without dropping frames.

### 4. Decoupled Platform Adapters

The accessibility bridge separates pure UI semantics (`lens`) from platform-native accessibility object graphs:
- **Linux Adapter (`a11y_atspi.c`):** Implements AT-SPI2 over D-Bus. Pollable fd integration remains private to the Linux backend loop.
- **Windows Adapter (`a11y_win32.c` / `a11y_stub.c`):** Interfaces with Microsoft UI Automation (`IRawElementProviderSimple`, `IRawElementProviderFragment`), mapping `lens_role` and `lens_semantics` directly into UIA control patterns.
- **macOS Adapter (`a11y_cocoa.m` / `a11y_stub.c`):** Implements `NSAccessibilityElement` trees directly responding to VoiceOver queries.

## Alternatives Considered

- **Embedding a Third-Party Intermediate Bridge (e.g. AccessKit):**
  *Rejected for core library footprint.* Pulling in large third-party runtime dependencies would violate the zero-external-dependency discipline of `libiris` and `liblens`. The tripartite architecture of Optics already possesses pure semantic nodes (`lens_role`, `lens_semantics`), making a direct, lightweight C bridge architecturally superior and zero-overhead.

- **Background Worker Thread for AT-SPI Dispatch:**
  *Rejected for concurrency complexity.* `iris_a11y_update` reads `lens` semantic trees that are per-frame arena-allocated on the main thread. Pumping D-Bus on a background thread would require mutex locking or heavy snapshot cloning per frame. Keeping the async pump on the main event loop preserves thread-affinity and lock-free execution.

## Consequences

### Positive
- **Instantaneous Window Startup:** Application startup latency drops from ~25.5s down to <0.5s under missing or non-activatable accessibility registries.
- **Clean Public Boundary:** `<iris/a11y.h>` contains zero platform leakage, allowing equal first-class status for Windows UIA, macOS NSAccessibility, and Linux AT-SPI.
- **Zero Idle Overhead:** Idle applications consume zero CPU cycles on accessibility diffing when screen readers are not running.

### Negative
- Asynchronous registration means the root accessible is linked into the desktop hierarchy within the first few loop iterations rather than strictly before the first `while(pl.running)` cycle, which is standard behavior across GTK4 and Qt6.
