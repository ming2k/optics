# ADR-0083: Flux orthogonal shape-paint model and full C23 baseline

- Status: Accepted
- Date: 2026-08-26
- Scope: `flux` (2D/3D GPU rendering core), `flux-rs` (Rust bindings). Extends ADR-0004, ADR-0010, ADR-0025, ADR-0082.

## Context

`flux/canvas.h` evolved by adding specialized draw functions for each geometry type, stroke/fill variant, and texture optimization:
- `flux_canvas_fill_rect`, `flux_canvas_fill_rrect`, `flux_canvas_stroke_rrect`
- `flux_canvas_fill_circle`, `flux_canvas_stroke_circle`, `flux_canvas_fill_path`, `flux_canvas_stroke_path`
- `flux_canvas_draw_image`, `flux_canvas_draw_image_opaque`, `flux_canvas_draw_image_rrect`, `flux_canvas_draw_image_clipped_rrect`, `flux_canvas_draw_image_sub`, `flux_canvas_draw_image_sampled`
- `flux_canvas_fill_rect_color`, `flux_canvas_draw_glyph_run`

This fragmentation created several architectural liabilities:
1. **Geometric vs Material Coupling**: Function names entangled geometric shape (`Rect`, `RRect`, `Circle`, `Path`, `Image`), draw mode (`Fill`, `Stroke`), and material properties (`Color`, `Opaque`, `Sampled`).
2. **CPU Dispatch & Batching Overhead**: Disparate entry points hindered unified display-list recording and vertex batching across mixed draw commands.
3. **Legacy C Dialect Artifacts**: APIs used pre-C23 idioms (`NULL`, `{0}`, unconstrained enums, macro attributes) despite the project configuring `-std=c23`.

## Decision

1. **Full C23 Standard Baseline**:
   - The entire codebase strictly adopts ISO C23 (`-std=c23`):
     - Native `nullptr` replacing `NULL` or `0` for pointers.
     - Native empty braces `{}` for zero-initialization of compound literals.
     - Fixed-underlying-type enums (`enum name : uint32_t`) for guaranteed 32-bit ABI stability.
     - Standard attributes (`[[nodiscard]]`, `[[reproducible]]`, `[[unsequenced]]`).
     - Native C23 `static_assert` without requiring `<assert.h>`.

2. **Orthogonal Geometry $\times$ Material (`flux_shape` $\times$ `flux_paint`)**:
   Every 2D draw operation is unified into a single canonical entry point:
   ```c
   FLUX_API void flux_canvas_draw(flux_canvas *c, const flux_shape *shape, const flux_paint *paint);
   ```

3. **Unified Geometry Descriptor (`flux_shape`)**:
   ```c
   typedef enum flux_shape_kind : uint32_t {
       FLUX_SHAPE_RECT = 0,
       FLUX_SHAPE_RRECT = 1,
       FLUX_SHAPE_CIRCLE = 2,
       FLUX_SHAPE_LINE = 3,
       FLUX_SHAPE_PATH = 4,
       FLUX_SHAPE_IMAGE = 5,
       FLUX_SHAPE_GLYPHS = 6,
   } flux_shape_kind;

   typedef struct flux_shape {
       flux_shape_kind kind;
       flux_rect rect;
       float radius;
       float stroke_width; // 0 = fill, > 0 = stroke
       flux_line_cap stroke_cap;
       flux_line_join stroke_join;
       const flux_path *path;
       flux_image *image;
       flux_rect src_rect;
       flux_sampler *sampler;
       bool opaque_only;
       const flux_glyph_quad *glyphs;
       uint32_t glyph_count;
   } flux_shape;
   ```

4. **Excise Legacy Specializations**:
   Remove all 15+ legacy `fill_*`, `stroke_*`, `draw_image_*` entry points without backward-compatibility shims.

## Consequences

### Positive
- **Single Dispatch Pipeline**: Canvas internals process a uniform, sequential descriptor stream, maximizing GPU draw-call batching and DisplayList compaction.
- **Modern C23 Ergonomics**: Zero-cost, type-safe compound literal calls (`&(flux_shape){.kind = FLUX_SHAPE_RRECT, .rect = r, .radius = 8.0f}`).
- **Predictable FFI**: 1:1 struct mapping for Rust, Zig, and Python without wrapper impedance mismatch.

### Negative
- **Breaking Change**: All direct callers of legacy `flux_canvas_*` draw functions must migrate to `flux_canvas_draw`.
