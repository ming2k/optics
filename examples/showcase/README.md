# Showcase Examples

Flagship desktop applications built on Optics. Unlike the minimal starters under
`examples/getting_started/`, these showcases combine multiple layers of the stack
(gfx rendering + text + svg + materials + UI + native windowing) into complete
programs with native Wayland cursor-shape and fractional HiDPI support
([ADR-0171](../../docs/adr/0171-curated-examples-taxonomy-and-showcase-design-system.md)).

```sh
meson setup build -Dexamples=true
meson compile -C build
./build/examples/showcase/widget_gallery
```

| Demo | Kind | Description |
|------|------|-------------|
| [`widget_gallery`](#widget_gallery) | interactive | Multi-page desktop UI shell: typography, controls, data views, overlays, drag-and-drop, and motion |
| [`material_gallery`](#material_gallery) | interactive, visual | All 4 materials side-by-side: Liquid Glass, Frosted, Acrylic, Mica |
| [`scene_studio`](#scene_studio) | interactive, visual | 3D glTF 2.0 viewer with a floating camera and lighting inspector |

---

## widget_gallery

The flagship `optics_ui` composition showcase. A resizable sidebar, dark/light
theme polarity, 90%/100%/115% text scales, and a reduced-motion toggle frame five
pages:

1. **Typography** — optical type scale, Sans/Serif/Mono switching, and CJK +
   Latin + BiDi shaping.
2. **Controls** — button variants, checkboxes, segmented controls, sliders, and
   IME-aware text editing.
3. **Layout & Data** — closable tab strip, responsive 3-column grid cards, tree
   view, and a sortable/resizable data grid.
4. **Overlays & DnD** — anchored popup menu, centered modal dialog, and native
   drag-and-drop source and drop zone.
5. **Motion** — interactive closed-form spring physics playground
   (`<optics/motion.h>`).

### Interactive Hotkeys

- `1`–`5`: Switch to the corresponding page
- `Esc`: Exit

---

## material_gallery

The comparative visual showcase for the entire Optics material library ([ADR-0096](../../docs/adr/0096-prism-mica-foundation-material.md), [ADR-0097](../../docs/adr/0097-canonical-example-taxonomy-and-governance.md)).
Renders an animated dynamic backdrop (radial color orbs, moving high-contrast
stripes, geometric rings) and overlays all four modern UI materials side-by-side:

1. **Liquid Glass** (`<optics/material/liquid_glass.h>`): physical IOR refraction,
   chromatic dispersion (RGB split), continuous G2 squircle curvature, and 360° dual-rim optics.
2. **Frosted Glass** (`<optics/material/frosted.h>`): high-purity dual-Kawase blur with
   macOS-style vibrancy color saturation boost.
3. **Acrylic** (`<optics/material/acrylic.h>`): Windows Fluent-style dual-Kawase blur,
   luminance plate balancing (Smoke / Pearl), 1px SDF border highlight rim, and
   procedural blue-noise grain.
4. **Mica & Mica Alt** (`<optics/material/mica.h>`): Windows 11 desktop foundation material
   with screen-anchored wallpaper sampling, soft blur, theme tinting, mineral dither,
   and inactive fallback state.

### Interactive Hotkeys

- `1`: Raw Backdrop inspection (Control variable baseline with zero material overlay)
- `2`: Liquid Glass comprehensive suite (Hero G2 Squircle physical glass, Fluid Metaball fusion, Amber tint, micro-dock pill)
- `3`: Frosted Glass fullscreen focus
- `4`: Acrylic fullscreen focus
- `5`: Mica & Mica Alt fullscreen focus
- `B`: Cycle backdrop style (Sunset Aurora, Spatial Grid, Pure Black, Pure White)
- `D`: Toggle Dark / Light mode theme plate polarity (Smoke vs. Pearl)
- `F`: Toggle Inactive state fallback on Mica
- `Space`: Pause / Resume backdrop animation (also available via UI Play/Pause button)
- `Esc` or `Q`: Exit

---

## scene_studio

Combines `liboptics_scene` and the `liboptics_gfx` depth-buffered Phong pass with a
floating `optics_ui` toolbar and live camera/lighting inspector. By default it
loads the bundled `assets/Duck.glb`; pass a custom glTF binary with `--file`:

```sh
./build/examples/showcase/scene_studio --file path/to/model.glb
```

The inspector exposes orbit speed, pitch, field of view, and ambient light
sliders. If the bundled asset cannot be read, the viewer falls back to a
procedurally generated cube.

### Interactive Hotkeys

- `Space`: Pause / resume auto-orbit
- `I`: Show / hide the camera and lighting inspector
- `Esc` or `Q`: Exit
