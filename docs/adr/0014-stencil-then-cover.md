# ADR-0014: Stencil-then-cover fallback for self-intersecting fills

- Status: Accepted
- Date: Stage 9

## Context

The ear-clipping tessellator (ADR-0005, extended by ADR-0011) cannot
triangulate self-intersecting contours: no ear exists, the bounded-step
guard trips, and `flux_canvas_fill_path` produced an empty (or partial)
fill. The roadmap has carried "stencil-then-cover is the correct future
primitive for these cases" since ADR-0005. This ADR lands it — as a
fallback only, not a replacement: the ear clip stays the primary path
because it needs no stencil state changes, no extra attachment traffic,
and produces exact geometry for the (overwhelmingly common) simple and
hole-bridged cases.

Two ownership questions had to be settled:

1. **Who owns the stencil attachment?** ADR-0001 makes depth a
   peer-supplied attachment for the scene module. The stencil here is
   different in kind: it is not part of the caller's scene, it is an
   implementation detail of fill correctness — the same category as the
   canvas's scratch buffers. Making callers supply it would leak the
   tessellator's limitation into every embedder's setup code.
2. **When is it attached?** Dynamic rendering requires a pipeline's
   `stencilAttachmentFormat` to match the pass instance's stencil
   attachment. Attaching it "only when needed" would split every canvas
   pipeline into with/without-stencil variants and force a pass break
   when a fill first stalls mid-pass.

## Decision

**The canvas owns a stencil-only attachment** (probed per device:
`S8_UINT`, then `D24_UNORM_S8_UINT`, then `D32_SFLOAT_S8_UINT`), sized
to the surface and recreated on extent change. **Every canvas pass
carries it and every canvas pipeline declares it**; the cost when no
fill stalls is one cleared, never-stored attachment.

`flux_pass_desc` gains a public `stencil` field (same
`flux_pass_depth_attachment` type, depth fields ignored) so the canvas
can route it through `flux_frame_begin_pass`; the field is usable by
raw-Vulkan consumers too.

`ear_clip_contour` now returns `false` on a stall instead of silently
emitting whatever it had. `flux_canvas_fill_path` then discards the
partial triangulation and redoes the whole fill in two passes:

1. **Stencil write**: a triangle fan per contour (winding preserved,
   color writes masked off) with `INCREMENT_AND_WRAP` on front faces
   and `DECREMENT_AND_WRAP` on back faces — after this, each pixel's
   stencil value is its nonzero winding count.
2. **Cover**: the fill's bounding quad drawn with the paint's pipeline
   variant, stencil test `NOT_EQUAL 0`, and `VK_STENCIL_OP_ZERO` on
   both pass and fail — painting the winding region and wiping the
   attachment clean for the next fill in the same pass.

The fallback fills by the **nonzero winding rule**, which agrees with
the ear-clip path's hole semantics (CW holes in CCW outers cancel).

## Consequences

- Self-intersecting paths (pentagrams, figure-eights, malformed input)
  now fill correctly instead of disappearing. The roadmap known-gap is
  closed.
- On a device with no stencil-capable format (not observed in
  practice; Vulkan requires one of `D24S8`/`D32S8` to support
  DEPTH_STENCIL_ATTACHMENT), pipelines declare `UNDEFINED`, no
  attachment is bound, and the old bail-out behavior remains.
- Raw-Vulkan draws recorded *inside* a canvas pass must now use
  pipelines declaring the canvas stencil format. Mixing raw pipelines
  into the canvas's own pass was never a documented pattern (the
  documented seam is `flux_frame_begin_pass` with your own pass);
  noted here for completeness.
- The cover quad covers the fill's bounding box, so gradient paints
  evaluate identically to the tessellated path (gradients are
  computed in pixel space from push constants, not per-vertex).
- `flux_canvas_destroy` now waits for device idle when a stencil
  image exists, since in-flight frames may still reference it.
