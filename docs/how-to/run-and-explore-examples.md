# How to run and explore interactive examples

Optics organizes its in-tree examples into two curated tiers ([ADR-0171](../adr/0171-curated-examples-taxonomy-and-showcase-design-system.md)):

- **`examples/getting_started/`** — minimal, single-concept starters for learning each architectural path.
- **`examples/showcase/`** — three flagship desktop applications sharing a unified design system (`examples/showcase/common/design.h`).

---

## 1. Build Configuration

Enable examples via Meson and compile:

```bash
meson setup build -Dexamples=true
meson compile -C build
```

Binaries are placed under `./build/examples/getting_started/` and `./build/examples/showcase/`.

---

## 2. Flagship Showcases (`examples/showcase/`)

### `widget_gallery` — Multi-Page Desktop UI Showcase

Demonstrates `optics_ui`, `optics_app`, `optics_window`, `optics_text`, and `optics_motion` in a cohesive desktop shell with a resizable sidebar (`optics_ui_split_handle_v`), dark/light theme polarity toggle, text-scale selector, and reduced-motion toggle:

1. **Typography** — optical type scale, Sans/Serif/Mono switching, CJK + Latin + BiDi shaping.
2. **Controls** — button variants, checkboxes, segmented controls, sliders, IME-aware text editing.
3. **Layout & Data** — closable TabStrip, responsive 3-column Grid cards, TreeView, sortable/resizable DataGrid.
4. **Overlays & DnD** — anchored popup menu, centered modal dialog, native drag-and-drop source and drop zone.
5. **Motion** — interactive closed-form spring physics playground (`<optics/motion.h>`).

```bash
./build/examples/showcase/widget_gallery
```

### `material_gallery` — Optical Surface Materials Showcase

Interactive side-by-side and fullscreen comparison of `liboptics_material` over a dynamic animated backdrop:

1. **Liquid Glass** — physical IOR refraction, chromatic dispersion, Fresnel rim highlights.
2. **Frosted Glass** — dual-Kawase blur with vibrancy boost.
3. **Acrylic** — dual-Kawase blur, luminance plate balancing, and procedural blue-noise grain.
4. **Mica & Mica Alt** — wallpaper-anchored backdrop sampling and mineral dither.

```bash
./build/examples/showcase/material_gallery
```

### `scene_studio` — 3D glTF 2.0 Studio Viewer

Combines `liboptics_scene` and `liboptics_gfx` depth-buffered 3D rendering with a floating `optics_ui` toolbar and live camera/lighting inspector (`--file path/to/model.glb` loads custom glTF binary assets; defaults to bundled `Duck.glb`).

```bash
./build/examples/showcase/scene_studio
```

---

## 3. Minimal Starters (`examples/getting_started/`)

| Binary | Libraries | What it teaches |
| :--- | :--- | :--- |
| `build/examples/getting_started/hello_app` | `app`, `ui`, `window` | Minimal windowed GUI app with immediate-mode state and theme switching. |
| `build/examples/getting_started/canvas_window` | `window`, `gfx` | Native window drawing 2D vector geometry without linking `ui`. |
| `build/examples/getting_started/headless_canvas` | `gfx` | GPU-free CPU canvas (`<optics/gfx/canvas_cpu.h>`) rendering to PPM. |
| `build/examples/getting_started/headless_svg` | `svg` | GPU-free SVG parsing and rasterization (`<optics/svg.h>`) to PPM. |
| `build/examples/getting_started/compute_pass` | `gfx` | Headless Vulkan compute shader dispatch (`Slang -> SPIR-V`) and readback. |
