# ADR-0017: Canvas render-target capture for real backdrop effects

- Status: Accepted
- Date: 2026-06-26

## Context

ADR-0008 landed the image-effect pipeline and named, as the explicit
shape of a drop shadow, a three-step recipe:

> rasterize the path's alpha mask into a scratch image (a future canvas
> capability) → blur it → composite underneath the geometry.

That **future canvas capability** — the canvas rendering into a
`flux_image` that the effect module can then sample — never landed.
Today the canvas renders into exactly one target: the `flux_frame`'s
swapchain image (`flux_canvas_begin(c, frame, clear)`). There is no
`flux_canvas_begin_target`. ADR-0013 added an *offscreen surface*, but
that captures a whole frame synchronously into a staging buffer for
read-back (`flux_surface_read_pixels`) — it does not hand back a
sampleable `flux_image` with a bindless handle, and it is not usable
mid-frame.

This was surfaced concretely while writing a liquid-glass backdrop-blur
example (`examples/liquid_glass.c`). The desired effect is "blur
whatever the canvas has drawn behind the glass panel." With the current
canvas there is no way to capture that rendered content, so the example
falls back to a workaround:

- Generate one procedural texture on the CPU.
- Draw it **sharp** as the full-screen backdrop.
- Run `flux_effect_blur` on it **before** `flux_canvas_begin`
  (the compute dispatch can't run inside the canvas's active render
  pass — a Vulkan invariant).
- Draw the **blurred** copy behind the glass, aligned to the same
  screen position as the sharp tiles.

The blur therefore reflects a *stand-in texture*, not the actual
rendered scene. If the scene behind the glass were live UI (widgets,
text, a video frame), the workaround would be wrong: it can only blur
its own static source.

Three compounding gaps made the workaround the only option:

1. **No canvas → `flux_image` path.** The canvas can't render into a
   sampleable image the effect pipeline consumes.
2. **`flux_canvas_draw_image` ignores its `paint` argument**
   (`canvas.c`: `(void)paint; /* Stage 4.2.4 doesn't honour tint/blend
   yet */`). Even with a captured image there is no per-pixel tint,
   mask, or blend on an image draw — the glass body had to be rebuilt
   from stacked gradient *fills* rather than one tinted image draw.
3. **Only `clip_rect`, no `clip_path`.** The blurred image could be
   scissored to the glass's bounding box but not to its rounded
   outline; the corner seam had to be hidden behind a gradient layer.

(2) and (3) are quality issues. **(1) is structural**: it is what makes
a true backdrop blur impossible on the current canvas regardless of how
much effort goes into the shader work.

## Decision

Add a **render-target capture seam** to the canvas so its output can
feed the effect pipeline. Proposed v1 shape (illustrative spelling):

```c
/* Render the draws between begin/end into `target` instead of the
 * frame's swapchain image. `target` is a caller-owned flux_image
 * (SAMPLED + COLOR_ATTACHMENT) created at the desired extent/format.
 * Returns FLUX_ERROR_INVALID_STATE if a frame pass is already active. */
FLUX_NODISCARD FLUX_API flux_result flux_canvas_begin_target(
    flux_canvas *c, flux_image *target, const flux_color *clear_color);
FLUX_API void flux_canvas_end_target(flux_canvas *c);
```

Properties:

- The captured `target` is a regular `flux_image` with a sampled
  bindless handle, so `flux_effect_blur(cmd, { .input = target, ... })`
  and `flux_canvas_draw_image(c, target, ...)` work on it unchanged.
- `begin_target`/`end_target` opens and closes its own dynamic-rendering
  pass on the target image, so a capture does not require breaking the
  *primary* frame's pass. A capture can run while a frame is in flight
  but not nested inside the frame's own `canvas_begin`/`canvas_end`
  (a single active pass at a time, enforced by `FLUX_ERROR_INVALID_STATE`).
- Layout and sync are owned by the canvas: `target` is transitioned to
  `COLOR_ATTACHMENT_OPTIMAL` on begin and to
  `SHADER_READ_ONLY_OPTIMAL` on end, with the barrier already in the
  command stream — so the immediately-following effect or draw needs no
  caller-side synchronisation, mirroring `flux_effect_blur`'s trailing
  barrier contract.

This deliberately does **not** introduce a Skia-style
`saveLayer`/`restore` (ADR-0008 rejected that for the canvas, and this
ADR inherits the rejection). The seam is explicit: the caller says
*render this rectangle of content into this image*, then composites it.
No implicit layer stack, no effect-graph object.

Two follow-on gaps are scoped but **not** required to land with this
ADR, and are called out so the seam is designed to absorb them:

- **Image draw honouring `paint`** (gap 2): once `draw_image` tints and
  blends per its paint, a captured-and-blurred backdrop can be drawn in
  one call with a thickness mask + tint. The seam returns the image;
  improving `draw_image` is independent.
- **`flux_canvas_clip_path`** (gap 3): lets the blurred backdrop be
  clipped to the rounded outline instead of a bounding box. Independent
  of capture; orthogonal quality win.

## Consequences

Positive:

- **Real backdrop blur, drop shadow, bloom, and refraction** become
  expressible as the three-step recipe ADR-0008 always assumed: capture
  → effect → composite. The liquid-glass example drops its static-
  texture workaround and blurs the actual scene behind the panel.
- The effect module gains a first-class input source (the canvas)
  instead of only caller-uploaded `flux_image`s. ADR-0008's "effects
  compose through `flux_image`" promise is fulfilled end to end.
- Headless thumbnails, compositor integration, and "render this widget
  to a texture" all reduce to the same primitive.
- The primary frame's render pass stays single and uninterrupted for
  consumers who never capture — zero cost when unused.

Negative:

- The canvas gains a second render-pass state (target vs frame). This is
  real new state-machine surface, though bounded: one target pass at a
  time, enforced, no nesting with the frame pass.
- A capture mid-frame costs a pass open/close + layout transitions. For
  a per-frame backdrop blur that is the intended cost; for callers who
  don't need it, it's opt-in.
- `flux_image_create` today produces images with `SAMPLED |
  TRANSFER_DST` usage. A capture target additionally needs
  `COLOR_ATTACHMENT` — either a new desc flag or a dedicated
  `flux_image_create_render_target` constructor. Either is additive.

## Alternatives considered

- **`saveLayer`/`restore` on the canvas.** Rejected for the same reason
  ADR-0008 rejected it: forces the canvas to own an implicit layer
  stack, offscreen target management, and blend tracking. The explicit
  `begin_target`/`end_target` seam gives the same capability without
  the state-machine expansion.
- **Capture via the offscreen surface (ADR-0013).** Rejected as the
  primary path: it captures a whole frame synchronously into a staging
  buffer, returns bytes, not a sampleable `flux_image`, and cannot run
  mid-frame. It remains the right tool for headless read-back and
  testing; it is the wrong tool for live backdrop effects.
- **Make `flux_effect_blur` capture the frame directly.** Rejected: it
  would couple the effect module to canvas frame internals and the
  swapchain image's layout/lifetime, breaking the "effects are
  image-in/image-out" contract ADR-0008 established. The capture
  belongs on the canvas; the effect stays a pure image operator.
- **Defer until a second consumer appears.** Rejected as premature
  caution: backdrop blur, drop shadow, and bloom are all the *same*
  capture → effect → composite shape. The liquid-glass example is the
  first consumer to hit the wall, but the wall is shared.

## When to revisit

- Promote to a layer model (`saveLayer`/`restore`) only if a real
  consumer needs **nested** captures with automatic blend tracking —
  the explicit seam cannot express that, and forcing it to would
  re-open the state-machine expansion this ADR avoids.
- If capture frequency makes per-pass layout transitions a measured
  cost, reconsider a render-graph or pass-merging layer; until then the
  explicit seam is cheaper to reason about.

## See also

- ADR-0008 — image-effect pipeline; this ADR lands the "future canvas
  capability" it named but never shipped.
- ADR-0013 — offscreen surface (the read-back counterpart; this ADR is
  its sampleable-image counterpart for live effects).
- `examples/liquid_glass.c` — the first consumer; carries the
  static-texture workaround this ADR retires.
- `include/flux/canvas.h` — `flux_canvas_begin`/`_end`, `flux_image`,
  `flux_canvas_draw_image`.
