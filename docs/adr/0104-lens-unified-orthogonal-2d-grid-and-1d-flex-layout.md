---
id: ADR-0104
title: "Lens Unified Orthogonal 2D Grid and 1D Flex Layout Subsystem"
status: accepted
date: 2026-09-18
scope: libs/lens, bindings/lens-rs
superseded_by: null
negative_knowledge: true
---

# ADR-0104: Lens Unified Orthogonal 2D Grid and 1D Flex Layout Subsystem

- Status: Accepted
- Date: 2026-09-18
- Scope: `libs/lens` (`lens.h`, `internal.h`, `solve.c`, `tree.c`, `node.c`), `bindings/lens-rs`.
- Depends on: [ADR-0028](0028-lens-flexbox-layout.md), [ADR-0060](0060-lens-single-tree-placement-and-z-bands.md), [ADR-0081](0081-lens-unified-fluent-flex-containers-and-component-orthogonality.md), [ADR-0085](0085-lens-box-model-orthogonality-legacy-layout-cleanup-and-c23-baseline.md).

## Context

Desktop and dashboard user interfaces routinely require two fundamentally complementary layout models:
1. **One-Dimensional Linear Flow (1D Flexbox)**: Sizing and distributing items sequentially along a single axis (horizontal `Row` or vertical `Column`), with secondary cross-axis alignment.
2. **Two-Dimensional Planar Topology (2D Grid)**: Partitioning a 2D surface into discrete column and row tracks, where cards or widgets can occupy specific planar coordinates and span multiple grid cells (e.g. Bento Grids, metric dashboards, split master-detail cards, settings tileboards).

Historically, `lens` established a robust two-pass Flexbox engine (ADR-0028, ADR-0081, ADR-0085) for 1D containers, but its preliminary grid support (`lens_grid_begin`) had severe architectural limitations:
1. **Uniform Single-Cell Flow Only**: Children were restricted to filling exactly one cell each in sequence. There was no concept of column spans (`col_span`), row spans (`row_span`), or explicit track placement (`grid_col`, `grid_row`).
2. **Inability to Form Bento / Dashboard Formats**: High-density layouts where cards vary in size (e.g., a 2×1 wide banner next to a 1×2 tall gauge and 1×1 tiles) could not be represented natively without fragile nested row/column hacks and hardcoded pixel sizes.
3. **Ambiguity Regarding Nesting and Isolation**: Questions arose regarding whether 1D and 2D layouts compete or conflict. In a clean micro-kernel UI architecture, 1D and 2D layout mechanisms must be completely orthogonal, composable, and nestable to arbitrary depths.

## Decision

We establish an uncompromised, long-term oriented layout architecture in `libs/lens` and `lens-rs` that treats 1D Flex and 2D Grid as fully orthogonal, composable peers under the unified `lens_box` model.

### 1. Unified 2D Placement in the Base `lens_box` Model

In accordance with ADR-0085 (Box Model Orthogonality), child layout hints are hosted directly on the universal `lens_box` struct:

```c
typedef struct lens_box {
    const char *id;
    float flex;
    float width;
    float height;
    float min_width;
    float min_height;
    float max_width;
    float max_height;
    int32_t col_span;    /* grid: columns spanned (default 1) */
    int32_t row_span;    /* grid: rows spanned (default 1) */
    int32_t grid_col;    /* grid: 1-based column coordinate (0 = auto-place) */
    int32_t grid_row;    /* grid: 1-based row coordinate (0 = auto-place) */
    bool disabled;
    bool error;
    const char *tooltip;
    lens_style style;
} lens_box;
```

This guarantees that *any* container or leaf widget (a `lens_button`, a `lens_column_begin` card, a nested `lens_grid_begin`) can seamlessly participate as a grid item simply by setting `.col_span`, `.row_span`, or `.grid_col`/`.grid_row`.

Immediate-mode procedural hints are also provided:
```c
LENS_API void lens_col_span(lens *ui, uint32_t span);
LENS_API void lens_row_span(lens *ui, uint32_t span);
LENS_API void lens_grid_at(lens *ui, int32_t col, int32_t row);
```

### 2. Orthogonal Composability and Nesting Boundary

1D Flex and 2D Grid are completely non-conflicting:
- **Child-Container Duality**: To its parent container, a `lens_node` is merely a box with measured intrinsic extents (`measured.x`, `measured.y`) and placement constraints. Inside its own boundary, the node governs its children with its own layout strategy.
- **1D Hosting 2D**: A `lens_column` can host a `lens_grid` as a child. The grid's measure pass computes total planar dimensions; the column arranges the grid along its vertical flow.
- **2D Hosting 1D**: A `lens_grid` cell can host a `lens_column` card. The grid solves the cell's 2D bounding rect (respecting multi-cell spans); the column then runs flex distribution on its own children within that assigned boundary.
- **Arbitrary Recursive Depth**: Grids may be nested inside Grids, Columns inside Rows inside Grids, with zero impedance mismatch.

### 3. Two-Pass Deterministic 2D Grid Layout Algorithm

`lensi_layout_solve` implements a deterministic, zero-allocation two-pass grid solver in `solve.c`:

1. **2D Track Placement Phase**:
   - Explicitly positioned items (`grid_col > 0 && grid_row > 0`) are resolved first and registered into a 2D occupancy matrix.
   - Flow items (auto-placed) are packed into the next available rectangular slot `[r .. r + span_r - 1][c .. c + span_c - 1]` using the standard 2D packing cursor.
   - Total rows `total_rows = max(resolved_row + resolved_row_span)` are determined.
2. **Measure Pass (Bottom-Up)**:
   - For 1-span items, intrinsic widths and heights are recorded into column and row track accumulators.
   - For multi-span items, any deficit beyond the sum of spanned tracks plus intermediate gaps is distributed evenly across the spanned tracks.
   - Total container intrinsic width and height are synthesized:
     $$\text{Width} = \sum \text{col\_w} + (\text{cols} - 1) \cdot \text{col\_gap} + 2 \cdot \text{pad}$$
     $$\text{Height} = \sum \text{row\_h} + (\text{rows} - 1) \cdot \text{row\_gap} + 2 \cdot \text{pad}$$
3. **Arrange Pass (Top-Down)**:
   - Available width divides into `cols` tracks: $\text{unit\_col\_w} = (\text{avail\_w} - (\text{cols} - 1) \cdot \text{col\_gap}) / \text{cols}$.
   - Row heights either respect fixed `row_height` or content-derived track heights.
   - Each child is positioned at its spanned cell rectangle:
     $$\text{rect.x} = \text{col\_x}[\text{col}], \quad \text{rect.y} = \text{row\_y}[\text{row}]$$
     $$\text{rect.w} = \text{span\_c} \cdot \text{unit\_col\_w} + (\text{span\_c} - 1) \cdot \text{col\_gap}$$
     $$\text{rect.h} = \sum_{i=0}^{\text{span\_r}-1} \text{row\_h}[\text{row} + i] + (\text{span\_r} - 1) \cdot \text{row\_gap}$$
   - Child constraints (`min_width`, `max_width`, `fit`) and cell alignments (`align`, `cross`) are applied before recursing into `arrange(c, child_rect)`.
   - ADR-0060 ABS children are resolved cleanly at the grid boundary.

### 4. Zero-Heap Allocation Contract

All temporary occupancy bitsets and track size vectors are allocated either on the stack (for typical $\le 32$ column, $\le 32$ row topologies) or bumped from the per-frame `ui->arena`. Zero dynamic heap allocations (`malloc`/`free`) occur during layout execution.

### 5. Idiomatic Rust Binding (`lens-rs`)

`lens-rs` exposes a fluent `GridBuilder` matching `FlexBuilder`:
```rust
f.grid(4)
    .col_gap(12.0)
    .row_gap(12.0)
    .pad(16.0)
    .show(|f| {
        // Wide hero card spanning 2 cols x 2 rows
        f.col()
            .col_span(2)
            .row_span(2)
            .bg(palette.card)
            .show(|f| {
                f.label("Hero Card");
            });
    });
```

## Consequences

### Positive
- **Complete Bento & Dashboard Capability**: Arbitrary 2D card layouts with multi-column and multi-row spans are supported natively.
- **Architectural Purity**: 1D Flexbox and 2D Grid are decoupled, orthogonal primitives that compose cleanly without special cases or legacy baggage.
- **Full Backward Compatibility**: Uniform grid callers (`lens_grid_opts` without spans) continue to work identically with zero behavioral deviation.
- **Performance**: Single measure + single arrange pass convergence, zero heap allocation, bounded arena usage.

### Negative
- `lens_box` size increases by 16 bytes (`col_span`, `row_span`, `grid_col`, `grid_row`), which is negligible and preserved on the stack/arena.
