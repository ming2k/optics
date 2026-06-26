# ADR-0010: Glyph-blit primitive — text rendering at the canvas seam, shaping stays out

- Status: Accepted — glyph-blit primitive implemented (see Implementation notes). The "shaping stays out" boundary is superseded by [ADR-0015](0015-text-layering.md): Layer-0 shaping now ships in libflux as the `text` module, which draws through this primitive.
- Date: 2026-05-24

## Context

Text rendering is the most-requested gap in the canvas. Every
consumer wraps `flux_canvas_draw_image` over a self-built glyph
atlas today: bilinear filtering blurs sub-pixel-positioned glyphs
(addressed independently in the `flux_canvas_draw_image_sampled`
change), no premultiplied-alpha contract is documented for coverage
textures, and there is no canonical way to express "this is a glyph
run, not an arbitrary image".

The naive scope expansion is to ship a full text stack: font loading
(FreeType), shaping (HarfBuzz), atlas packing, atlas eviction, LCD
sub-pixel positioning, colour-emoji decoding, BiDi reordering, and a
gamma-aware blend path. That scope was explicitly closed in
[ADR-0001](0001-project-foundations.md):

> Text shaping. Belongs in a sibling library wrapping HarfBuzz.

ADR-0005 also queues a related constraint: it documents that font
outlines with holes (e.g. `O`, `e`, `a`) require stencil-then-cover
and that the trigger to write that ADR is *"the moment text shaping
lands"*. Re-importing shaping into flux therefore drags a second
architectural rewrite (rasterising paths with holes) along with it.

The actual gap consumers feel is narrower: there is no primitive
that says "draw these screen-space rectangles, each sampled from a
sub-rect of this alpha texture, blended as coverage". Every UI
toolkit that targets flux has to fabricate that primitive out of
`flux_canvas_draw_image_sampled` calls — one draw per glyph, with
state churn, no batching, and no shared blend contract. That is
where the real value sits, and it is shippable without resolving
the shaping question.

## Decision

Introduce a glyph-blit primitive that takes pre-shaped runs and a
caller-owned atlas. The library does not own fonts, shaping, atlas
packing, or atlas eviction. Sibling library boundary from ADR-0001
holds. Initial public surface (spelling illustrative):

```c
typedef struct flux_glyph_quad {
    float    sx, sy;          /* screen-space top-left, post-transform */
    float    sw, sh;
    uint16_t ax, ay;          /* atlas top-left (texel) */
    uint16_t aw, ah;
    flux_color color;         /* premultiplied; per-glyph tint */
} flux_glyph_quad;

typedef struct flux_glyph_run_desc {
    flux_struct_type        type;       /* FLUX_TYPE_GLYPH_RUN_DESC */
    const void             *next;
    flux_image             *atlas;       /* caller-owned, alpha or RGBA */
    flux_sampler           *sampler;     /* typically NEAREST for crisp blits */
    const flux_glyph_quad  *quads;       /* batched in a single draw */
    uint32_t                quad_count;
    flux_blend_mode         blend;       /* default SRC_OVER premultiplied */
} flux_glyph_run_desc;

FLUX_API void flux_canvas_draw_glyph_run(flux_canvas *c,
                                          const flux_glyph_run_desc *desc);
```

Contract:

- **Pre-shaped**. Quads are positioned in screen space by the
  caller. The library does no kerning, no advance computation, no
  baseline math. A HarfBuzz output buffer maps 1:1 onto the quad
  array.
- **Caller-owned atlas**. The atlas is a `flux_image`. Single-
  channel (R8) for grayscale coverage, four-channel (RGBA8) for
  colour glyphs. The library does not pack, evict, or grow it; the
  caller owns its lifetime and updates it via
  `flux_image_update_region` (already public).
- **Single pipeline kind, batched draw**. All quads in a run hit
  one `vkCmdDraw` with one push-constants update; the vertex
  buffer is built per-call from the quad array via a transient
  slice. State machine matches `draw_image_sampled` — pipeline
  cache extended with `FLUX_PAINT_GLYPH`-equivalent (internal
  enum; not exposed in `flux_paint_kind`).
- **Premultiplied-alpha contract**. Coverage atlases are sampled
  as alpha; per-quad colour is premultiplied. Gamma is left in
  the surface's storage colour-space — same policy as the rest of
  the canvas. LCD sub-pixel AA is **out of scope** for v1; it
  requires a dual-source blend variant and a 3-channel sample
  pattern that warrant their own decision.

ADR-0005 supersession is queued but not blocking: glyph outlines
with holes are the caller's atlas-generation problem, not the
runtime's. The library's tessellator still ear-clips disjoint
contours; consumers who outline glyphs through `fill_path` for
display still benefit from the stencil-then-cover work when it
lands, but this ADR does not depend on it.

## Consequences

Positive:

- The scope flux ships matches what only flux can ship — the
  batched, pipeline-cached, bindless-friendly draw call. The
  things sibling libraries do better (shaping, atlases, font
  fallback) stay there.
- The public surface grows by one entry point, one desc struct,
  and one POD quad type. No new resource lifetime to manage.
- Existing pipeline-cache discipline (ADR-0004, ADR-0009)
  extends linearly: one new pipeline kind, keyed alongside
  paint kinds.
- The contract is testable without a font: synthesise a 4×4
  R8 atlas, draw a single quad, golden-image-compare. Headless
  tests reach as far for this primitive as they do for
  `draw_image_sampled`.

Negative:

- LCD sub-pixel AA is unavailable in v1. Consumers who need it
  fall back to per-glyph `draw_image_sampled` with a custom
  shader for now. Acceptable: LCD is a Windows-era default that
  most modern targets (HiDPI, macOS, Linux Wayland) deliberately
  don't use, and the dual-source blend path can be added
  additively once a consumer demonstrably needs it.
- The "draw a glyph run" call is one more entry point a future
  text-API audit would want to fold into a "text" object. We
  accept the duplication temporarily; the orthogonality is
  worth more than the deduplication.
- The caller has to ship a HarfBuzz dependency (and a font-cache
  + atlas) to render text. That cost is not new — every UI
  toolkit on flux already shoulders it; this ADR formalises the
  boundary.

## Alternatives considered

- **Full text stack inside flux** (font loading + shaping +
  atlas + LCD AA + colour emoji). Rejected: contradicts the
  scope boundary in ADR-0001 and would force a re-import of
  HarfBuzz/FreeType into the library's link line. The consumer
  base flux is targeting already brings these libraries; flux
  should not duplicate them.
- **`FLUX_PAINT_GLYPH` as a `flux_paint_kind`.** Rejected for
  the public enum (it's not a paint — paints describe
  appearance, glyph runs describe geometry + appearance + a
  sampler), but used internally as the pipeline-cache
  discriminator (same pattern as the private "image" kind in
  ADR-0004).
- **Single `draw_glyph` per-call (no batching).** Rejected:
  ten draw calls per text line is the exact misuse pattern this
  primitive exists to prevent.
- **Library-owned glyph atlas with explicit upload API.**
  Rejected as bigger than the gap. Once we own the atlas we
  also own packing, eviction, and the question "what happens
  when the atlas fills mid-frame" — which forces a multi-pass
  contract back into the canvas. Caller-owned keeps the canvas
  single-pass.
- **Defer entirely until a real consumer signals.** Rejected:
  every consumer doing UI on flux is the real consumer; the
  hack-around (a sequence of `draw_image_sampled` calls per
  glyph) is observable in profiles already.

## When to revisit

Promote to a richer text API — or fold a subset back into a
proper paint kind — when *any* of:

- A consumer ships LCD-AA-required text and demonstrates that
  dual-source-blend pipelines are the right addition.
- The glyph-run path becomes the dominant canvas draw, and
  batching opportunities (multi-atlas runs, glyph-instanced
  draws) become measurable.
- ADR-0005's stencil-then-cover work lands; glyph outlines with
  holes then become a reasonable secondary path for very large
  or highly-tessellated glyphs that don't fit in any atlas.

## See also

- ADR-0001 — project foundations (the scope boundary this ADR
  respected: "text shaping belongs in a sibling library" — now
  superseded by ADR-0015).
- ADR-0004 — paint kind drives pipeline selection (the pattern
  this ADR's internal pipeline kind follows).
- ADR-0005 — ear-clipping tessellator (queued supersession
  trigger for outline-with-holes glyph rasterisation).
- ADR-0008 — image-effect pipeline (the desc-struct + command-
  buffer surface shape this ADR mirrors).
- ADR-0009 — canvas sample count (composes with this primitive;
  4× MSAA + NEAREST glyph blit is the default UI text path).
- `include/flux/canvas.h` — `flux_image`, `flux_sampler` (the
  resource handles this primitive consumes).

## Implementation notes (2026-06-13)

Shipped as `flux_canvas_draw_glyph_run` with `flux_glyph_quad` /
`flux_glyph_run_desc` (`FLUX_TYPE_GLYPH_RUN_DESC`). Deltas from the
illustrative spelling above, none changing the contract:

- **No `blend` field.** v1 is premultiplied SRC_OVER like every other
  canvas draw; a blend-mode enum would have invented API for a single
  value. Added when a second mode exists.
- **`sampler` is optional.** `NULL` selects the canvas default
  (linear); callers pass a NEAREST `flux_sampler` for crisp blits.
- **R8 coverage only in v1.** The atlas's `.r` channel is coverage,
  multiplied by the per-quad premultiplied tint — the exact contract
  of `flux_canvas_draw_image_coverage`. RGBA colour-glyph atlases are
  additive later (a desc flag selecting a pass-through sample path).
- **Per-vertex UV rides the existing vertex layout.** The canvas
  vertex's `_pad` word carries the atlas UV as unorm16×2; the shared
  vertex shader unpacks it and only the glyph fragment shader consumes
  it. No new vertex format, no second buffer-reference block.
- **Batching is chunked**, one draw per `FLUX_CANVAS_PATH_SCRATCH_CAP
  * 3 / 6` quads (1024 at the current cap), so arbitrarily long runs
  work without growing the scratch.
