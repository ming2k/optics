# Architecture Decision Records

ADRs are numbered, append-only, and immutable once Accepted. To revise
a decision, write a new ADR and update the old one's status to
"Superseded by ADR-NNNN".

| #    | Title                                                                          | Status   |
|------|--------------------------------------------------------------------------------|----------|
| 0001 | [Project foundations](0001-project-foundations.md)                             | Accepted |
| 0002 | [Per-module device state via opaque slot + destroy hook](0002-per-module-device-state.md) | Accepted |
| 0003 | [Bindless handles pack binding type into the high bits](0003-bindless-handle-packing.md) | Accepted |
| 0004 | [Paint kind drives pipeline selection](0004-paint-kind-drives-pipeline.md)     | Accepted |
| 0005 | [Ear-clipping tessellator with stencil-then-cover deferred](0005-tessellator-scope.md) | Superseded by ADR-0011 |
| 0006 | [No runtime RHI; build-time backend selection if ever needed](0006-no-runtime-rhi.md) | Accepted |
| 0007 | [Hand-rolled Vulkan slab allocator (no VMA)](0007-vk-slab-allocator.md)         | Accepted |
| 0008 | [Image-effect pipeline as the home for blur and friends](0008-image-effect-pipeline.md) | Accepted |
| 0009 | [Canvas sample count joins the pipeline-cache key](0009-canvas-msaa-pipeline-key.md) | Proposed |
| 0010 | [Glyph-blit primitive — text rendering at the canvas seam](0010-glyph-blit-primitive.md) | Accepted |
| 0011 | [Hole-bridging ear-clipping tessellator](0011-hole-bridging-tessellator.md) | Accepted |
| 0012 | [Phong lighting parameters ride a transient buffer-device-address block](0012-phong-light-transient-block.md) | Accepted |
| 0013 | [Offscreen rendering is a surface mode, not a new object](0013-offscreen-surface.md) | Accepted |
| 0014 | [Stencil-then-cover fallback for self-intersecting fills](0014-stencil-then-cover.md) | Accepted |
| 0015 | [Text layering — Layer-0 shaping in libflux, Layer-1 layout in flux-text-layout](0015-text-layering.md) | Superseded by ADR-0016 |
| 0016 | [Pure RHI and draw primitives — text and scene content move to sibling libraries](0016-pure-rhi-and-draw-primitives.md) | Accepted |
| 0017 | [Canvas render-target capture for real backdrop effects](0017-canvas-render-target-capture.md) | Accepted |
| 0018 | [Obsolete iris meson subprojects](0018-obsolete-iris-meson-subprojects.md) | Accepted |
| 0019 | [Canvas rendering backend seam + software (CPU) backend](0019-canvas-backend-seam-and-cpu-backend.md) | Accepted |
| 0020 | [GPU memory production hardening (amends ADR-0007)](0020-gpu-memory-production-hardening.md) | Accepted |
| 0021 | [Batched uploads, surface-scoped quiescent waits, prefers-dedicated floor](0021-batched-uploads-and-quiescent-waits.md) | Accepted |
| 0022 | [Deferred upload submission (amends ADR-0021 item 1)](0022-deferred-upload-submission.md) | Accepted |
