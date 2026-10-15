# Animation, Opacity, and Layer Performance Know-How

Engineering know-how and root-cause patterns for high-refresh (60Hz/120Hz/144Hz) UI fade animations, premultiplied sRGB color pipelines, Vulkan offscreen layer passes, and GPU text recording across `lens`, `flux`, and `glyph` (see [ADR-0109](../adr/0109-premultiplied-command-opacity-scissor-bounded-layers-and-glyph-run-batching.md)).

## 1. Core Architectural Invariants

1. **Command-Level Opacity Over Per-Node Offscreen Layers (`lens`)**:
   - `lens_set_opacity` stamps `ui->opacity` onto every node allocated while the scope is active (`lensi_store_touch` in `libs/lens/src/store/store.c`).
   - During `lensi_compile_node_body` (`libs/lens/src/render/replay.c`) and `compile_ghost_node` (`libs/lens/src/render/ghost.c`), node opacity must be passed directly into `lensi_compile_commands(..., n->opacity)` rather than wrapping every individual `lens_node` in `flux_encoder_save_layer` / `flux_encoder_restore`.
   - Wrapping each node in `save_layer` turns a 200-node panel fade into 200 separate offscreen `RGBA16_SFLOAT` render-pass interruptions, clears, pipeline barriers, and full-screen composite draws per frame (~30 GB/frame at 4K).

2. **Premultiplied sRGB Scaling Invariant (`R, G, B <= A`)**:
   - `flux_color` stores premultiplied sRGB channels (`0xAARRGGBB` where `R, G, B` are already multiplied by `A / 255`).
   - Both the Vulkan vertex shader (`flux_decode_premul_srgb` in `canvas_color.glsl`) and the CPU rasterizer (`flux_color_to_linear` in `color.c`) un-premultiply by dividing `rgb / a` before applying the sRGB-to-linear transfer function, then re-multiply by `a`.
   - Therefore, any opacity modulation helper (`lensi_opacity_color`) **must scale all four channels (`A, R, G, B`) proportionally by `opacity`**. Scaling only the `A` byte leaves `R, G, B > A`, which causes `rgb / a > 1.0` during linear decoding and produces severe additive highlight blowout.
   - Similarly, `lens::Color::with_alpha(a)` in Rust bindings must un-premultiply non-opaque colors (`0 < a0 < 255`) before re-premultiplying with the new alpha `a`.

3. **Scissor-Bounded `vk_save_layer` and Stencil State Synchronization (`flux`)**:
   - When an explicit `flux_canvas_save_layer` is invoked on the Vulkan backend (`libs/flux/src/canvas/backend_vk.c`), the offscreen `RGBA16_SFLOAT` pass must restrict its `flux_pass_desc` (`render_offset_x`, `render_offset_y`, `width`, `height`) to the active scissor `c->states[c->state_top].scissor` (`layer->area`), and `vk_restore_layer` must composite only that `layer->area` sub-rectangle.
   - Note that the `bounds` parameter passed to `flux_canvas_save_layer` is an allocation hint, not an implicit clip ([ADR-0091](../adr/0091-drawing-group-and-effect-semantics.md)); only the active canvas scissor truncates content.
   - Because offscreen layer passes do not bind a stencil attachment (`v->active_stencil = false`), `vk_save_layer` **must also set `c->stencil_available = false`** (and `vk_restore_layer` must restore `c->stencil_available = layer->saved_stencil`). Otherwise, `geometry_tess.c` inspects `c->stencil_available == true`, emits `CANVAS_PIPE_STENCIL_WRITE*` draws inside a stencil-less pass, and `get_canvas_pipeline_id` fails with `FLUX_ERROR_INVALID_ARGUMENT`, dropping vector paths during animations.

4. **GPU Glyph Run Batching (`glyph`)**:
   - In `draw_layout_pass` (`libs/glyph/src/layout.c`), flushing `flux_encoder_draw_glyph_run` before `txt_glyph_get` is only required on device-less CPU canvases (`!t->atlas`) where `run.host_coverage` points into mutable host memory that an `atlas_clear` could overwrite in place.
   - On GPU canvases (`t->atlas != NULL`), glyph quads must accumulate up to `TEXT_RUN_BATCH` (256 quads) per `FLUX_CMD_DRAW_GLYPH_RUN` command.

---

## 2. Diagnostic Recipe

| Symptom | Root-Cause Location | Verification / Check |
|---------|---------------------|----------------------|
| Severe frame drops / GPU bandwidth saturation whenever a panel or HUD fades (`0 < opacity < 1`) | Per-node `flux_encoder_save_layer` emission in `replay.c` or unbounded full-screen `vk_save_layer` in `backend_vk.c` | Inspect `flux_canvas_get_plan_stats(canvas).pass_count` during fade frames; an entire UI snapshot should execute in 1 pass unless explicit group layers are requested. |
| Semi-transparent text, icons, or surfaces glow overly bright / blow out mid-fade | `lensi_opacity_color` or `Color::with_alpha` scaling only `A` instead of all 4 premultiplied channels `(A, R, G, B)` | Check `test_opacity.c`: `lensi_opacity_color(flux_color_rgba_premul(r, g, b, 255), 0.5f)` must equal `flux_color_rgba_premul(r, g, b, 128)`. |
| Complex SVG icons or even-odd paths disappear while inside `save_layer` and pop back at rest | `c->stencil_available` out of sync with `v->active_stencil` inside `vk_save_layer` | Check `flux_get_last_error()` for `"invalid canvas pipeline sample count or id"` from `renderer.c:get_canvas_pipeline_id`. |
| High CPU command overhead on text-heavy screens | `draw_layout_pass` flushing `flux_encoder_draw_glyph_run` per character when `t->atlas != NULL` | Verify `if (encoder && !t->atlas && n > 0)` guard in `libs/glyph/src/layout.c`. |
