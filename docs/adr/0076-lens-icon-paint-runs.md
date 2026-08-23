# ADR-0076: Per-shape paint runs for runtime SVG icons

- Status: Accepted
- Date: 2026-08-23
- Deciders: optics maintainers
- Relates-to: ADR-0059 (widget skins), ADR-0066 (table icon slots)

## Context

`lens_icon_register_svg` (the runtime icon path) flattened every visible
shape into one verb stream and painted the whole glyph with a single
`flux_paint` in the theme colour — documented in the public header as
"The SVG's own paint colours are ignored". The built-in feather/material
sets are monochrome by design, so the single paint covered them; runtime
icons inherit that limitation whether or not their source is monochrome.

The first downstream consumer of runtime registration (aphrodite, a
pixel-art editor) immediately hit the wall: its tool glyphs need filled
and multicoloured shapes (paint bucket, eyedropper, palette) and had to
be authored as stroke-only monochrome look-alikes, with a comment in the
consumer source explaining that lens ignores SVG colours.

Three layers conspired to make colour inexpressible:

1. **Data**: `lens_icon_desc` carried only `{cmds, count}` — no per-shape
   attributes could ride along.
2. **Registration**: the parser read exactly one bit from each shape's
   paint (`fill.type != NSVG_PAINT_NONE`) to pick a whole-icon mode.
3. **Replay**: `LENS_DRAW_ICON` built one `flux_path` for the entire
   stream and filled/stroked it once.

The canvas layer was never the limitation — `flux_canvas_fill_path` /
`stroke_path` with per-call paints (and gradients) already exist.

## Decision

1. `lens_icon_desc` grows an optional parallel run table:
   ```c
   typedef struct lens_icon_run {
       uint32_t first_cmd, count;  /* bracket into desc->cmds */
       uint32_t color;             /* 0xRRGGBBAA straight; 0 = theme colour */
       uint8_t fill;               /* 1 = fill_path, 0 = stroke_path */
   } lens_icon_run;
   ```
   `runs == NULL` (or `run_count == 0`) means "single paint for the whole
   stream" — every built-in icon, and the exact previous behaviour.

2. Registration emits one run per source shape (adjacent shapes with
   equal `(color, fill)` merge). Explicit `fill`/`stroke` colours are
   preserved (nanosvg's 0xBBGGRR packing is converted to straight
   0xRRGGBBAA; gradients degrade to their first stop). `currentColor`
   and unpainted shapes record `color == 0` = follow the theme.

3. An icon whose every run is theme-coloured collapses to
   `runs == NULL`: registration discards the table, and the icon hashes,
   replays and compares exactly like a built-in. This is the common case
   (all feather-style assets), so the new representation costs nothing
   for existing content.

4. Replay takes the run path when `runs != NULL`: one `flux_path` per
   run, painted with that run's colour (`flux_color_rgba_premul` on the
   straight components; `color == 0` falls back to the widget colour).
   The per-icon `mode` and the single-paint path remain for `runs == NULL`.

### currentColor handling

nanosvg has no `currentColor` representation (it parses as a named colour
and lands on fallback grey). lens rewrites the token to the sentinel hex
`#010203` in its private copy before parsing and recognises the parsed
value afterwards. A source SVG that uses that exact hex verbatim would
degrade to a near-black paint — accepted: the alternative is forking the
parser for one token. The sentinel is chosen in the deep-blue-black
corner where hand-authored icons do not live.

## Consequences

- Runtime icons can be multicoloured and solid-filled; monochrome
  assets behave exactly as before (collapsed form).
- Built-in icons are untouched: `icon_data.c` tables keep two fields,
  the new fields read as NULL/0 through the designated initialisers.
- The drawlist hash covers the same bytes as before for collapsed icons;
  run-bearing icons hash their run table with the command stream.
- Still deliberately out of scope (documented on
  `lens_icon_register_svg`): source `strokeWidth` (one weight keeps the
  set interchangeable), per-shape fill rules (nonzero only), gradients
  (first stop), and stroke-on-fill within one shape (duplicate the shape).
- Memory: a run table costs 16 bytes × shapes for coloured icons only;
  collapsed icons allocate nothing.

## Alternatives considered

- **Type-7 colour-marker verbs inside the cmd stream** — rejected: the
  cmd stream is also consumed by the tessellator/geometry layer, which
  would have to learn to skip markers; a parallel table keeps the
  geometry pipeline ignorant of paint.
- **One `flux_path` per shape plus a colour array keyed by shape index** —
  isomorphic to the run table but requires the consumer to re-derive
  shape boundaries; runs bracket cmd ranges explicitly and merge
  same-colour neighbours, so replay issues as few paints as possible.
- **Doing nothing** — leaves the only runtime icon path strictly weaker
  than what SVG assets naturally are, with the limitation documented as
  deliberate. A downstream already worked around it by degrading their
  art; that is the signal to fix the seam, not the asset.
