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
| 0007 | [Hand-rolled Vulkan slab allocator (no VMA)](0007-vk-slab-allocator.md)         | Accepted (amended by [0020](0020-gpu-memory-production-hardening.md)) |
| 0008 | [Image-effect pipeline as the home for blur and friends](0008-image-effect-pipeline.md) | Accepted |
| 0009 | [Canvas sample count joins the pipeline-cache key](0009-canvas-msaa-pipeline-key.md) | Superseded by ADR-0071 |
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
| 0020 | [GPU memory production hardening (amends ADR-0007)](0020-gpu-memory-production-hardening.md) | Accepted (item 5 amended by [0021](0021-batched-uploads-and-quiescent-waits.md)) |
| 0021 | [Batched uploads, surface-scoped quiescent waits, prefers-dedicated floor](0021-batched-uploads-and-quiescent-waits.md) | Accepted |
| 0022 | [Deferred upload submission (amends ADR-0021 item 1)](0022-deferred-upload-submission.md) | Accepted |
| 0023 | [Unified monorepo build](0023-unified-monorepo-build.md) | Accepted |

|      |                                                                  |          |
|------|------------------------------------------------------------------|----------|
| 0024 | [Lens project foundations](0024-lens-foundations.md) | Accepted |
| 0025 | [Lens draws only through \<flux/canvas.h\>](0025-lens-draws-through-flux-canvas.md) | Accepted |
| 0026 | [Lens widget identity — FNV-1a hashing over an id stack](0026-lens-id-system.md) | Accepted |
| 0027 | [Lens retained store — open-addressing id→node map with ENTERING/STABLE/LEAVING GC](0027-lens-retained-store.md) | Accepted |
| 0028 | [Lens two-phase flexbox layout (measure / arrange)](0028-lens-flexbox-layout.md) | Accepted (amended by [0060](0060-lens-single-tree-placement-and-z-bands.md)) |
| 0029 | [Lens interaction model — prev-frame geometry, one-frame hit-test latency](0029-lens-interaction-model.md) | Accepted (amended 2026-08-10) |
| 0030 | [Lens damage / redraw tracking — deferred, full-tree repaint today](0030-lens-damage-tracking.md) | Accepted (Decision 3 superseded by implementation; see status note) |
| 0031 | [Lens symbol namespaces — public `lens_*`, internal `lensi_*`](0031-lens-symbol-namespaces.md) | Accepted |
| 0032 | [Lens theme token system — sized struct with ABI guard](0032-lens-theme-tokens.md) | Accepted |
| 0033 | [Lens text seam — draw and shape through flux-text, no in-tree font engine](0033-lens-text-seam.md) | Accepted |
| 0034 | [Lens text measurement — host port + monospace fallback](0034-lens-text-measurement.md) | Accepted |
| 0035 | [Lens accessibility semantic tree — per-node records and post-end walk](0035-lens-accessibility-tree.md) | Accepted (amended by [0060](0060-lens-single-tree-placement-and-z-bands.md)) |
| 0036 | [Lens input / clipboard / IME — host-supplied, size-guarded ABI](0036-lens-input-clipboard-ime.md) | Accepted |
| 0037 | [Lens overlay layers — transient overlays + persistent floating panels](0037-lens-overlay-layers.md) | Superseded by ADR-0060 |
| 0038 | [Lens node state GC — 8-frame grace window for leaving nodes](0038-lens-node-state-gc.md) | Accepted |
| 0039 | [Lens modal dialog — centered overlay + backdrop + Tab focus trap](0039-lens-modal-dialog.md) | Accepted (amended 2026-08-10) |
| 0040 | [Lens menus — menubar, context menu, submenu, items (hover-dwell)](0040-lens-menus.md) | Accepted |
| 0041 | [Lens resizable split panel — persisted ratio, draggable divider](0041-lens-resizable-split.md) | Accepted |
| 0042 | [Lens virtualized table / data grid](0042-lens-virtualized-table.md) | Accepted |
| 0043 | [Iris project foundations — L3 application toolkit](0043-iris-foundations.md) | Accepted |
| 0044 | [Iris backend compile-time selection](0044-iris-backend-selection.md) | Accepted |
| 0045 | [Iris host resource lifecycle callbacks](0045-iris-host-resource-lifecycle.md) | Accepted |
| 0046 | [Liquid glass as a convex-lens material](0046-liquid-glass-convex-lens-model.md) | Accepted |
| 0047 | [Caller-owned policy boundary for flux effects](0047-caller-owned-policy-boundary-for-flux-effects.md) | Superseded by ADR-0063 |
| 0048 | [Textured surface materials and core images](0048-textured-surface-materials-and-core-images.md) | Accepted |
| 0049 | [Strict DRM identity for Vulkan device selection](0049-strict-drm-vulkan-device-selection.md) | Accepted |
| 0050 | [Single-body liquid-glass focus field](0050-single-body-liquid-glass-focus-field.md) | Accepted |
| 0051 | [Independent rounded image clip for composed previews](0051-independent-rounded-image-clip.md) | Accepted |
| 0052 | [flux platform abstraction layer + Linux-only dma-buf gate](0052-flux-platform-layer-and-dmabuf-gate.md) | Accepted |
| 0053 | [Shader embedding — C23 #embed with generated-header fallback](0053-shader-embed-fallback.md) | Accepted |
| 0054 | [Font discovery as a platform layer (fontconfig / DirectWrite / CoreText)](0054-font-discovery-platform-layer.md) | Accepted |
| 0055 | [Callback-driven watch APIs + the backend wakeup seam](0055-watch-apis-and-wakeup-seam.md) | Accepted |
| 0056 | [Win32 + Cocoa backends and the MoltenVK baseline](0056-win32-cocoa-backends-and-moltenvk.md) | Accepted |
| 0057 | [Paste drain and caret rect for app-owned editing surfaces](0057-paste-drain-and-caret-rect-for-app-surfaces.md) | Accepted |
| 0058 | [Lens widget state bitflags and per-instance styles](0058-lens-widget-state-and-instance-styles.md) | Accepted (amended by ADR-0061) |
| 0059 | [Lens widget skins — emission as a replaceable function](0059-lens-widget-skins.md) | Accepted (extended by ADR-0061) |
| 0060 | [Lens single-tree placement and z bands — place/band supersedes parallel overlay roots](0060-lens-single-tree-placement-and-z-bands.md) | Accepted |
| 0061 | [Lens style cascade and the mechanism / neutral-default / flavor rule](0061-lens-style-cascade-mechanism-neutral-flavor.md) | Accepted |
| 0062 | [Bidirectional accessibility — a11y action invocation and text-changed events](0062-lens-bidirectional-a11y-action-and-text-changed.md) | Accepted |
| 0063 | [Liquid glass moves to the prism material library](0063-liquid-glass-material-library.md) | Accepted |
| 0064 | [Host-controlled caret and selection for lens_textfield](0064-lens-textfield-host-caret-selection.md) | Accepted |
| 0065 | [Per-group material overrides and backdrop statistics](0065-per-group-overrides-and-backdrop-stats.md) | Accepted |
| 0066 | [Keyboard cursor, icons, and host-owned selection for lens_table](0066-lens-table-keyboard-icons-host-selection.md) | Accepted |
| 0067 | [Wayland text-input depth — key repeat, compose, per-widget IME sessions](0067-wayland-text-input-depth.md) | Accepted |
| 0068 | [Frame-scoped, node-stamped opacity for lens](0068-lens-frame-scoped-node-stamped-opacity.md) | Accepted |
| 0069 | [Color management — parametric color spaces, scRGB working space, explicit output transform](0069-color-management.md) | Accepted (amended by ADR-0070) |
| 0070 | [ICC profile support — in-tree C parser over vendored skcms](0070-icc-in-tree-parser.md) | Accepted |
| 0071 | [Pass-scoped canvas antialiasing policy](0071-pass-scoped-canvas-antialiasing.md) | Accepted |
| 0072 | [Long-session resource governance — bounded queues, O(1) eviction, live-list GC](0072-long-session-resource-governance.md) | Accepted |
| 0073 | [Widget-kind extension range and the user-widget contract](0073-lens-user-widget-kind-range.md) | Accepted |
| 0074 | [Effect intake path — new visual operators and where choreography never enters](0074-effect-intake-path.md) | Accepted |
