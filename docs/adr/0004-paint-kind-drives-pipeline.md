# ADR-0004: Paint kind drives pipeline selection

- Status: Accepted
- Date: Stage 4.2.3

## Context

The canvas needs three rendering paths: solid colour, gradients
(linear + radial), and image sampling. Each has its own fragment
shader, so each is a distinct `VkPipeline`. The question is *how
the caller selects which one runs for a given draw*.

Two plausible models:

1. **Per-pipeline API**. `flux_canvas_fill_path_solid`,
   `flux_canvas_fill_path_gradient`, `flux_canvas_fill_path_image`,
   `flux_canvas_stroke_path_solid`, ... — N × M entry points.
2. **Per-paint API**. One entry point per geometry kind
   (`fill_path`, `stroke_path`, `fill_rect`, `draw_image`); the
   paint object's `kind` field selects the pipeline internally.

The N × M entry-point matrix grows quadratically. The per-paint
API matches how every modern 2D API (Skia, Cairo, Direct2D)
expresses this — paint is a value that describes appearance,
geometry is what's drawn.

## Decision

`flux_paint.kind` is a discriminator that selects the pipeline:

```c
typedef enum flux_paint_kind {
    FLUX_PAINT_SOLID            = 0,
    FLUX_PAINT_LINEAR_GRADIENT  = 1,
    FLUX_PAINT_RADIAL_GRADIENT  = 2,
} flux_paint_kind;
```

Geometry entry points take `const flux_paint *`. The canvas
implementation reads `paint->kind` and calls
`get_canvas_pipeline(device, color_format, kind, ...)` which
returns the appropriate pipeline from the device-level cache.

`draw_image` is a separate entry point — image kind isn't in the
paint enum. Image draws require an explicit `flux_image *`
argument and produce different vertex semantics (UV-mapped
quads), so the API surface is meaningfully different. Internally
the canvas uses a private "image" pipeline kind that lives
alongside SOLID/GRADIENT in the cache.

Push-constant layout is the same struct for every paint kind;
each shader reads only the fields it needs. The solid shader
ignores the gradient tail; the gradient shader ignores the image
tail; both ignore each other's parameters. This makes a single
`vkCmdPushConstants` per draw work for any pipeline.

## Consequences

Positive:

- One geometry × N paints is N pipelines and N entry points, not
  N × M. The API surface is small and stable as we add more paint
  kinds (textured gradient, conic gradient, masked fill).
- Switching paints between draws is a `vkCmdBindPipeline` + the
  same push update — no API churn.
- The pipeline cache lookup is keyed by `(color_format, kind)`,
  not by full pipeline shape, so cache hit rate is high even with
  diverse draw orders.
- Matches the mental model of every existing 2D API; consumers
  port idiomatically.

Negative:

- The push-constant struct carries fields irrelevant to most
  draws. A solid draw still ships 144 bytes. In practice this is
  cheap — push constants are pipelined separately and the budget
  fits.
- A future paint kind that needs a substantially different
  push-constant layout would force a struct grow (and trigger the
  ADR-0007-style budget recheck). Acceptable; rare event.

## Alternatives considered

- **Per-pipeline entry points** (`fill_path_solid`,
  `fill_path_gradient`, ...). Rejected: API surface grows
  quadratically with paints × geometry kinds.
- **Paint as opaque handle returned from a builder**. Rejected:
  paint is a small POD; the builder pattern is overkill and adds
  lifetime questions for a value type.
- **Separate push-constant struct per paint kind**. Rejected:
  forces multiple pipeline layouts (or a complex tagged union in
  shaders); the struct-tail-pattern is simpler and bounded.

## See also

- `include/flux/canvas.h` — `flux_paint_kind`, `flux_paint`.
- `src/canvas/canvas.c` — `get_canvas_pipeline`,
  `ensure_pipeline_bound`, `build_push`, `submit_triangles`.
