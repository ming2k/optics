# ADR-0051: Independent rounded image clip for composed previews

- Status: Accepted
- Date: 2026-08-03

## Context

`flux_canvas_draw_image_rrect` couples a rounded silhouette to one image's
destination. That is correct for portraits and standalone images, but a
compositor preview is assembled from several images: a toplevel, subsurfaces,
and popups. Rounding every destination independently creates false corners at
internal surface boundaries. A rectangular Canvas scissor prevents overflow
but cannot produce the rounded outer silhouette required by the product.

A stateful rounded clip or stencil mask could express the shape, but preview
composition otherwise uses an attachment-free image pass. Adding state and a
stencil dependency would enlarge the mechanism and interfere with batching for
one axis-aligned analytic clip already supported by the image shader.

## Decision

Add `flux_canvas_draw_image_clipped_rrect`. It draws an image destination
through an independent rounded-rectangle clip. Callers composing a surface
tree repeat the same clip and radius for every constituent image, producing one
shared outer silhouette rather than rounding each destination.

The implementation reuses image shader kind 5 and the existing image
destination/radius push fields. The vertex quad and UV mapping remain based on
the image destination; only the analytic distance field uses the independent
clip rectangle. No push layout, descriptor, pipeline, or state stack changes.

The operation uses source-over blending. At the antialiased clip fringe, zero
coverage must preserve the destination; an opaque source-replace variant would
instead erase it. The clip follows Canvas translation and uniform scale, which
matches the existing rounded-image contract.

## Alternatives Considered

- **Round each image destination.** Rejected because internal surfaces gain
  visible corners and cease to read as one client window.
- **Add a stateful rounded clip stack.** Rejected because it introduces global
  draw state and more complex save/restore semantics for one bounded use case.
- **Use a stencil mask.** Rejected because it requires a stencil-capable pass
  and extra draw work where the existing analytic image shader is sufficient.
- **Keep the rectangular scissor and paint a rounded overlay.** Rejected
  because it hides rather than clips client pixels and cannot guarantee exact
  antialiased coverage.

## Consequences

- Compositors can apply one analytic rounded silhouette to a multi-image
  preview while retaining each image's own destination and UV mapping.
- Repeating the shared clip is explicit at each draw and remains compatible
  with Canvas batching and recording.
- GPU integration tests verify centre coverage, rejection outside an
  independent clip, and rounded-corner rejection.

## References

- [ADR-0004 — Paint kind drives pipeline selection](0004-paint-kind-drives-pipeline.md)
- [ADR-0025 — Lens draws only through flux/canvas.h](0025-lens-draws-through-flux-canvas.md)
- [ADR-0050 — Single-body liquid-glass focus field](0050-single-body-liquid-glass-focus-field.md)
