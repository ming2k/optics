# ADR-0079: Layered backdrop material — frost beneath glass in one dispatch

- Status: Accepted
- Date: 2026-08-25
- Scope: `libs/prism` (new material `prism_backdrop_layer_filter`, shader `backdrop_frost.comp`, shared glass dispatcher `glass_dispatch.h`); amends [ADR-0063](0063-liquid-glass-material-library.md)

## Context

A compositor chrome surface is frequently two materials stacked: a
frosted sheet (the blurred backdrop) carrying analytic liquid-glass
bodies. Aegis' command panel is the motivating consumer — one fullscreen
frost under six floating glass bodies — but the pattern is generic.

The existing materials could not express the relation. Both the frost and
the glass sampled the same desktop capture: the caller blurred it, ran the
liquid-glass filter over the capture, and drew the two results in
sequence. Draw order is not a sampling relation — the glass lens still
sampled the sharp capture, so through every glass body the frost was
optically bypassed. The stack read as two independent views of the
background that happened to be painted one after the other.

Chaining is deliberately caller-side ([ADR-0008], [ADR-0074]), and the
capture seam ([ADR-0017]) deliberately offers no `saveLayer`/effect-graph
object. ADR-0050 rejected a *second analytic glass group* inside one body
(nested rims/shadows); it did not address a glass material nesting over a
frost sheet, which adds no nested SDF.

## Decision

Add a named material for the layered relation: `prism_backdrop_layer`
(prism's "backdrop layer"). One filter, one persistent transparent output
per frame slot, one ordered dispatch sequence:

1. every `prism_backdrop_frost` rectangle writes the blurred backdrop
   source-over inside a rounded-rect SDF (`backdrop_frost.comp`);
2. every `prism_liquid_glass_group` runs the unchanged liquid-glass
   reference recipe — but the lens samples the **frosted layer image**,
   so glass refracts the frost.

The layer order (all frost beneath all glass) is part of the material's
identity, not caller policy. The glass dispatch recorder is extracted
into `glass_dispatch.h` and shared verbatim with the standalone filter, so
a layered body cannot drift from a standalone one. The glass pass's input
and output are the same image, so the inter-layer barrier publishes the
frost writes to both sampled reads (`texture()`) and storage reads
(`imageLoad`), a case the standalone filter never has.

Per-group backdrop statistics keep the standalone contract: they reduce
over the sharp capture and its blur, because they describe the desktop
behind the material, and are read back through
`prism_backdrop_layer_filter_stats` on the same frame-slot cadence.

## Alternatives

- **Caller-side chaining** (capture → frost → write frost into a second
  render target → glass over it). Rejected: doubles the scene capture,
  and pushes material composition into every consumer — the exact
  duplication the material library exists to prevent.
- **A per-group input texture** on `prism_liquid_glass_group`. Rejected:
  the 160-byte push-constant budget is already bit-packed, and a
  group-scoped input admits arbitrary effect graphs that [ADR-0008]
  rejected.
- **A general layer/effect-graph object in flux.** Rejected: [ADR-0017]
  already recorded that the explicit seam is enough, and revisit is
  warranted only for *nested captures*, which this is not.

## Consequences

- The frost→glass nesting becomes a reusable material; consumers state
  frost rects and glass groups and receive one image.
- One material output instead of two draws for the stack; the caller
  composites a single blit.
- `tests/prism/integration/test_backdrop_layer.c` is the pixel gate: it
  asserts the glass body's interior shows the frost's blur gradient (a
  lens pointed at the sharp capture fails the test), plus footprint
  clearing across reused slots.
- Glass-over-glass (a lens over a lens) remains out of scope; that
  relation would need its own layer material and its own ADR.

[ADR-0008]: 0008-image-effect-pipeline.md
[ADR-0017]: 0017-canvas-render-target-capture.md
[ADR-0074]: 0074-effect-intake-path.md
