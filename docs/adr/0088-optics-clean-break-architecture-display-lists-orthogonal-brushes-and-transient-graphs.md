# ADR-0088: Optics Clean-Break Architecture — Immutable Display Lists, Pure Algebraic Geometry-Brush Model, and Transient FrameGraph

- Status: Superseded by ADR-0089 through ADR-0094
- Date: 2026-08-27
- Scope: `flux` (core, canvas, compute, effect), `flux-text`, `flux-scene-graph`, `prism`, `lens`, `iris`, and Rust bindings (`flux-rs`, `lens-rs`, `iris-rs`). Extends ADR-0010, ADR-0019, ADR-0083, ADR-0087.

## Context

Following the consolidation in ADR-0087 (which unified lifecycle semantics into a strict dual-contract and introduced polymorphic render targets), an architectural review identified lingering deep-structure liabilities inherited from legacy 2D immediate-mode canvas paradigms:

1. **Stateful Canvas Bottleneck**: `flux_canvas` was simultaneously an immediate state machine, a geometry processor, and a GPU dispatch gateway. This prevented multi-threaded parallel recording of UI subtrees and prohibited global batch optimization or overdraw culling.
2. **Fat Shape Descriptors vs. Incomplete Materials**: `flux_shape` was a flat struct carrying redundant fields for unused geometry types, while materials (`flux_paint`) were limited to solid/gradient. Advanced optical materials (`prism` liquid glass, frosted sheets) existed as disconnected compute filters rather than first-class brushes.
3. **Canvas-Target Coupling**: Canvases required dimensions at creation time, leading to dual-specification anomalies with polymorphic `flux_target` outputs.
4. **Immediate-Mode UI Coupling**: `lens_render` directly pushed into `flux_canvas`, tightly coupling Flexbox layout traversal with hardware rasterization and preventing headless offline analysis.
5. **Rust Ownership Disparity**: Lack of compile-time separation between owned targets and borrowed swapchain frame targets created risks of double-free or reference escape.

## Decision

We adopt a clean-break, first-principles architecture across the entire Optics stack:

### 1. Separation of Recording and Rasterization (`flux_encoder` & `flux_display_list`)

- **`flux_encoder`**: A pure CPU, memory-only command encoder operating on an arena. It has zero GPU or Vulkan dependencies and can record concurrently on worker threads.
- **`flux_display_list`**: An immutable, compact command stream produced by freezing an encoder. It can be moved across threads, cached across frames, or serialized.
- **`flux_canvas_submit_display_list`**: The hardware rasterizer consumes display lists, performing batch compaction, depth ordering, and submission to the active `flux_target`.

### 2. Pure Algebraic Model: `flux_geometry` $\times$ `flux_brush`

We factor 2D drawing strictly into an algebraic product:
$$\text{Draw Primitive} = \text{Geometry (Boundary / Shape)} \times \text{Brush (Material / Energy)}$$

- **`flux_geometry`**: A compact 32-byte tagged union:
  - `FLUX_GEOM_RECT`: Axis-aligned rectangle.
  - `FLUX_GEOM_RRECT`: Rounded rectangle with per-corner radii.
  - `FLUX_GEOM_SQUIRCLE`: Superellipse with continuous curvature exponent $n$.
  - `FLUX_GEOM_CIRCLE`: Analytical circle.
  - `FLUX_GEOM_LINE`: Stroke line segment.
  - `FLUX_GEOM_PATH`: Pointer to arena-allocated vector contour.
- **`flux_brush`**: A unified material descriptor supporting solid color, linear/radial gradients, image patterns, and advanced optical shaders (frosted backdrop and liquid glass). Any geometry can be rendered with any brush.
- **Glyph Runs**: Explicitly separated from geometry into dedicated `flux_canvas_draw_glyph_run` / `flux_encoder_draw_glyph_run` primitives due to unique font-atlas lifetimes.

### 3. Canvas Dimensionless Decoupling

`flux_canvas_create` no longer accepts width, height, or scale. The canvas acts purely as an execution pipeline context; physical extent, stride, viewport, and color space are dynamically derived from the bound `flux_target` at `flux_canvas_begin`.

### 4. Canvas State Stack & Layer Groups (`save_layer`)

- In addition to `flux_canvas_save` and `flux_canvas_restore` for affine transforms and scissor clips, we introduce:
  ```c
  FLUX_API void flux_canvas_save_layer(flux_canvas *c, const flux_rect *bounds, float opacity);
  ```
- Subtree draws inside a layer group render into a tightly scissored transient offscreen buffer, automatically composited back onto the host target with modulated alpha upon `restore`, eliminating Alpha-overlap penetration artifacts in semi-transparent composite widgets.

### 5. Multi-Frame Font Atlas Isolation

- `flux_text_measure` is strictly read-only and pure CPU.
- Font glyph uploads are ring-buffered or epoch-tagged to prevent host-to-device writes from colliding with in-flight GPU frames (RAW hazards).

### 6. Lens 3-Phase Functional Pipeline

UI execution is cleanly split into three independent stages:
1. **Layout & Diff**: Pure CPU Flexbox computation generating an immutable `lens_scene` containing geometry and layout boxes. Can execute in headless test environments with zero graphics drivers.
2. **Compile & Batch**: Transforms `lens_scene` into an immutable `lens_draw_list` / `flux_display_list` with automatic state deduplication and occlusion culling.
3. **Submit**: Submits the compiled draw list to the target canvas.

### 7. Rust Type-State & Borrowed Target Guarantees

In `flux-rs`:
- `OwnedTarget`: Owns an allocated target, drops via `flux_target_release`.
- `TargetRef<'frame>`: Borrows from `&'frame Frame`, statically prevented from outliving the frame or implementing `Drop`.
- Unified by `AsTarget` trait, ensuring compile-time memory safety without runtime overhead.

## Consequences

### Positive
- **Complete Parallelism**: UI layouts and draw commands can be computed concurrently across multiple CPU threads.
- **Zero Alpha Artifacts**: `save_layer` enables correct opacity compositing for composite UI controls.
- **Predictable Performance**: Decoupled display lists enable global batching, state caching, and zero-redundancy draw calls.
- **Type Safety**: Rust client code cannot trigger double-free or target dangling reference bugs.

### Negative & Migration
- Callers transitioning from direct immediate rendering must adopt the encoder/display list workflow for advanced multi-pass pipelines.
- Existing `flux_canvas_draw` remains supported as an immediate frontend forwarding into the underlying geometry/brush pipeline.
