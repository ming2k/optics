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
| 0018 | [Meson subprojects for the flux + lens + iris stack](0018-obsolete-iris-meson-subprojects.md) | Superseded by ADR-0023 |
| 0019 | [Canvas rendering backend seam + software (CPU) backend](0019-canvas-backend-seam-and-cpu-backend.md) | Accepted |
| 0020 | [GPU memory production hardening (amends ADR-0007)](0020-gpu-memory-production-hardening.md) | Accepted |
| 0021 | [Batched uploads, surface-scoped quiescent waits, prefers-dedicated floor](0021-batched-uploads-and-quiescent-waits.md) | Accepted |
| 0022 | [Deferred upload submission (amends ADR-0021 item 1)](0022-deferred-upload-submission.md) | Accepted |
| 0023 | [Unified monorepo build](0023-unified-monorepo-build.md) | Accepted |

|      |                                                                  |          |
|------|------------------------------------------------------------------|----------|
| 0024 | [Lens project foundations](0024-lens-foundations.md) | Accepted |
| 0025 | [Lens draws only through \<flux/canvas.h\>](0025-lens-draws-through-flux-canvas.md) | Accepted |
| 0026 | [Lens widget identity — FNV-1a hashing over an id stack](0026-lens-id-system.md) | Accepted |
| 0027 | [Lens retained store — open-addressing id→node map with ENTERING/STABLE/LEAVING GC](0027-lens-retained-store.md) | Accepted |
| 0028 | [Lens two-phase flexbox layout (measure / arrange)](0028-lens-flexbox-layout.md) | Accepted |
| 0029 | [Lens interaction model — prev-frame geometry, one-frame hit-test latency](0029-lens-interaction-model.md) | Accepted |
| 0030 | [Lens damage / redraw tracking — deferred, full-tree repaint today](0030-lens-damage-tracking.md) | Accepted |
| 0031 | [Lens symbol namespaces — public `lens_*`, internal `lensi_*`](0031-lens-symbol-namespaces.md) | Accepted |
| 0032 | [Lens theme token system — sized struct with ABI guard](0032-lens-theme-tokens.md) | Accepted |
| 0033 | [Lens text seam — draw and shape through flux-text, no in-tree font engine](0033-lens-text-seam.md) | Accepted |
| 0034 | [Lens text measurement — host port + monospace fallback](0034-lens-text-measurement.md) | Accepted |
| 0035 | [Lens accessibility semantic tree — per-node records and post-end walk](0035-lens-accessibility-tree.md) | Accepted |
| 0036 | [Lens input / clipboard / IME — host-supplied, size-guarded ABI](0036-lens-input-clipboard-ime.md) | Accepted |
| 0037 | [Lens overlay layers — transient overlays + persistent floating panels](0037-lens-overlay-layers.md) | Accepted |
| 0038 | [Lens node state GC — 8-frame grace window for leaving nodes](0038-lens-node-state-gc.md) | Accepted |
| 0039 | [Lens modal dialog — centered overlay + backdrop + Tab focus trap](0039-lens-modal-dialog.md) | Accepted |
| 0040 | [Lens menus — menubar, context menu, submenu, items (hover-dwell)](0040-lens-menus.md) | Accepted |
| 0041 | [Lens resizable split panel — persisted ratio, draggable divider](0041-lens-resizable-split.md) | Accepted |
| 0042 | [Lens virtualized table / data grid](0042-lens-virtualized-table.md) | Accepted |
| 0043 | [Iris project foundations — L3 application toolkit](0043-iris-foundations.md) | Accepted |
| 0044 | [Iris backend compile-time selection](0044-iris-backend-selection.md) | Accepted |
| 0045 | [Iris host resource lifecycle callbacks](0045-iris-host-resource-lifecycle.md) | Accepted |
| 0046 | [Liquid glass as a convex-lens material](0046-liquid-glass-convex-lens-model.md) | Accepted |
| 0047 | [Caller-owned policy boundary for flux effects](0047-caller-owned-policy-boundary-for-flux-effects.md) | Accepted |
| 0048 | [Textured surface materials and core images](0048-textured-surface-materials-and-core-images.md) | Accepted |
