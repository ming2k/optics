# Showcase Examples

Flagship desktop application built on Optics. Unlike the minimal starters under
`examples/getting_started/`, this showcase combines all layers of the core stack
(`gfx` rendering + `text` + `ui` + `window` + `app`) into a complete
program with native Wayland cursor-shape and fractional HiDPI support
([ADR-0171](../../docs/adr/0171-curated-examples-taxonomy-and-showcase-design-system.md)).

```sh
meson setup build -Dexamples=true
meson compile -C build
./build/examples/showcase/widget_gallery
```

| Demo | Kind | Description |
|------|------|-------------|
| [`widget_gallery`](#widget_gallery) | interactive | Multi-page desktop UI shell: typography, controls, data views, overlays, drag-and-drop, and motion |

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
   (`<optics/ui/motion.h>`).

### Interactive Hotkeys

- `1`–`5`: Switch to the corresponding page
- `Esc`: Exit
