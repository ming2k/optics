---
id: ADR-0123
title: "Multi-layer compositing, hardware subsurfaces, and incremental damage presentation"
status: accepted
date: 2026-10-28
superseded_by: null
scope: libs/flux, libs/lens, libs/iris, libs/prism, architecture
negative_knowledge: true
---

# ADR-0123: Multi-layer compositing, hardware subsurfaces, and incremental damage presentation

- Status: Accepted
- Date: 2026-10-28
- Scope: `libs/flux/include/flux/`, `libs/flux/src/compositor/`, `libs/lens/include/lens/`, `libs/lens/src/render/`, `libs/iris/`, `libs/prism/`.
- Implementation: **Landed on Wayland, Win32, and Cocoa.** Landed: `lens_damage_rect(ui)` computing tight visual damage bounding boxes across dirty nodes, tooltips, and carets, clipped to the layout viewport (`tests/lens/test_repaint.c`); incremental damage dynamic rendering pass clipping (`render_offset_x/y`, `render_width/height`) and `wl_surface_damage_buffer` across all three backends; hardware dual-plane subsurface composition (`wl_subcompositor` + `wl_subsurface` with premultiplied alpha) on Wayland, physically isolating Host Canvas (`cfg->paint`) from Lens UI chrome (`[INV-COMP-01]`, `[INV-COMP-03]`); and persistent Material Backdrop Boundary caching in `prism_backdrop_layer_filter` skipping compute passes on static scenes (`[INV-COMP-04]`, verified by `tests/prism/integration/test_backdrop_layer.c`).
- Depends on: [ADR-0025](0025-lens-draws-through-flux-canvas.md), [ADR-0089](0089-rendering-program-and-execution-plan.md), [ADR-0090](0090-display-list-resource-ownership.md), [ADR-0091](0091-drawing-group-and-effect-semantics.md), [ADR-0094](0094-lens-scene-snapshots-and-invalidation.md), [ADR-0122](0122-compositor-driven-presentation-pacing.md).
- Supersedes: Decision #3 of [ADR-0030](0030-lens-damage-tracking.md) (the legacy full-tree repaint policy).

## Context

Prior to this decision, Optics rendered all visual elements—including host custom drawing (`cfg->paint`, e.g., complex audio visualizers, 3D Vista scenes), optical materials (`prism`), and immediate-mode UI chrome (`lens`)—into a single flat swapchain image back-buffer. Every rendered frame began with an unconditional whole-window clear (`flux_canvas_begin` with `clear_color`), redrew all host canvas geometry, and then rasterized the entire Lens UI snapshot over the top before invoking `flux_frame_present`.

On modern high-resolution displays (HiDPI / Retina, Scale $\ge$ 2), this flat, monolithic pipeline caused severe memory bandwidth saturation and thermal throttling:

1. **Host-UI Invalidation Entanglement:** In creative and media applications (such as Wavora), the host stage operates at a high dynamic cadence (e.g. 60–120 Hz particle fields or waveforms), whereas the UI controls (sliders, toolbars, buttons) remain largely static. Conversely, during playback pause, a single blinking cursor (1 Hz) or progress bar tick forced the entire 3D/vector stage to clear and redraw from scratch.
2. **Bandwidth Explosion on HiDPI Displays:** On a 2342×1454 window at Scale=2 (equivalent to ~3.4 million pixels, ~13.6 MB per RGBA8 buffer), performing whole-frame clear-and-draw 60–120 times per second consumes gigabytes of memory bandwidth per second. On integrated GPUs (Intel UHD / Iris Xe, AMD Radeon 700M series) sharing unified system RAM with the CPU, this continuous memory traffic starved the CPU cache, increased package temperatures, and caused unpredictable frame drops.
3. **Absence of Hardware Composition:** Modern display servers (Wayland Compositors like Mutter/KWin, macOS WindowServer, Windows DWM) provide hardware overlay planes via subsurfaces. Optics completely bypassed these facilities, forcing the GPU shader units to manually blend opaque UI chrome over canvas content.
4. **Material Backdrop Starvation:** `prism` materials (such as Frosted Glass, Acrylic, and Liquid Glass) require sampling the rendered backdrop underneath the material surface. In a flat swapchain, this required expensive mid-pass GPU pipeline barriers, multi-pass framebuffer resolve, or redundant redraw passes.

## Decision

Optics formally introduces the **Layered Compositing Subsystem (`flux_compositor`)**, integrating hardware subsurfaces, offscreen layer caching, and incremental damage presentation:

```text
Window Root Scene
  ├── Subsurface Layer: Host Canvas (cfg->paint, Vista 3D, Wavora Visual Stage)
  ├── Backdrop Filter Layer: Prism Materials (Frosted Glass, Acrylic, Liquid Glass)
  └── Cached Picture Layer: Lens UI Chrome (DisplayList Cache + Damage Bounds)
           │
           ▼
     flux_compositor
  ┌────────┴──────────────────────────┐
  ▼                                   ▼
Hardware Subsurface Plane       Offscreen GPU Layer Cache (FBO)
(wl_subsurface / CALayer)       + Incremental Present (VkPresentRegionsKHR)
```

### 1. The Compositor Layer Tree (`flux_layer_tree`)

Drawing targets are no longer unified into a single monolithic canvas pass. Applications compose their visual hierarchy into an explicit, lightweight layer tree:

```c
typedef enum flux_layer_kind {
    FLUX_LAYER_HOST_SURFACE = 0, /* Independent background canvas / 3D target */
    FLUX_LAYER_PICTURE      = 1, /* Rasterized 2D display list with dirty rects */
    FLUX_LAYER_BACKDROP     = 2, /* Optical material filter sampling underneath */
} flux_layer_kind;

typedef struct flux_layer_desc {
    flux_layer_kind kind;
    flux_rect bounds;
    float opacity;
    bool opaque;
    bool hardware_plane_preferred; /* Hint to use wl_subsurface / CALayer */
} flux_layer_desc;

typedef struct flux_layer flux_layer;
typedef struct flux_compositor flux_compositor;
```

### 2. Macro-Level Hardware Subsurface Composition (`iris_subsurface`)

- **Wayland (`app_wayland.c`):**
  - Allocate a secondary `wl_surface` via `wl_subcompositor_get_subsurface()`.
  - The host canvas (`cfg->paint`) renders to the base subsurface; the Lens UI chrome renders to the top subsurface with an alpha channel.
  - The Wayland compositor (KWin, Mutter, Sway) blends the two surfaces using hardware display controller planes (KMS planes), completely eliminating GPU blending passes and shader readback bandwidth.
- **macOS (`app_cocoa.m`):**
  - Structure the view hierarchy using dedicated `CAMetalLayer` instances nested in the host `CALayer`. CoreAnimation performs compositing on dedicated hardware planes.
- **Windows (`app_win32.c`):**
  - Integrate `IDCompositionVisual` / DirectComposition to present independent visual surfaces.

### 3. Micro-Level GPU Layer Caching & Incremental Damage Tracking

When hardware subsurface delegation is unavailable or for fine-grained subtrees inside Lens:

- **Offscreen Texture Cache (FBO Cache):** Static UI subtrees are rasterized once into a transient GPU texture. In subsequent frames where `cmd_hash` and layout geometry remain identical, `flux_compositor` simply blits the cached texture with zero vertex generation or font atlas re-evaluation.
- **Damage Region Aggregation:** `lens` tracks dirty rectangles (`lensi_damage_rect`) across consecutive frames. Only the bounding union of damaged areas is submitted to the rasterizer.
- **Incremental Swapchain Presentation:**
  - On Vulkan: Integrate `VK_KHR_incremental_present` (`VkPresentRegionsKHR`), advising the presentation engine to copy only the modified tiles or scanlines.
  - On Wayland: Call `wl_surface_damage_buffer()` specifying the precise dirty bounding box, avoiding full-screen compositor repaints.
  - On macOS: Utilize `-[CAMetalLayer presentAfterMinimumDuration:bounds:]` with explicit damage rects.

### 4. Explicit Backdrop Filter Architecture (`prism` Integration)

Materials in `prism` (Liquid Glass, Frosted Glass, Acrylic) are formalized as `FLUX_LAYER_BACKDROP` nodes within the compositor:
- The backdrop layer isolates the region of the scene behind the material into an offscreen scratch buffer.
- The dual-pass Gaussian blur and refractive IOR dispersion passes execute strictly within the scratch buffer bounds.
- If the background layer is static, the blurred backdrop texture is cached. Subsequent UI movements over the glass material do not re-trigger expensive background filtering passes.

## Invariants & Behavioral Boundaries

- **`[INV-COMP-01]` Zero Full-Screen Invalidation for Localized Damage:** A localized UI or canvas update must never trigger a whole-frame framebuffer clear or full-screen swapchain present. Swapchain presentation must issue partial damage bounds (`wl_surface.damage_buffer`, `VkPresentRegionsKHR`, or `CAMetalLayer` damage rects).
- **`[INV-COMP-02]` Layer Independence Contract:** Host custom drawing (`cfg->paint` / 3D Vista) and immediate-mode UI chrome (`lens`) must inhabit decoupled layers within `flux_compositor`. An animation or state mutation in one layer must not force re-rasterization or command recompilation of orthogonal layers.
- **`[INV-COMP-03]` Hardware Overlay Plane Delegation:** Where supported by the platform compositor (Wayland `wl_subsurface`, macOS `CALayer` hierarchy, Windows DirectComposition), layer composition must be delegated to the OS compositor / display controller plane rather than flattened via GPU readback/blit passes.
- **`[INV-COMP-04]` Material Backdrop Boundary:** Optical materials (`prism` acrylic, frosted glass, liquid glass) must sample from an explicit, isolated offscreen layer cache; un-invalidated backdrops must never be re-filtered per frame.

## Rejected Alternatives & Negative Knowledge

- **Option A — Retaining a single flat swapchain buffer with CPU-side dirty rect scissor clears.**
  *Why considered:* Avoids the memory overhead of allocating secondary subsurface buffers or offscreen textures.
  *Why rejected:* In modern double/triple-buffered swapchains, previous frames contain stale content from $N-1$ or $N-2$ cycles ago. Scissoring clears and partial redrawing directly on a multi-buffered swapchain causes severe flickering, ghosting, and tearing unless complex multi-frame history tracking is implemented, which degrades GPU tile cache utilization and breaks on Mailbox presentation.
- **Option B — Full browser-style retained DOM layer tree (100+ fine-grained layers).**
  *Why considered:* Maximizes theoretical caching granularity down to individual buttons and icons.
  *Why rejected:* Excessive layer fragmentation leads to "layer explosion" (severe VRAM allocation overhead, excessive GPU draw calls, context switching, and thrashing of display controller hardware planes). Optics enforces a coarse-grained, high-impact two-to-three layer model (Host Canvas, Backdrop Filter, UI Chrome).
- **Option C — Pure CPU-side blitting of damaged rectangles.**
  *Why considered:* Simple to implement in software.
  *Why rejected:* CPU readback from GPU memory or CPU-to-GPU PCI-e blits on HiDPI displays introduce massive bus latency, stalling both the CPU and GPU. All layer caching and compositing must remain fully resident in GPU memory via Vulkan textures.
