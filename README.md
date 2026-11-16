# Optics

A C23 graphics and UI stack on Vulkan 1.3.

Optics is a set of libraries for desktop applications and creative tools: a GPU and CPU 2D canvas, text, an immediate-mode UI engine, surface materials, 3D scene loading, and native windowing. It is pre-1.0: there is no API or ABI stability promise yet.

---

## Highlights

- **C23 throughout**: `-std=c23` with `nullptr`, `{}` zero-initialization, fixed-underlying-type enums, `[[nodiscard]]`, and `#embed` for shaders.
- **Vulkan 1.3, no RHI abstraction**: dynamic rendering, timeline semaphores, buffer device addresses, and bindless descriptors, used directly.
- **CPU canvas backend**: a software rasterizer (`<flux/canvas_cpu.h>`) behind the same canvas API, for headless rendering, pixel tests, and CI. It is also the reference the GPU path is measured against.
- **Immediate-mode UI over a retained core**: Flex and Grid layout, overlay z-bands, damage tracking, and an accessibility tree (`lens`).
- **Surface materials**: Liquid Glass (refraction and dispersion), Frosted Glass, Acrylic, and Mica (`prism`).
- **Closed-form motion**: analytic spring integration and parametric easing (`transit`, header-only).
- **Native windowing**: Wayland, Win32, and Cocoa (via MoltenVK) backends (`iris`); see [Platforms](#platforms) for how each is verified.
- **Use what you need**: `iris` + `flux` is a window with a canvas and no UI toolkit; `lens` runs headless with no window system; `aperture` joins them.
- **Rust bindings**: bindgen-generated `-sys` crates and safe wrappers in `bindings/crates/`, one workspace manifest at the repository root.

---

## Architecture

One foundation library, seven libraries that build on it, and one shell that joins the UI engine to the window. Dependencies point one way:

| Library | Depends on | Role |
| :--- | :--- | :--- |
| **`flux`** | Vulkan | Rendering foundation: RHI, 2D canvas (GPU and CPU backends), 3D draw core, compute, image effects, curve kernel. One library, one ABI, no optional modules. |
| **`glyph`** | flux | Text: HarfBuzz shaping, layout metrics, glyph atlas. |
| **`reticle`** | flux | SVG static-art compiler: parse once, paint at any size. |
| **`vista`** | flux | 3D scene graph, TRS transforms, glTF 2.0 loading. |
| **`prism`** | flux | Named surface materials. |
| **`transit`** | libm | Motion vocabulary: springs, easing, smoothing. Header-only. |
| **`iris`** | flux | Platform shell: native window, event pump, frame cadence, clipboard, IME, dialogs, accessibility bridge. |
| **`lens`** | flux, glyph, reticle | UI engine: immediate-mode API, layout, widgets, snapshots, semantic tree. Headless. |
| **`aperture`** | iris, lens | Application shell: runs a lens UI in an iris window. |

```text
aperture ──┬──► iris ───────────────► flux
           └──► lens ──┬──► glyph ───► flux
                       └──► reticle ─► flux
vista ─────────────────────────────► flux
prism ─────────────────────────────► flux
transit   (header-only; libm only)
```

`iris` never includes a UI header and `lens` never includes a windowing header. The UI layer plugs into the frame loop through one interface, [`<iris/host.h>`](libs/iris/include/iris/host.h), which `aperture` implements for lens.

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
  ./build/examples/aperture/widgets
  ```

- **A Window With flux Content and No UI Toolkit**:
  ```bash
  ./build/examples/iris/canvas_window
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

- **Headless SVG Rasterizer**:
  ```bash
  ./build/examples/reticle/rasterize_svg out.ppm
  ```

---

## Platforms

Optics targets three desktop platforms from one codebase. Linux/Wayland is built, tested, and run interactively during development. CI also builds Windows and macOS and runs the headless test layers there; the Win32 and Cocoa windowing backends have no automated windowed coverage.

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
