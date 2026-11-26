# Optics

A pure C23 2D graphics and UI stack on Vulkan 1.3.

Optics is a restrained, orthogonal set of six libraries for desktop applications and creative tools: a GPU and CPU 2D vector canvas, text shaping, an immediate-mode UI engine, closed-form analytic motion, native windowing, and an application shell. External asset/document format parsers (PNG/JPEG/SVG/glTF) and branded OS shader packs are deliberately out of scope (ADR-0174). It is pre-1.0: there is no API or ABI stability promise yet.

---

## Highlights

- **C23 throughout**: `-std=c23` with `nullptr`, `{}` zero-initialization, fixed-underlying-type enums, `[[nodiscard]]`, and `#embed` for shaders.
- **Vulkan 1.3, no RHI abstraction**: dynamic rendering, timeline semaphores, buffer device addresses, and bindless descriptors, used directly.
- **CPU canvas backend**: a software rasterizer (`<optics/gfx/canvas_cpu.h>`) behind the same canvas API, for headless rendering, pixel tests, and CI. It is also the reference the GPU path is measured against.
- **Immediate-mode UI over a retained core**: Flex and Grid layout, overlay z-bands, damage tracking, per-context format-agnostic vector icons, zero steady-state frame allocations, and an accessibility tree (`ui`).
- **Closed-form motion**: analytic spring integration and parametric easing (`motion`, header-only).
- **Native windowing**: Wayland, Win32, and Cocoa (via MoltenVK) backends (`window`); see [Platforms](#platforms) for how each is verified.
- **Use what you need**: `window` + `gfx` is a window with a canvas and no UI toolkit; `ui` runs headless with no window system; `app` joins them.
- **Rust ecosystem**: `optics` facade crate, `optics-*-sys` bindgen crates, and safe `optics-*` wrappers under `crates/`, symmetric with `libs/` under a single repository-root Cargo workspace.

---

## Architecture

One foundation library, four orthogonal libraries that build on it, and one shell that joins the UI engine to the window. All libraries use canonical semantic names under the unified `optics` namespace (ADR-0170, ADR-0174). Dependencies point one way:

| Library | Depends on | Role |
| :--- | :--- | :--- |
| **`gfx`** (`liboptics_gfx`) | Vulkan | Rendering foundation: Vulkan 1.3 RHI, 2D canvas (GPU and CPU backends), compute, image effects, analytic curve kernel, SDF, color management. One library, one ABI, no optional modules. |
| **`text`** (`liboptics_text`) | gfx | Text: HarfBuzz shaping, layout metrics, glyph atlas. |
| **`motion`** (`<optics/motion.h>`) | libm | Motion vocabulary: springs, easing, smoothing. Header-only. |
| **`window`** (`liboptics_window`) | gfx | Platform shell: native window, event pump, frame cadence, clipboard, IME, dialogs, accessibility bridge. |
| **`ui`** (`liboptics_ui`) | gfx, text | UI engine: immediate-mode API, layout, widgets, per-context vector icons, snapshots, semantic tree. Headless. |
| **`app`** (`liboptics_app`) | window, ui | Application shell: runs an `optics_ui` context in an `optics_window`. |

```text
app ──┬──► window ───────────────► gfx
      └──► ui ────► text ────────► gfx
motion   (header-only; libm only)
```

`window` never includes a UI header and `ui` never includes a windowing header. The UI layer plugs into the frame loop through one interface, [`<optics/window/host.h>`](libs/window/include/optics/window/host.h), which `app` implements for `ui`.

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

### Running the Flagship Showcase (`examples/showcase/`)

- **Widget Gallery** (Typography, Controls, Layout & Data, Overlays & DnD, Motion):
  ```bash
  ./build/examples/showcase/widget_gallery
  ```

### Running Minimal Starters (`examples/getting_started/`)

- **Minimal GUI App** (`app` + `ui`): `./build/examples/getting_started/hello_app`
- **Canvas Window without UI** (`window` + `gfx`): `./build/examples/getting_started/canvas_window`
- **Headless CPU Canvas** (`gfx`): `./build/examples/getting_started/headless_canvas out.ppm`
- **Headless Compute Shader** (`gfx`): `./build/examples/getting_started/compute_pass`

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
