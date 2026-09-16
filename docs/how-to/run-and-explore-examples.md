# How to run and explore interactive examples

Optics ships a rich suite of interactive and headless examples demonstrating 2D canvas drawing, 3D scene rendering, GPU particle dynamics, physical glass and acrylic materials, and desktop UI workflows.

Use this guide to build, execute, and interact with the available examples across the monorepo.

---

## 1. Prerequisites and Build Configuration

All examples are configured through the root Meson build. Ensure you have installed the core dependencies (Vulkan SDK / loader, GLFW, FreeType, HarfBuzz) as described in [Setup](../dev/setup.md).

Enable examples during setup or reconfigure your existing build tree:

```bash
# Initial setup with examples enabled
meson setup build -Dexamples=true

# Or reconfigure an existing build tree
meson setup --reconfigure build -Dexamples=true

# Compile the entire suite
meson compile -C build
```

Binary executables are placed in `./build/examples/<module>/<binary>`.

---

## 2. Visual Effects & Material Showcase (`examples/showcase/`)

The showcase suite focuses on visual quality, GPU compute pipelines, and complex effect compositions built on `flux` and `prism`.

### `material_gallery` — Modern UI Material Library Showcase

The flagship visual comparison demo for `libs/prism` (ADR-0096 / ADR-0097). It consolidates modern desktop materials into an interactive gallery over a dynamic animated backdrop (moving radial gradients, high-contrast stripes, and geometric rings), allowing side-by-side or fullscreen inspection:

1. **Liquid Glass** (`<prism/liquid_glass.h>`): Physical index-of-refraction (IOR) bending, chromatic dispersion (RGB split), Fresnel specular rim highlights, and SDF focus field.
2. **Frosted Glass** (`<prism/frosted.h>`): High-performance dual-Kawase blur with vibrancy saturation boost (macOS-style vibrancy).
3. **Acrylic** (`<prism/acrylic.h>`): Windows Fluent-style dual-Kawase blur, luminance plate balancing (Smoke/Pearl), 1px SDF border highlight, and procedural blue-noise grain dither.
4. **Mica & Mica Alt** (`<prism/mica.h>`): Windows 11 desktop foundation material with screen-anchored backdrop sampling, soft blur, theme plate tinting, mineral dither, and inactive fallback state.

**Run command:**
```bash
./build/examples/showcase/material_gallery
```

**Interactive Controls:**
| Key | Action |
| :--- | :--- |
| `1` | **4-Up Comparison Grid** — View all 4 materials side-by-side |
| `2` | **Liquid Glass Fullscreen** — Focus on SDF refraction and chromatic dispersion |
| `3` | **Frosted Glass Fullscreen** — Focus on vibrant dual-Kawase blur |
| `4` | **Acrylic Fullscreen** — Focus on noise-dithered blur and luminance plates |
| `5` | **Mica & Mica Alt Fullscreen** — Focus on wallpaper composite and mineral dither |
| `D` | Toggle **Dark / Light** theme plate polarity |
| `F` | Toggle **Inactive State Fallback** on Mica |
| `Space` | Pause / resume background animation |

---

## 3. 2D Canvas & 3D Core Graphics (`examples/flux/`, `flux-scene-graph/`, `flux-text/`)

These examples demonstrate the fundamental rendering primitives in `libs/flux`.

| Binary | Description | Key Features Demonstrated |
| :--- | :--- | :--- |
| `build/examples/flux/canvas_hello` | 2D immediate & retained canvas | Rectangles, rounded rects, circles, paths, linear gradient brushes, stroking. |
| `build/examples/flux/scene_cube` | 3D mesh rendering | Depth buffer management, matrix transforms, Phong lighting with transient buffer device addresses. |
| `build/examples/flux/compute_fill` | Compute-to-canvas integration | Offscreen compute dispatch with synchronization barriers. |
| `build/examples/flux/image_animation` | Animated texture streaming | Host-clock synchronization, strided image updates. |
| `build/examples/flux-scene-graph/gltf_viewer` | glTF 2.0 3D asset viewer | Scene hierarchy traversal, PBR material evaluation, bundled `Duck.glb` rendering. |
| `build/examples/flux-text/text_hello` | Typography & text shaping | HarfBuzz shaping, dual-layer R8 glyph atlas, subpixel glyph blitting. |

**Quick run:**
```bash
./build/examples/flux/canvas_hello
./build/examples/flux/scene_cube
./build/examples/flux-scene-graph/gltf_viewer
./build/examples/flux-text/text_hello
```

---

## 4. UI Components, Window Shell & Interactions (`examples/iris/`, `examples/lens/`, `examples/anim/`)

These examples demonstrate user interface state management, window shell integration, and physics-based motion.

### `examples/iris/widgets` & `desktop_demo` — Retained Desktop UI

Demonstrates standard controls and multi-view window layouts using `liblens` widgets hosted in an `iris` application shell:
- Push buttons, toggle switches, and checkboxes
- Text fields with native caret selection and IME hooks
- Sliders, progress bars, and segmented tabs
- Split view containers and scrollable viewports

```bash
# Launch full desktop UI control gallery
./build/examples/iris/widgets

# Launch multi-view desktop workspace shell
./build/examples/iris/desktop_demo
```

### `examples/iris/dnd_demo` — Native Drag and Drop

Demonstrates the cross-platform drag-and-drop subsystem (ADR-0086) with Wayland data device protocol integration and visual drop indicators:

```bash
./build/examples/iris/dnd_demo
```

### `examples/iris/overlay_demo` — Modals, Popups & Tooltips

Demonstrates the spatial overlay architecture (`Place` layer): non-clipping tooltips, dropdown menus, and modal dialogs escaping parent scroll viewports without hierarchy restructuring.

```bash
./build/examples/iris/overlay_demo
```

### `examples/anim/motion_spring` — Spring Physics Dynamics

Demonstrates critically damped, under-damped, and over-damped analytical spring curves (ADR-0077) driving interface transformations without fixed duration timers.

```bash
./build/examples/anim/motion_spring
```

### Headless & Accessibility Verification (`examples/lens/`)

These tools run in CI or headless environments without an active GPU or display server:

- **`build/examples/lens/headless_demo`**: Retained scene graph construction, flexbox layout calculation, and snapshot tree walk without GPU initialization.
- **`build/examples/lens/a11y_tree_demo`**: Constructs and dumps the semantic accessibility tree with role bindings, states, and text ranges for screen readers.

```bash
./build/examples/lens/headless_demo
./build/examples/lens/a11y_tree_demo
```

---

## 5. Command Reference Cheat Sheet

| Category | Command | Window / Display Required |
| :--- | :--- | :--- |
| **Material Showcase** | `./build/examples/showcase/material_gallery` | Yes (Vulkan + GLFW) |
| **2D Canvas** | `./build/examples/flux/canvas_hello` | Yes (Vulkan + GLFW) |
| **3D Cube** | `./build/examples/flux/scene_cube` | Yes (Vulkan + GLFW) |
| **Compute Fill** | `./build/examples/flux/compute_fill` | Yes (Vulkan + GLFW) |
| **Image Animation** | `./build/examples/flux/image_animation` | Yes (Vulkan + GLFW) |
| **glTF 3D Viewer** | `./build/examples/flux-scene-graph/gltf_viewer` | Yes (Vulkan + GLFW) |
| **Typography** | `./build/examples/flux-text/text_hello` | Yes (Vulkan + GLFW) |
| **UI Widgets** | `./build/examples/iris/widgets` | Yes (Wayland / Win32 / Cocoa) |
| **Desktop Shell** | `./build/examples/iris/desktop_demo` | Yes (Wayland / Win32 / Cocoa) |
| **Drag & Drop** | `./build/examples/iris/dnd_demo` | Yes (Wayland / Win32 / Cocoa) |
| **Popups & Menus** | `./build/examples/iris/overlay_demo` | Yes (Wayland / Win32 / Cocoa) |
| **Damage Tracking** | `./build/examples/iris/paint_static_demo` | Yes (Wayland / Win32 / Cocoa) |
| **Font Discovery** | `./build/examples/iris/fonts` | Yes (Wayland / Win32 / Cocoa) |
| **Spring Motion** | `./build/examples/anim/motion_spring` | Yes (Wayland / Win32 / Cocoa) |
| **Headless UI Walk**| `./build/examples/lens/headless_demo` | **No** (Headless CPU) |
| **A11y Tree Dump** | `./build/examples/lens/a11y_tree_demo` | **No** (Headless CPU) |
| **Reactive State** | `./build/examples/lens/state_demo` | **No** (Headless CPU) |

---

## Related Documentation

- [Getting Started Tutorial](../tutorials/01-getting-started.md) — First-time build and basic app bootstrap.
- [Surface Materials Governance](../governance/materials.md) — Architectural invariants for Prism materials.
- [UI Components Governance](../governance/components.md) — Invariants for Lens interaction atoms and layout.
- [Developer Setup](../dev/setup.md) — Complete toolchain prerequisites and build flags.
