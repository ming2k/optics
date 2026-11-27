# How to run and explore interactive examples

Optics organizes its in-tree examples into two curated tiers ([ADR-0171](../adr/0171-curated-examples-taxonomy-and-showcase-design-system.md)):

- **`examples/getting_started/`** — minimal, single-concept starters for learning each architectural path.
- **`examples/showcase/`** — the flagship desktop UI application sharing a unified design system (`examples/showcase/common/design.h`).

---

## 1. Build Configuration

Enable examples via Meson and compile:

```bash
meson setup build -Dexamples=true
meson compile -C build
```

Binaries are placed under `./build/examples/getting_started/` and `./build/examples/showcase/`.

---

## 2. Flagship Showcase (`examples/showcase/`)

### `widget_gallery` — Multi-Page Desktop UI Showcase

Demonstrates `optics_ui`, `optics_app`, `optics_window`, and `optics_text` in a cohesive desktop shell with a resizable sidebar (`optics_ui_split_handle_v`), dark/light theme polarity toggle, text-scale selector, and reduced-motion toggle:

1. **Typography** — optical type scale, Sans/Serif/Mono switching, CJK + Latin + BiDi shaping.
2. **Controls** — button variants, checkboxes, segmented controls, sliders, IME-aware text editing.
3. **Layout & Data** — closable TabStrip, responsive 3-column Grid cards, TreeView, sortable/resizable DataGrid.
4. **Overlays & DnD** — anchored popup menu, centered modal dialog, native drag-and-drop source and drop zone.
5. **Motion** — interactive closed-form spring physics playground (`<optics/ui/motion.h>`).

```bash
./build/examples/showcase/widget_gallery
```

---

## 3. Minimal Starters (`examples/getting_started/`)

| Binary | Libraries | What it teaches |
| :--- | :--- | :--- |
| `build/examples/getting_started/hello_app` | `app`, `ui`, `window` | Minimal windowed GUI app with immediate-mode state and theme switching. |
| `build/examples/getting_started/canvas_window` | `window`, `gfx` | Native window drawing 2D vector geometry without linking `ui`. |
| `build/examples/getting_started/headless_canvas` | `gfx` | GPU-free CPU canvas (`<optics/gfx/canvas_cpu.h>`) rendering to PPM. |
| `build/examples/getting_started/compute_pass` | `gfx` | Headless Vulkan compute shader dispatch (`Slang -> SPIR-V`) and readback. |
