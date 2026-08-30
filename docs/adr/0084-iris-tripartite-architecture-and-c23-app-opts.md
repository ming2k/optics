# ADR-0084: Iris tripartite architecture and C23 app-opts descriptor

- Status: Accepted
- Date: 2026-08-26
- Scope: `iris` (OS platform shell & application runtime), `iris-rs` (Rust bindings). Extends ADR-0043, ADR-0044, ADR-0082, ADR-0083.

## Context

`iris/app.h` served as the cross-platform application harness across Wayland, Win32, and Cocoa. Over time, window configuration options, visual preferences, host callbacks (`start`, `build`, `paint`, `stop`), and platform service hooks accumulated into loosely structured fields.

## Decision

1. **Tripartite Architecture Separation**:
   Decompose the platform runtime into three strictly orthogonal layers:
   - **Physical Window & Lifecycle**: Manages native window handles, Vulkan swapchains, and DPI scale notifications.
   - **Pure Event Pump**: Translates OS events into a clean, unidirectional `iris_event` snapshot stream with zero business logic.
   - **Modular Host Services**: Pluggable bridges for System Clipboard, Text-Input (IME v3), and Assistive Technology (AT-SPI / UIA).

2. **Single C23 Application Descriptor (`iris_app_opts`)**:
   Unify application launch into a single canonical entry point:
   ```c
   typedef struct iris_window_opts {
       const char *title;
       uint32_t width;
       uint32_t height;
       bool resizable;
       bool borderless;
       bool transparent;
   } iris_window_opts;

   typedef struct iris_app_opts {
       iris_window_opts window;
       bool dark_theme;
       float default_ui_scale;
       iris_build_fn build;
       iris_paint_fn paint;
       iris_init_fn init;
       iris_cleanup_fn cleanup;
       void *user;
   } iris_app_opts;

   [[nodiscard]] IRIS_API int iris_app_run(const iris_app_opts *opts);
   ```

3. **Full C23 Baseline**:
   Apply native `nullptr`, `{}`, fixed underlying enums, and `[[nodiscard]]` across all `iris` public headers and internal modules.

## Consequences

### Positive
- Unified, symmetric API pattern across the entire Optics stack (`flux` ➔ `lens` ➔ `iris`).
- Simplified cross-platform event pump with no platform-specific leaks into userland.
- Clean integration with `flux_canvas_draw` and `lens` 4-layer UI model.
