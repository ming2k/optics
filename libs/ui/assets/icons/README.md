# Vendored icons

Icon assets bundled with ui and drawn through `optics_ui_icon` (and the
`icon` field of `optics_ui_button_opts` and the other widget options), keyed by
the `optics_ui_icon_id` enum (`OPTICS_UI_ICON_*`) defined in
[`<optics/ui/icon.h>`](../../include/optics/ui/icon.h). They render as gfx path
commands via `OPTICS_UI_DRAW_ICON` in the draw list.

## feather/

[Feather](https://github.com/feathericons/feather) — **MIT** (see
`feather/LICENSE`). Version 4.29.2, commit `3dc050d`. 287 icons.

Every icon is a uniform 24×24 grid: `fill="none"`, `stroke="currentColor"`,
`stroke-width="2"`, round caps/joins, built from `<path>`, `<polyline>`,
`<line>`, `<circle>`, `<rect>`, `<polygon>`. That shape maps directly onto
gfx core's path API (`optics_gfx_path_*` + `optics_gfx_canvas_stroke_path`):

- `currentColor` resolves to a theme token (`color_fg` / `color_accent`).
- The 24-unit viewBox scales to any size, so icons stay crisp at any
  HiDPI scale — no `@2x` raster variants needed.

### Pipeline

1. A build step converts each Feather SVG into baked path commands
   (see `libs/ui/src/icon_data.c`).
2. The `OPTICS_UI_DRAW_ICON` draw command (see `src/render/replay.c`)
   replays those commands through `optics_gfx_path` + `optics_gfx_canvas_stroke_path`
   / `optics_gfx_canvas_fill_path`, scaled to the target rect.
3. The widgets wrap the draw command: `optics_ui_icon` with
   `optics_ui_icon_opts`, or `optics_ui_button` with `.icon` (and `.active` for a
   toggled state).

## material-rounded/

[Material Icons](https://github.com/google/material-design-icons) by Google —
**Apache-2.0** (see `material-rounded/LICENSE`). This directory contains the
rounded empty/filled star pair used by checkable icon buttons. The SVG wrappers
are normalized for the ui generator; the upstream path data is unchanged.

Unlike Feather's stroked paths, these assets are replayed with
`optics_gfx_canvas_fill_path`. The generator appends non-Feather sources after the
stable Feather ID range and emits a render-mode table alongside the path data.

## Licensing

Feather is MIT and Material Icons is Apache-2.0; both are compatible with
ui's MIT license. Keep each source directory's `LICENSE` file alongside the
assets when redistributing.
