---
id: ADR-0175
title: "Per-Corner Analytic Squircle SDF, Unified RRect Geometry, and Single-Representation Corner Radii"
status: accepted
date: 2026-11-27
scope: [gfx, ui, shaders, rust]
superseded_by: null
negative_knowledge: true
---

# ADR-0175: Per-Corner Analytic Squircle SDF, Unified RRect Geometry, and Single-Representation Corner Radii

- Status: Accepted
- Date: 2026-11-27
- Amends: [ADR-0088](0088-optics-clean-break-architecture-display-lists-orthogonal-brushes-and-transient-graphs.md), [ADR-0118](0118-coverage-strategy-placement-distance-fields-and-curve-kernel.md), [ADR-0157](0157-display-list-serialization-protocol.md)

## Context

Three structural cracks surfaced when desktop chrome attempted to render bezel-flush edge tabs (90° against the display edge, continuous curvature facing the desktop) and quarter-arc corner triggers:

1. **Missing per-corner radii in `gfx` and `ui`**: ADR-0088 specified `RRECT` as a rounded rectangle with per-corner radii, but the implementation collapsed it into a single `float radius` and introduced a second, overlapping geometry variant `OPTICS_GFX_GEOM_SQUIRCLE`. Downstream callers tried to fake bezel-flush shapes by placing negative/overflowing rectangles outside the viewport, which `optics_ui`'s `OPTICS_UI_PLACE_EXACT` viewport clamp pushed back on-screen — turning corner arcs into full circles and causing partial-damage scissors to slice them in half.
2. **Redundant exported symbols and overlapping API variants**: `liboptics_gfx` exported both `optics_gfx_path_add_round_rect` and `optics_gfx_path_add_squircle` alongside five separate SDF symbols (`optics_gfx_sdf_round_rect`, `optics_gfx_sdf_squircle`, `optics_gfx_sdf_squircle_gradient`, `optics_gfx_sdf_squircle_sample`) even though a circular-arc rounded rectangle is simply a squircle with `curvature == 0` and uniform radii are simply `optics_gfx_corner_radii_uniform(r)`.
3. **Analytic SDF limited to uniform radii**: Restricting the analytic SDF shader (`canvas_sdf.frag.slang`) and host oracle (`sdf.c`) to a single scalar radius would force asymmetric per-corner UI elements onto path flattening and compute tile rasterisation.

## Decision

We execute a clean break across `optics-gfx`, `optics-ui`, their Slang shaders, and their Rust bindings with zero legacy compatibility shims or redundant wrapper aliases:

### 1. First-Class Per-Corner Radii Value Type (`optics_gfx_corner_radii`)

`<optics/gfx/math.h>` defines `optics_gfx_corner_radii { float tl, tr, br, bl; }` in clockwise CSS/Skia order with inline constructors `optics_gfx_corner_radii_uniform(r)` and `optics_gfx_corner_radii_all(tl, tr, br, bl)` and a single canonical normaliser `optics_gfx_corner_radii_normalize(radii, width, height)`. Radii are proportionally normalised per CSS Backgrounds & Borders Level 3 (`s = min(1, w/(tl+tr), w/(bl+br), h/(tl+bl), h/(tr+br))`, with overflow protection for large pill sentinels such as `FLT_MAX` and NaN/Inf clamping), permitting any single corner to claim up to `min(w, h)` (e.g. a quarter-arc `[0, 0, r, 0]` in an `r × r` box, a bezel-flush tab `[0, 0, h, h]`, or a diagonal leaf `[r, 0, r, 0]`).

### 2. Unified Analytic Per-Corner RRect SDF & Derivatives (`[INV-SDF-02]`, `[INV-SDF-06]`)

Both `<optics/gfx/sdf.h>` (`src/sdf/sdf.c`) and `<optics/gfx/shaders/optics_gfx_sdf.slang>` retire the uniform-only `optics_gfx_sdf_round_rect` and `optics_gfx_sdf_squircle*` symbols and expose ONE orthogonal per-corner family from one shared body:
- `optics_gfx_sdf_rrect(p, half_w, half_h, radii, curvature)`
- `optics_gfx_sdf_rrect_gradient(p, half_w, half_h, radii, curvature)`
- `optics_gfx_sdf_rrect_sample(p, half_w, half_h, radii, curvature, aa)`

Evaluation starts from the exact axis-aligned box SDF `d_box(p, he)` (with interior crease resolution over `aa` in `_sample`), then folds every corner `i` with `r_i > 0` and local outward coordinates `q_i.x > 0 && q_i.y > 0` via `d = max(d, d_arc(q_i, r_i, curvature))` (with matching interior crease blending in `_sample`).

`optics_gfx_canvas_push` defines a dedicated `sdf` view inside its existing 160-byte push block (`inv` at offset 32, `radii` at offset 64, `box` at offset 128, `{stroke_hw, curvature, aa}` at offset 144), so both Vulkan (`canvas_sdf.frag.slang`) and CPU (`backend_cpu.c`) evaluate every uniform and asymmetric rounded rect / squircle through the single analytic SDF pipeline with zero extra pipelines, zero push-constant growth, and zero path flattening. `test_sdf_parity` pins GPU ↔ CPU parity across uniform and per-corner shapes (including quarter-arc, bezel tab, asymmetric 4-corner, and diagonal leaf).

### 3. Unified `OPTICS_GFX_GEOM_RRECT`, Image Clip, and Single Path / Geometry Constructors

- `OPTICS_GFX_GEOM_SQUIRCLE` is retired. `OPTICS_GFX_GEOM_RRECT` carries `{ optics_gfx_rect rect; optics_gfx_corner_radii radii; float curvature; }`, keeping `sizeof(optics_gfx_geometry) <= 64` (one 64-byte cache line).
- `optics_gfx_brush_image_data` replaces `float clip_radius` with `optics_gfx_corner_radii clip_radii` + `float clip_curvature` (zero `sizeof(optics_gfx_brush)` growth), validated alongside `clip_rect` in `canvas_valid_brush`.
- Display list wire format bumps `DL_VERSION` from `1` to `2` (ADR-0157), serialising `OPTICS_GFX_GEOM_RRECT` as `(rect, radii, curvature)` and image brush clips as `(clip_rect, clip_radii, clip_curvature)`.
- `liboptics_gfx` exports one canonical path builder `optics_gfx_path_add_rrect(p, r, radii, curvature)`, and `<optics/gfx/canvas_helpers.h>` provides one canonical geometry constructor `optics_gfx_geom_rrect(r, radii, curvature)` and updated `optics_gfx_canvas_*rrect*` helpers with no redundant `round_rect`/`squircle`/`_radii` aliases.

### 4. Single-Representation Corner Radii in `optics-ui` and Rust Bindings

- `float radius` is retired from `optics_ui_layout_opts`, `optics_ui_grid_opts`, `optics_ui_skin_rect_opts`, `optics_ui_skin_border_opts`, and `optics_ui_draw_cmd`, replaced everywhere by `optics_gfx_corner_radii corner_radii` + `float corner_curvature`.
- `replay.c` removes the `<= 0.5f` magic threshold and emits `optics_gfx_geom_rrect` directly for any non-zero corner radius, eliminating temporary arena path allocation during replay.
- `drawlist.c` hashes finite floats via canonical IEEE-754 bit patterns (`hash_f32`), eliminating `float` → `uint32_t` cast overflow UB for large pill sentinels.
- Rust `optics-gfx` owns the canonical `CornerRadii` value type (with `From<f32>`, `From<[f32; 4]>`, `ZERO`, `new`, `uniform`, `top`, `bottom`, `left`, `right`, `max_radius`, `is_zero`, `normalize`) and exposes one `Path::add_rrect` plus updated `CanvasCommands::{fill_rrect, stroke_rrect, draw_image_rrect, draw_image_clipped_rrect_with_style}`.
- Rust `optics-ui` re-exports `optics_gfx::CornerRadii`, replaces `LayoutOpts.radius` with `corner_radii: CornerRadii` + `corner_curvature: f32`, and removes redundant builder/frame aliases (`radius`, `rounded`, `center`, `w`/`h`/`min_w`/`max_w`/`min_h`/`max_h`, `col`, `row_ex`, `col_ex`, `column_ex`, `StackBuilder::show_flat`).

## Rejected Alternatives & Negative Knowledge

### Quadrant-Selection SDF (Inigo Quilez `r.xy = p.x > 0 ? ...`)
- **Why considered**: Fewer instructions in the fragment shader.
- **Why rejected**: Mathematically broken whenever any corner radius exceeds `min(hx, hy)` (half the box width or height), because the arc crosses the box midlines into adjacent quadrants and suffers hard discontinuous steps. Quarter-circles `[0, 0, r, 0]` and full-height tabs `[0, 0, h, h]` both violate `r <= min(hx, hy)`.

### Keeping Uniform-Only `optics_gfx_sdf_squircle` / `optics_gfx_sdf_round_rect` Alongside a `_radii` Variant
- **Why considered**: Preserved existing uniform call sites without updating tests.
- **Why rejected**: Duplicated the L4 corner mathematics in both C and Slang, left `_gradient`/`_sample` uniform-only, and violated both `[INV-SDF-02]` (single definition) and the repo's no-variant-suffix API rule.

### Dual-Track `float radius` + `bool has_corner_radii` Compatibility Bolt-On
- **Why considered**: Avoids updating existing `LayoutOpts { radius: ... }` call sites.
- **Why rejected**: Creates two competing sources of truth on every descriptor and draw command, leaves `grid_opts` and `skin_*_opts` asymmetric, and encodes precedence ambiguity into the ABI.

### Path Fallback for Asymmetric Corners in `replay.c`
- **Why considered**: Kept `sizeof(optics_gfx_geometry) <= 48`.
- **Why rejected**: Forced every asymmetric UI surface through CPU path construction on `ui->arena` (adding arena-overflow hazards) and GPU compute tile rasterisation instead of the single analytic SDF fragment pass.

## Consequences

- Uniform and asymmetric rounded rectangles and G2 squircles share one exact analytic SDF and derivative family on CPU and Vulkan GPU, verified by `test_sdf`, `test_sdf_parity` (GPU ↔ CPU across 10 shapes including quarter-arc, bezel tab, 4-corner asymmetric, and leaf), `test_canvas_squircle` (zero wrong-side pixels vs. fitted two-cubic paths), and `test_canvas_consistency` (GPU ↔ CPU pixel parity).
- `optics_gfx_geometry` size ceiling updates from 48 B to 64 B (one cache line) and display list `DL_VERSION` bumps from 1 to 2.
- All C, Slang, and Rust surfaces share one orthogonal `CornerRadii` / `rrect` representation with zero compatibility scaffolding.
