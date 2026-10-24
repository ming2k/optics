# Optics

A modern, high-performance C23 graphics and UI stack powered by Vulkan.

Optics delivers a unified, zero-bloat foundation for desktop applications, creative tooling, and visual computing. From raw GPU compute and 2D vector drawing to immediate-mode UI, physical materials, and window management, Optics provides modular, cohesive building blocks built on standard C23.

---

## Highlights

- **Pure C23 Baseline**: Idiomatic modern C (`-std=c23`) using native `nullptr`, `{}` zero-initialization, type-safe enums, `[[nodiscard]]`, and compound literals.
- **Vulkan 1.3 First-Class**: Modern GPU pipeline with dynamic rendering, timeline semaphores, buffer device addresses, and bindless descriptors.
- **Headless CPU Fallback**: Built-in software rasterizer (`<flux/canvas_cpu.h>`) for GPU-free headless rendering, pixel testing, and CI environments.
- **Immediate-Mode UI Engine**: Declarative ergonomics with deterministic state, Flexbox & Grid layouts, overlay z-bands, and accessibility tree export (`lens`).
- **Physical Material Optics**: Real-time optical effects including refractive dielectric Liquid Glass (IOR & chromatic dispersion), Frosted Glass, Acrylic, and Mica (`prism`).
- **Provable Motion Physics**: Closed-form non-divergent analytic spring integration, parametric easing curves, and motion-adaptive smoothing without timers (`transit`).
- **Cross-Platform Native Windowing**: Direct OS integration across Linux (Wayland), Windows (Win32), and macOS (Cocoa / MoltenVK) without intermediate baggage (`iris`).
- **Modular & Embeddable**: Every library is autonomous with clean C ABI boundaries and zero circular dependencies. Use the full stack or only what you need.
- **Rust Bindings**: Idiomatic, safe Rust wrappers available in `bindings/`.

---

## Architecture

Optics is organized into seven focused, orthogonal libraries under `libs/`:

| Library | Role | Description |
| :--- | :--- | :--- |
| **`flux`** | *Rendering Foundation* | Vulkan RHI, 2D vector canvas (GPU & CPU software backends), 3D draw core, and compute pipelines. |
| **`glyph`** | *Text & Typography* | HarfBuzz-backed text shaping, layout metrics, and dynamic texture atlas caching. |
| **`vista`** | *3D Scene Graph* | Spatial node hierarchies, TRS transforms, and glTF 2.0 model loading. |
| **`prism`** | *Material Optics* | Named modern materials: Liquid Glass (refraction & dispersion), Acrylic, Frosted Glass, and Mica. |
| **`transit`** | *Motion Vocabulary* | Analytic non-divergent springs, parametric easing curves, and motion-adaptive de-jitter smoothing. |
| **`lens`** | *Immediate-Mode UI* | Headless UI micro-kernel: 1D Flexbox & 2D Grid layouts, canonical widgets, overlay layers, and a11y tree. |
| **`iris`** | *Platform Shell* | Native window creation, display event loops, clipboard/IME, and OS input routing. |

```text
Application
  ├── iris   ── Native Windowing & Event Loop
  │     └── lens   ── UI Engine, Layout & Canonical Widgets
  │           ├── glyph   ── Text Shaping & Atlas
  │           └── flux    ── 2D Canvas (GPU / Headless CPU)
  └── Direct Rendering
        ├── vista   ── 3D Scene & glTF Models
        ├── prism   ── Physical Materials (Liquid Glass, Acrylic, Mica)
        ├── transit ── Spring Physics & Motion Smoothing
        └── flux    ── Vulkan Core, Compute & Image Effects
```

---

## Quick Start

### Building from Source

Optics uses [Meson](https://mesonbuild.com/) and Ninja for fast, cross-platform builds:

```bash
# Configure the build with examples enabled
meson setup build -Dexamples=true

# Compile
meson compile -C build
```

### Running Showcase Demos

- **Modern Material Gallery** (Liquid Glass, Frosted Glass, Acrylic, and Mica side-by-side):
  ```bash
  ./build/examples/showcase/material_gallery
  ```

- **Interactive UI Widgets**:
  ```bash
  ./build/examples/iris/widgets
  ```

- **3D glTF Model Viewer**:
  ```bash
  ./build/examples/vista/gltf_viewer
  ```

- **2D Vector Canvas**:
  ```bash
  ./build/examples/flux/canvas_hello
  ```

- **Motion Spring Physics**:
  ```bash
  ./build/examples/transit/motion_spring
  ```

---

## Platforms

Optics targets major desktop platforms from a single unified codebase:

| Platform | Windowing Backend | Graphics Backend | Font Discovery |
| :--- | :--- | :--- | :--- |
| **Linux** | Wayland (native) | Vulkan 1.3 / DRM | Fontconfig |
| **Windows** | Win32 | Vulkan 1.3 | DirectWrite |
| **macOS** | Cocoa (Objective-C) | Vulkan 1.3 via MoltenVK ≥ 1.3 | CoreText |

---

## Documentation

Comprehensive guides, references, and design specifications are located in [`docs/`](docs/index.md):

- **[Getting Started Tutorial](docs/tutorials/01-getting-started.md)**: Step-by-step introduction to building and running your first app.
- **[Interactive Examples Guide](docs/how-to/run-and-explore-examples.md)**: In-depth exploration of available showcase binaries and controls.
- **[Design Philosophy](docs/explanation/design-philosophy.md)**: Core architectural principles, orthogonality, and widget model.
- **[Application Architecture](docs/explanation/application-architecture.md)**: System layer boundaries, resource lifecycles, and event flow.
- **[API Reference](docs/reference/api.md)**: Symbol specifications, headers, and ABI contracts.

---

## License

This project is licensed under the [MIT License](LICENSE).
