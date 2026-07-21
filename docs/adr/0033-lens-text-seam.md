# ADR-0033: Lens text seam — draw and shape through flux-text, no in-tree font engine

- Status: Accepted
- Date: 2026-07-21
- Scope: lens (L2 toolkit). Defines the text rendering and shaping
  boundary.

## Context

Lens widgets render text (labels, buttons, textfields, tables). Doing
font loading, glyph rasterisation, shaping, BiDi, and atlas packing
inside lens would duplicate work that belongs in a sibling library —
exactly the boundary flux drew in ADR-0001/ADR-0016, where shaping lives
in the `flux-text` sibling and the canvas exposes a glyph-blit
primitive.

Forces:

1. **No font/shaping dependency in lens.** Lens must not link FreeType,
   HarfBuzz, or a font engine directly.
2. **Always-usable measurement.** Widgets that need to measure text
   before layout must get a sensible answer even when no font or shaping
   backend is available (headless tests, CI).
3. **Single engine instance.** One `flux_text` engine shared across all
   widgets, scaled with the device-pixel scale.

## Decision

1. **Lens holds one `flux_text *` on the context.** `lens_create`
   constructs it from `flux_text_desc` with the optional `device` and
   the current `scale`. If construction fails (allocation only), `ui->text`
   is `NULL` and text simply stays blank. Implemented in
   `libs/lens/src/core/context.c`.
2. **The engine degrades to monospace metrics internally.** When no
   shaping backend or font is available, `flux-text` falls back to
   monospace metrics, so widgets never branch on backend presence.
3. **A thin seam in `seam.c`.** `lensi_text_measure_label`,
   `lensi_text_caret_x`, `lensi_text_caret_byte`, `lensi_text_sel_rects`,
   `lensi_text_caret_visual` route to `flux_text_*` and apply lens's
   label conventions (measure only the visible prefix before `"##"`).
   No backend branching lives in lens. Implemented in
   `libs/lens/src/text/seam.c`.
4. **Public measurement entry points.** `lens_text_measure` and
   `lens_text_measure_ex` (the latter takes a weight) are the only text
   API surface; they return `lens_text_metrics { width, height,
   baseline }`. Layout pass 1 ([ADR-0028](0028-lens-flexbox-layout.md))
   is the only caller that uses them during measure.
5. **Drawing rides the canvas seam.** Widget draw commands push
   `LENS_DRAW_TEXT`; replay emits the canvas text call through the
   flux-text/`flux_canvas` integration, never an in-tree rasteriser.
6. **Scale is forwarded.** `lens_set_scale` calls `flux_text_set_scale`
   so shaping/metrics track HiDPI.

References: `libs/lens/include/lens/lens.h` (Text seam section),
`libs/lens/src/text/seam.c`, `libs/lens/src/core/context.c`,
`libs/lens/src/internal.h` (text seam helpers).

## Alternatives Considered

- **In-tree FreeType + HarfBuzz + atlas.** Reject: contradicts the
  sibling-library boundary; every lens consumer already links these.
- **Per-widget text engines.** Reject: cache miss and memory cost; one
  shared engine is correct and cheaper.
- **Stub measurement that returns a fixed advance.** Reject: layout
  would mis-size everything; the monospace fallback is a better default
  and ships in `flux-text` already.

## Consequences

Positive:

- Lens has no font/shaping code to maintain; shaping improvements land
  in `flux-text` and reach lens for free.
- Headless tests get real (monospace) metrics without a font.
- The seam is small and grep-able: all text calls go through `seam.c`.

Negative:

- Lens is coupled to the `flux-text` API surface; changes there ripple
  into `seam.c`.
- The monospace fallback is approximate; a UI that needs accurate
  metrics before a font loads will measure slightly off.

## References

- [ADR-0024](0024-lens-foundations.md) — foundations.
- [ADR-0025](0025-lens-draws-through-flux-canvas.md) — drawing seam.
- [ADR-0034](0034-lens-text-measurement.md) — measurement details.
