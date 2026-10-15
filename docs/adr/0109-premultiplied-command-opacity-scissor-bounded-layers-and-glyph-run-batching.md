---
id: ADR-0109
title: "Premultiplied Command Opacity Modulation, Scissor-Bounded Vulkan Layer Passes, and GPU Glyph Run Batching"
status: accepted
date: 2026-10-14
scope: libs/lens, libs/flux, libs/glyph, bindings/lens-rs, tests/lens, tests/flux
superseded_by: null
negative_knowledge: true
---

# ADR-0109: Premultiplied Command Opacity Modulation, Scissor-Bounded Vulkan Layer Passes, and GPU Glyph Run Batching

- Status: Accepted
- Date: 2026-10-14
- Scope: `libs/lens`, `libs/flux`, `libs/glyph`, `bindings/lens-rs`, `tests/lens`, `tests/flux`.
- Amends: [ADR-0068](0068-lens-frame-scoped-node-stamped-opacity.md), [ADR-0078](0078-lens-ghost-replay-leave-surface.md), [ADR-0091](0091-drawing-group-and-effect-semantics.md), [ADR-0094](0094-lens-scene-snapshots-and-invalidation.md).

## Context

Profiling high-refresh desktop compositor workloads (60Hz/120Hz/144Hz at 4K resolution) during modal overlay reveals and status-bar fades exposed four compounding architectural defects across `lens`, `flux`, and `glyph`:

1. **Per-Node Full-Screen `save_layer` Explosion in `lens`**:
   `lensi_store_touch` (`libs/lens/src/store/store.c`) stamps the frame-scoped `ui->opacity` onto every node constructed during a faded subtree. In `lensi_compile_node_body` (`libs/lens/src/render/replay.c`) and `compile_ghost_node` (`libs/lens/src/render/ghost.c`), every node with `0.0 <= opacity < 1.0` was unconditionally wrapped in `flux_encoder_save_layer` / `flux_encoder_restore` — even layout containers with `cmd_count == 0`. For a 200-node panel fade, this emitted 200 separate offscreen `RGBA16_SFLOAT` render-pass transitions, full-screen clears, and full-screen composite quads per frame (~30 GB of VRAM bandwidth per frame at 4K).
2. **Premultiplied Alpha Violation in `lensi_opacity_color`**:
   `flux_color` is packed premultiplied sRGB (`0xAARRGGBB` where `R, G, B <= A`), and both the GPU vertex shader (`flux_decode_premul_srgb`) and CPU backend (`flux_color_to_linear`) un-premultiply by `1.0 / a` before linearizing. However, `lensi_opacity_color` scaled only the `A` byte while leaving `R, G, B` unscaled, producing invalid premultiplied colors (`R, G, B > A`) that caused additive highlight blowout when decoded into linear light.
3. **Unbounded Full-Screen `vk_save_layer` Clear/Blit and Stencil Desynchronization in `flux`**:
   In `libs/flux/src/canvas/backend_vk.c`, `vk_save_layer` opened its offscreen `RGBA16_SFLOAT` pass across the entire `fb_width × fb_height` without restricting `renderArea` to the active scissor, and `vk_restore_layer` drew a full-screen `(0, 0, fb_width, fb_height)` quad. Furthermore, `vk_save_layer` set `v->active_stencil = false` without updating `c->stencil_available = false`, causing path tessellation (`geometry_tess.c`) inside a layer to emit stencil-write pipeline draws (`CANVAS_PIPE_STENCIL_WRITE*`) that failed `get_canvas_pipeline_id` with `FLUX_ERROR_INVALID_ARGUMENT` and dropped vector icon draws during opacity animations.
4. **Per-Character Glyph Run Fragmentation on GPU Canvases in `glyph`**:
   In `draw_layout_pass` (`libs/glyph/src/layout.c`), the pre-fetch host-coverage capture check `if (encoder && n > 0)` lacked a `!t->atlas` guard, flushing a separate `FLUX_CMD_DRAW_GLYPH_RUN` command for every single character (`quad_count = 1`) even on GPU-backed atlases where `run.host_coverage` is `NULL`.

## Decision

1. **Premultiplied Command-Level Opacity Modulation in `lens`**:
   - `lensi_opacity_color` scales all four premultiplied channels (`A, R, G, B`) proportionally by `o` (clamped to `[0, 1]`), preserving `R, G, B <= A` and exact linear-space equivalence with `flux_color_rgba_premul`.
   - `lensi_compile_node_body` and `compile_ghost_node` pass node opacity (`n->opacity` and `ghost_alpha * g->opacity`) directly to `lensi_compile_commands`, eliminating per-node `flux_encoder_save_layer` wrappers and skipping emission entirely when `cmd_count == 0` or `alpha <= 0.0f`.
   - Multi-run SVG icons (`LENS_DRAW_ICON`) modulate run-specific colors through `lensi_opacity_color`.
   - `lens::Color::with_alpha` in `bindings/lens-rs` un-premultiplies non-opaque source colors before re-premultiplying with the new alpha.
2. **Scissor-Bounded `vk_save_layer` / `vk_restore_layer` and Stencil State Synchronization in `flux`**:
   - `vk_save_layer` records the active scissor (`c->states[c->state_top].scissor`) into `layer->area` and restricts the offscreen `flux_pass_desc` (`render_offset_x`, `render_offset_y`, `width`, `height`) to that scissor region.
   - `vk_restore_layer` composites only the `layer->area` sub-rectangle (`dst` and normalized UV `src`) back onto the parent pass.
   - `vk_save_layer` synchronizes `c->stencil_available = false` alongside `v->active_stencil = false`, and `vk_restore_layer` restores `c->stencil_available = layer->saved_stencil`, ensuring path fills inside explicit layers cleanly use the non-stencil tessellation path.
3. **Full-Batch GPU Glyph Runs in `glyph`**:
   - `draw_layout_pass` restricts per-glyph pre-fetch flushes to device-less host-coverage recorders (`if (encoder && !t->atlas && n > 0)`), allowing GPU-backed glyph runs to batch up to `TEXT_RUN_BATCH` (256) quads per command.
   - Mid-run atlas clear and page-rollover flushes dispatch to both `canvas` and `encoder` targets after flushing pending atlas uploads.

## Rejected Alternatives & Negative Knowledge

### 1. Retaining `flux_encoder_save_layer` only on container nodes in `lens`
- **Why considered**: Wrapping only top-level containers in `save_layer` while leaving leaf nodes at `opacity = 1.0` would reduce the number of offscreen passes from ~200 to ~10.
- **Why rejected**: `lensi_store_touch` stamps `ui->opacity` onto every node in the scope (`ADR-0068`), and individual `lens` nodes (labels, icons, buttons, cards) have non-overlapping internal draw commands or rely on theme alpha semantics. Even 10 offscreen render-pass switches per frame break single-pass command batching and incur unnecessary `RGBA16_SFLOAT` bandwidth. Command-level premultiplied modulation executes in zero extra passes while keeping `flux_canvas_save_layer` available as an explicit primitive in `flux`.

### 2. Allocating a separate stencil attachment per `canvas_vk_layer`
- **Why considered**: Allocating a dedicated `S8_UINT` image per layer slot would allow stencil-then-cover inside `vk_save_layer`.
- **Why rejected**: Multiplies transient VRAM footprint across `FLUX_VK_MAX_LAYERS` when the CPU ear-clipping tessellator in `geometry_tess.c` already handles arbitrary paths whenever `c->stencil_available == false`. Synchronizing `c->stencil_available` with `v->active_stencil` achieves full vector rendering fidelity at zero memory cost.

## Consequences

- Eliminates hundreds of full-screen `RGBA16_SFLOAT` render passes and gigabytes of per-frame VRAM traffic during UI fade animations.
- Restores accurate premultiplied sRGB-to-linear decoding across all faded primitives, text runs, and icons without highlight blowout.
- Explicit `flux_canvas_save_layer` calls on the Vulkan backend clear and composite strictly within the active scissor rectangle and never drop vector paths.
- GPU text recording batches up to 256 glyphs per command instead of 1 command per glyph.
