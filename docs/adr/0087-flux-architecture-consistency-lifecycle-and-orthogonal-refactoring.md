# ADR-0087: Flux Architecture Consistency, Lifecycle Contracts, and Orthogonal Refactoring

- Status: Superseded by ADR-0095
- Date: 2026-08-27
- Scope: `flux` (core, canvas, canvas_cpu, vulkan), `flux-text`, `flux-scene-graph`, downstream UI/compositor consumers (`lens`, `iris`). Extends ADR-0010, ADR-0019, ADR-0083. RFC Reference: RFC-0094.

## Context

Flux is Optics' core hardware-accelerated 2D/3D rendering engine, built upon C23 and Vulkan 1.3 (dynamic rendering, bindless descriptors, buffer device address, and DMA-BUF zero-copy pipelines). Following the adoption of ADR-0083 (which established `flux_shape` and `flux_paint` as the orthogonal primitives), architectural liabilities remained across API boundaries:

1. **Fragmented Lifecycle Dialects**:
   - Refcounted resources used `_retain` and `_release` (`flux_device`, `flux_surface`, `flux_buffer`, `flux_image`, `flux_target`).
   - Major handles used arbitrary `_destroy` terminology (`flux_canvas_destroy`, `flux_text_destroy`, `flux_arena_destroy`), obscuring whether objects supported shared borrowing, internal frame retirement queues, or unique ownership.
   - Display-list handles used `flux_canvas_record_release`.
2. **CPU vs. GPU Polymorphism Fracture**:
   - GPU-backed canvases were driven by `flux_canvas_begin_frame(c, frame, ...)` and `flux_canvas_end_frame(c)`.
   - Headless CPU canvases required `flux_canvas_cpu_begin(c, ...)` and `flux_canvas_cpu_end(c)`.
   - Higher-level UI frameworks (`lens`) and visual test suites could not drive a generic `flux_canvas*` polymorphically across headless snapshots and hardware presentation.
3. **API Bloat in Exported C Symbols**:
   - Despite ADR-0083, legacy specialized convenience entry points (`flux_canvas_fill_rect`, `flux_canvas_fill_rrect`, `flux_canvas_stroke_rrect`, `flux_canvas_draw_image_*`) remained in dynamic export symbol tables, burdening library size and forcing backends to maintain dual routing paths.
4. **Implicit Vulkan Synchronisation & Font Atlas Gaps**:
   - Cross-queue image transitions (e.g. storage images produced by asynchronous compute passes being sampled by `flux_canvas`) lacked explicit Vulkan 1.3 barrier abstractions (`VkImageMemoryBarrier2`).
   - Font rasterisation on the host CPU in `flux-text` lacked an explicit frame-level staging upload and barrier synchronization boundary, risking race conditions against in-flight command buffers.
5. **C23 Ergonomics**:
   - Initialization of descriptor chains with `.type` and `.next` was manual and error-prone, lacking idiomatic type-safe compound literal macros.

## Decision

We enact RFC-0094 across Flux and its satellite libraries with the following architectural rules:

### 1. Strict Dual-Lifecycle Contract

Every engine type is classified into one of two mutually exclusive lifecycle categories. Global use of `_destroy` is strictly prohibited:

1. **Shared Refcounted & Deferred-Retire Types** (`flux_<type>_create`, `flux_<type>_retain`, `flux_<type>_release`):
   - Scope: `flux_device`, `flux_surface`, `flux_target`, `flux_canvas`, `flux_text`, `flux_buffer`, `flux_image`, `flux_graphics_pipeline`, `flux_compute_pipeline`, `flux_sampler`.
   - Existing `_destroy` entry points are deprecated with C23 standard attribute `[[deprecated("Use flux_<type>_release instead")]]` and forwarded to `_release`.
2. **Value-Semantic & Bump Allocator Types** (`flux_<type>_init`, `flux_<type>_reset`, `flux_<type>_deinit`):
   - Scope: `flux_arena`, `flux_path`.
   - `flux_arena_destroy` is deprecated in favor of `flux_arena_deinit`.

### 2. Target-Driven Rendering Polymorphism (`flux_target`)

`flux_canvas` drives all passes exclusively through a unified render target (`flux_target`), removing the need for backend-specific begin/end verbs:

- **Target Kinds**:
  - GPU Swapchain/Framebuffer target: Borrowed reference obtained via `flux_frame_target(frame)`. Lifespan is strictly bounded to the parent `flux_frame`. Calling `flux_target_release` on a borrowed frame target is an assertion violation.
  - CPU host raster target: Owned instance created via `flux_target_create_cpu(desc, &target)`. Exposes rasterized pixels via `flux_target_cpu_pixels`.
  - Offscreen GPU target: Created via `flux_target_create(device, desc, &target)`.
- **Unified Canvas Driver**:
  ```c
  FLUX_NODISCARD FLUX_API flux_result flux_canvas_begin(
      flux_canvas *c,
      flux_target *target,
      const flux_color *clear_color
  );
  FLUX_NODISCARD FLUX_API flux_result flux_canvas_end(flux_canvas *c);
  ```
  Legacy `flux_canvas_begin_frame`, `flux_canvas_end_frame`, `flux_canvas_cpu_begin`, and `flux_canvas_cpu_end` remain supported as deprecated forwarders during the migration window.

### 3. Orthogonal Core & Header-Only Inline Convenience Layer

- Dynamic shared libraries (`libflux.so`, `libflux.dylib`, `flux.dll`) export only atomic, orthogonal primitives:
  - `flux_canvas_draw(flux_canvas *c, const flux_shape *shape, const flux_paint *paint)`
  - `flux_canvas_draw_glyph_run(flux_canvas *c, const flux_glyph_run_desc *desc)`
- Convenience helpers (`flux_canvas_fill_rect`, `flux_canvas_fill_rrect`, `flux_canvas_stroke_rrect`, `flux_canvas_draw_image`, etc.) are declared as `static inline` functions inside `<flux/canvas_helpers.h>`, compiling down to stack compound literals of `flux_shape` at zero runtime or ABI cost.

### 4. Explicit Vulkan Synchronization & Font Atlas Boundaries

- **Explicit Image Barrier Primitive**:
  `<flux/vulkan.h>` introduces `flux_frame_pipeline_barrier` using Vulkan 1.3 synchronization2 semantics (`VkImageMemoryBarrier2`), supporting queue family transfer ownership.
- **Font Atlas Flush & Defensive Guard**:
  - `<flux-text/text.h>` exposes:
    ```c
    FLUX_NODISCARD FLUX_TEXT_API bool flux_text_has_pending_uploads(const flux_text *t);
    FLUX_NODISCARD FLUX_TEXT_API flux_result flux_text_flush_atlas(flux_text *t, flux_frame *f);
    ```
  - In Debug/Validation mode, attempting to draw glyphs while atlas textures have uncommitted pending uploads triggers a non-fatal warning or assertion to guarantee deterministic host-to-device transfers before raster pass submission.

### 5. C23 Macro Safety (`FLUX_INIT`)

`<flux/core.h>` introduces a standardized macro for type-checked, zero-initialized struct construction with auto-stamped type tags and null-terminated `next` pointers:
```c
#define FLUX_INIT(TypeName, ...) \
    ((flux_##TypeName){ \
        .type = FLUX_TYPE_##TypeName, \
        .next = nullptr, \
        __VA_ARGS__ \
    })
```

## Consequences

### Positive
- **Complete Test/Production Parity**: Headless offscreen snapshots and live Vulkan presentation run through the exact same canvas orchestration code.
- **Zero ABI Bloat**: Export symbol counts for `flux_canvas` are cut drastically without compromising developer convenience.
- **Explicit Synchronization**: Multi-queue and font atlas hazards are surfaced at compile-time and frame boundaries rather than hidden deep inside command buffer recordings.
- **Unified Mental Model**: Elimination of `_destroy` removes ambiguity regarding object ownership and retirement semantics.

### Negative & Migration
- **Transitional Deprecation Period**: Callers must update `flux_canvas_destroy` -> `flux_canvas_release`, `flux_arena_destroy` -> `flux_arena_deinit`, and include `<flux/canvas_helpers.h>` where convenience drawing methods are used.
- **FFI Bindings**: Non-C languages that bind directly to shared library symbols must either use `flux_canvas_draw` directly or provide language-native extension wrappers corresponding to `canvas_helpers.h`.
