# Value-Semantic Rendering (VSR) Architecture

This document formalises the rendering architecture of Flux. It defines **Value-Semantic Rendering (VSR)**—the paradigm that replaces the traditional, decades-old dichotomy between "Immediate Mode" and "Retained Mode"—and details its physical data flow, caching model, and alignment with modern GPU hardware.

---

## 1. The Fallacy of the False Dichotomy

For more than two decades, computer graphics and UI frameworks have framed rendering architecture around a rigid binary choice:

```text
               Traditional Retained Mode                  Traditional Immediate Mode
            (e.g., Browser DOM, Qt, WPF)                    (e.g., Classic IMGUI, OpenGL)
          ┌───────────────────────────────┐               ┌───────────────────────────────┐
Coupling: │ Procedural API ➔ State Tree   │     versus    │ Procedural API ➔ Ephemeral    │
          │ (Forced Object Retention)     │               │ (Zero Caching / Full Recompute)│
          └───────────────┬───────────────┘               └───────────────┬───────────────┘
                          │                                               │
                          ▼                                               ▼
          • Massive object memory footprint               • Catastrophic CPU waste on static
          • Complex mutation invalidation bugs              content (re-tessellating 120 FPS)
          • Data races across threads                     • Zero cross-thread recording
```

This dichotomy rests on a fundamental **category error**: it conflates the **API mental model** (how the programmer expresses drawing commands) with the **underlying data lifecycle** (how the engine stores and executes them).

* **Traditional Retained Mode** assumed that in order to cache rendered content, the engine *must* own and maintain a mutable, stateful graph of persistent objects (`Node`, `Element`, `Widget`).
* **Traditional Immediate Mode** assumed that in order to keep the API stateless, procedural, and simple, the engine *must* discard everything every frame and recompute all geometry from scratch.

Flux rejects both assumptions.

---

## 2. Defining Value-Semantic Rendering (VSR)

Flux introduces and implements **Value-Semantic Rendering (VSR)**:

> **Value-Semantic Rendering (VSR)** is an architectural paradigm where drawing operations are expressed via a **stateless, procedural API**, while their execution payloads are captured into **immutable, relocatable, value-semantic command streams**.

In VSR:
1. **There are no "living" engine objects**: The graphics core maintains no stateful widget trees, parent-child links, layout graphs, or mutation listeners.
2. **Commands are immutable values**: A recorded drawing sequence is not an object graph; it is a frozen, contiguous byte slice (`flux_display_list`) with atomic reference counting. Like a string or an integer, it is pure data.
3. **Threading is inherently safe**: Because display lists are immutable, worker threads can record them concurrently and pass them across thread boundaries with zero synchronization locks.
4. **Caching is decoupled from application state**: Caching does not require scene-graph invalidation algorithms. The engine provides lightweight, value-keyed geometry caches and submission caches.

---

## 3. The Orthogonal Primitive Foundation (`Geometry × Brush`)

A core pillar of VSR is the complete decoupling of **spatial shape** from **optical material**. In Flux, every 2D draw operation converges onto a single orthogonal entry point:

```c
FLUX_API void flux_canvas_draw_geometry(flux_canvas *c,
                                        const flux_geometry *geom,
                                        const flux_brush *brush);
```

### Separation of Concerns

* **`flux_geometry` (Spatial Boundary & Topology)**:
  * Shape Kind: `Rect`, `RRect`, `Squircle` (G2 superellipse), `Circle`, `Line`, `Path`.
  * Stroke Style: `width` (0 = fill, > 0 = stroke), `cap` (butt, round, square), `join` (miter, round, bevel), and `miter_limit`.
  * *Purity*: Geometry knows nothing about colors, textures, or shaders. It only dictates CPU tessellation or GPU distance fields.

* **`flux_brush` (Optics & Material)**:
  * Material Kind: `Solid` color, `LinearGradient`, `RadialGradient`, `ImagePattern`.
  * Optical Attributes: `blend` mode (`SRC_OVER`, `SRC`, `PLUS`, `MULTIPLY`) and `opacity` ($0.0 \sim 1.0$).
  * *Purity*: Brush knows nothing about bounding boxes or line caps. It is packaged directly into GPU push constants.

### Zero-Cost Ergonomic Layer

To provide immediate-mode convenience without dynamic symbol table bloat, `<flux/canvas_helpers.h>` provides `static inline` functions that compile down to stack-allocated C23 compound literals:

```c
// High-level call:
flux_canvas_fill_rect_color(canvas, rect, 0xFF00FF00u);

// Expands at compile-time with zero runtime overhead into:
flux_canvas_draw_geometry(canvas,
    &(flux_geometry){ .kind = FLUX_GEOM_RECT, .rect.rect = rect },
    &(flux_brush){ .kind = FLUX_BRUSH_SOLID, .solid.color = 0xFF00FF00u, .opacity = 1.0f }
);
```

---

## 4. The Physical Pipeline: From Recording to Pixels

VSR operates across three distinct, physically decoupled phases:

```text
  Phase 1: Recording (CPU)            Phase 2: Freezing (Value)          Phase 3: Execution (GPU)
┌───────────────────────────┐       ┌───────────────────────────┐      ┌───────────────────────────┐
│ flux_encoder              │       │ flux_display_list         │      │ flux_canvas               │
│ • Linear Byte Buffer      │ ───►  │ • Immutable Byte Slice    │ ──►  │ • Tessellation Cache hit  │
│ • Zero GPU interaction    │       │ • Atomic refcount         │      │ • Push Constants BDA      │
│ • Retains image resources │       │ • Relocatable & threadsafe│      │ • Vulkan 1.3 vkCmdDraw    │
└───────────────────────────┘       └───────────────────────────┘      └───────────────────────────┘
```

### Phase 1: Recording (`flux_encoder`)
* When an application records drawing operations, `flux_encoder` does **not** call graphics drivers and does **not** perform expensive polygon triangulation.
* It writes packed binary command headers (`flux_cmd_header`), geometries, and brushes into a contiguous, cache-local byte stream.
* External GPU dependencies (such as `flux_image`) are retained via atomic reference counts so recorded bindless handles never dangle.

### Phase 2: Freezing (`flux_display_list`)
* Calling `flux_encoder_finish` seals the recording into an immutable `flux_display_list`.
* The list owns its memory budget. It has no pointer dependencies into transient recorder storage.
* It can be cached indefinitely by userland UI frameworks (e.g., `lens`), shared across thread boundaries, or submitted repeatedly.

### Phase 3: Replay & Tiered Acceleration (`flux_canvas`)
When submitted via `flux_canvas_submit_display_list(c, list)`, the engine executes the stream through a tiered acceleration pipeline:

1. **Analytic SDF Fast-Path**: For axis-aligned rounded rectangles and circles with solid colors, CPU vertex generation is bypassed entirely. The GPU evaluates signed distance fields directly in `CANVAS_PIPE_SDF`.
2. **Tessellation Cache (`tess_cache`)**: For arbitrary Bézier paths, the engine computes an FNV-1a hash over the path verb stream and stroke parameters.
   * *First frame*: The path is subdivided and ear-clipped into a triangle soup, stored in the bounded LRU cache.
   * *Subsequent frames*: The cache hits. Triangle generation time drops to **zero**, and pre-computed vertices are dispatched immediately.
3. **Stencil-then-Cover Fallback**: Self-intersecting complex paths or even-odd winding rules that stall ear-clipping fall back to Vulkan stencil-pass evaluation (`CANVAS_PIPE_STENCIL_WRITE`).

---

## 5. Architectural Separation: Display Lists vs. Submission Caches

Per [ADR-0095](../adr/0095-flux-public-contract-convergence.md), Flux strictly separates content description from hardware optimization:

| Capability | `flux_display_list` | `flux_canvas_cache_*` |
| :--- | :--- | :--- |
| **Role** | Portable, immutable content description | Low-level, disposable hardware submission cache |
| **Coordinate Space** | Logical / device-independent units | Physical framebuffer pixels |
| **State Coupling** | Decoupled; valid under any transform or scissor | Anchored to exact transform, target extent, and scissor |
| **Scope** | Cross-frame, cross-canvas, cross-thread | Local to one `flux_canvas` instance |
| **Lifecycle** | Explicit refcount (`_retain` / `_release`) | Canvas-owned LRU byte budget with auto-eviction |

---

## 6. Vulkan 1.3 Hardware Alignment & Low-Latency Mechanics

VSR maps directly to modern explicit graphics APIs (Vulkan 1.3, Metal 3, DirectX 12). Traditional APIs forced continuous driver state changes; Flux aligns with hardware pipelines:

### 1. Deterministic Hot-Path (Zero Heap Allocations)
* Dynamic per-frame scratch memory uses per-frame ring buffers (`flux_frame_alloc_transient`). Allocation is a single atomic pointer bump ($< 1\text{ ns}$).
* Temporary contour and link arrays (`scratch_pts`, `scratch_verts`) are pre-allocated per canvas. Draw operations never trigger `malloc` or `free`.

### 2. Minimum-Latency Presentation (`VK_PRESENT_MODE_MAILBOX_KHR`)
* In `libs/flux/src/core/surface.c`, Flux prioritises Vulkan's Mailbox presentation mode.
* Unlike FIFO double/triple buffering, Mailbox does not queue frames; it continuously updates the backbuffer with the newest frame completed before the vertical blanking interval.
* Input-to-photon latency is reduced by $16 \sim 33\text{ ms}$ compared to traditional queues.

### 3. Register-Direct Data Flow (BDA & Push Constants)
* **Buffer Device Address (BDA)**: Vertices are addressed directly via 64-bit device addresses stored in push constants, eliminating descriptor set and vertex buffer binding overhead.
* **Device-Wide Bindless Sets**: All sampled images live in a shared descriptor set at `set = 0`, eliminating pipeline stalls from texture slot swapping.
* **Dynamic Rendering**: Rendering passes begin directly with `vkCmdBeginRendering`, bypassing legacy render pass and framebuffer object management.

---

## 7. Industry Convergence: The Universal Modern Pattern

Flux's Value-Semantic Rendering is not an isolated experiment. It is the C23 native embodiment of the architecture to which all world-class graphics engines have converged:

* **Google Chromium (`cc::PaintRecord`)**: WebKit originally painted directly; modern Blink paints procedurally into immutable display item lists (`cc::PaintRecord`), rasterized asynchronously by worker pools.
* **Google Flutter (`Impeller::Picture`)**: Flutter replaced Skia's runtime state machine with Impeller, where widgets record into immutable `Picture` command streams executed on Vulkan/Metal entity pipelines.
* **Android OS (`HWUI::DisplayList`)**: Android 5.0+ eliminated direct OpenGL calls from views; views record into `DisplayList` structures consumed by a dedicated `RenderThread`.
* **Microsoft Direct2D (`ID2D1CommandList`)**: Windows modern UI uses serialized command lists to separate layout execution from GPU presentation.

---

## Summary

Value-Semantic Rendering provides:
* **The Ergonomic Clarity of Immediate Mode**: Clean, procedural, state-free call sites.
* **The High Throughput of Retained Mode**: Static subtree display lists, LRU tessellation reuse, and zero redundant CPU math.
* **Thread-Safe Architecture**: Immutable values that can be generated on thread pools and consumed without locks.
* **Hardware Native Execution**: Direct mapping to Vulkan 1.3 dynamic rendering and command buffer recording.
