# Vendored icons

Icon assets bundled with lens and exposed through the `lens_icon*` façade
verbs (`lens_icon`, `lens_icon_button`, `lens_icon_button_active`) and
the `lens_icon_id` enum (`LENS_ICON_*`) defined in
[`<lens/icon.h>`](../../include/lens/icon.h). They render as flux path
commands via `LENS_DRAW_ICON` in the draw list.

## feather/

[Feather](https://github.com/feathericons/feather) — **MIT** (see
`feather/LICENSE`). Version 4.29.2, commit `3dc050d`. 287 icons.

Every icon is a uniform 24×24 grid: `fill="none"`, `stroke="currentColor"`,
`stroke-width="2"`, round caps/joins, built from `<path>`, `<polyline>`,
`<line>`, `<circle>`, `<rect>`, `<polygon>`. That shape maps directly onto
flux core's path API (`flux_path_*` + `flux_canvas_stroke_path`):

- `currentColor` resolves to a theme token (`color_fg` / `color_accent`).
- The 24-unit viewBox scales to any size, so icons stay crisp at any
  HiDPI scale — no `@2x` raster variants needed.

### Pipeline

1. A build step converts each Feather SVG into baked path commands
   (see `libs/lens/src/icon_data.c`).
2. The `LENS_DRAW_ICON` draw command (see `src/render/replay.c`)
   replays those commands through `flux_path` + `flux_canvas_stroke_path`
   / `flux_canvas_fill_path`, scaled to the target rect.
3. The façade verbs wrap the draw command: `lens_icon(ui, id, size)`,
   `lens_icon_button(ui, id)`, `lens_icon_button_active(ui, id, active)`.

## Licensing

Feather is MIT, which is compatible with lens's MIT license.
Keep `feather/LICENSE` alongside the assets when redistributing.
