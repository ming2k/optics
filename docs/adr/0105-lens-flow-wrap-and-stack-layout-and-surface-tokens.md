---
id: ADR-0105
title: "Lens Flow Wrap and Stack In-Tree Layout Subsystems and Semantic Surface Tokens"
status: accepted
date: 2026-09-27
scope: libs/lens, bindings/lens-rs
superseded_by: null
negative_knowledge: true
---

# ADR-0105: Lens Flow Wrap and Stack In-Tree Layout Subsystems and Semantic Surface Tokens

- Status: Accepted
- Date: 2026-09-27
- Scope: `libs/lens` (`lens.h`, `internal.h`, `flex.c`, `solve.c`, `tree.c`, `theme.c`, `context.c`), `bindings/lens-rs`.
- Depends on: [ADR-0028](0028-lens-flexbox-layout.md), [ADR-0032](0032-lens-theme-tokens.md), [ADR-0060](0060-lens-single-tree-placement-and-z-bands.md), [ADR-0081](0081-lens-unified-fluent-flex-containers-and-component-orthogonality.md), [ADR-0085](0085-lens-box-model-orthogonality-legacy-layout-cleanup-and-c23-baseline.md), [ADR-0104](0104-lens-unified-orthogonal-2d-grid-and-1d-flex-layout.md).

## Context

Following the establishment of orthogonal 1D Flexbox (ADR-0028, ADR-0081) and 2D Bento Grid (ADR-0104) layout models, and the transition to a minimalist borderless baseline (`border_width = 0.0f`), two critical architectural gaps remain in Optics L2 (`lens`):

1. **Flow / Wrap Multi-Line Packaging (Chip/Tag Clouds and Responsive Toolbars)**:
   Linear flex rows (`LENS_ROW`) enforce single-axis distribution. When a row contains a dynamic collection of variable-length items (e.g., tags, filter chips, badge lists, media thumbnails), items either overflow the container boundary or compress unnaturally. Applications require a multi-line Flow/Wrap layout that packs items sequentially along the main axis and automatically breaks into subsequent lines when exceeding available width, with configurable column and row gaps.

2. **In-Tree Stack / Z-Layering (Overlays, Badges, and Composite Cards)**:
   Composite widgets frequently require layering elements within the same coordinate frame (e.g. an online indicator badge on the top-right of an avatar, an image card with dark gradient scrim and centered play button). Currently, developers are forced to resort to `lens_place` (which detaches nodes into Z-bands designed for popups/dialogs) or manual negative offsets. A first-class, in-tree `Stack` container is required that sizes itself to enclose its children while arranging them within a shared bounding box.

3. **Semantic Surface Color Hierarchy**:
   In a minimalist design language where borders are zero by default, visual hierarchy relies on subtle surface elevation rather than 1px strokes. Relying on hover tints (`bg_hover`) as makeshift resting surfaces is fragile. First-class semantic design tokens (`color_surface`, `color_surface_sunken`, `color_surface_elevated`) must be formally established in `lens_theme` under forward-compatible ABI protection (ADR-0032).

## Decision

We introduce two first-class in-tree layout primitives — **Flow Wrap** and **Stack** — and expand `lens_theme` with semantic surface tokens, fully integrated across the C23 core and Rust bindings.

### 1. Semantic Surface Tokens in `lens_theme`

`lens_theme` gains three explicit, semantic surface tokens:
- `color_surface`: Default card, panel, and container resting surface.
- `color_surface_sunken`: Inset/sunken surfaces for text fields, segmented control tracks, and progress bars.
- `color_surface_elevated`: Elevated surfaces for tooltips, floating popups, and dropdown menus.

Theme normalization (`lensi_theme_normalize`) fills zeroed tokens from existing base palettes to maintain 100% backward compatibility for custom or legacy themes. Controls (`textedit`, `checkbox`, `segmented`, `tooltip`) consume these tokens.

### 2. Flow Wrap Layout Subsystem (`lens_wrap_begin` / `lens_wrap_end`)

1. **Option Descriptor & Invariants**:
   `lens_layout_opts` is extended with:
   ```c
   typedef struct lens_layout_opts {
       lens_box box;
       float gap;          /* inter-item column gap */
       float pad;          /* container padding */
       lens_align align;   /* line distribution */
       lens_cross cross;   /* cross-axis alignment within line */
       flux_color bg;
       float radius;
       flux_color border;
       float border_width;
       bool wrap;          /* multi-line wrapping enabled */
       float row_gap;      /* vertical spacing between wrapped lines (0 = gap) */
   } lens_layout_opts;
   ```
2. **Dedicated Entry Points**:
   ```c
   LENS_API void lens_wrap_begin(lens *ui, const lens_layout_opts *opts);
   LENS_API void lens_wrap_end(lens *ui);
   ```
3. **Solver Strategy**:
   In `solve.c` and `flex.c`:
   - **Measure Pass**: Calculates intrinsic widths and heights of all flow children.
   - **Arrange Pass**: Traverses children sequentially, accumulating line width. When `current_x + child.w > available_w` (and the current line is non-empty), a line break occurs: `current_y += line_height + row_gap`, `current_x = pad`.
   - Dynamic height expansion: Container resolves to the total height of all wrapped lines.

### 3. Stack Layout Subsystem (`lens_stack_begin` / `lens_stack_end`)

1. **In-Tree Layering**:
   ```c
   LENS_API void lens_stack_begin(lens *ui, const lens_layout_opts *opts);
   LENS_API void lens_stack_end(lens *ui);
   ```
2. **Solver Strategy**:
   - `n->is_stack = true`.
   - **Measure Pass**: Intrinsic width is `max(child.w) + 2*pad`; intrinsic height is `max(child.h) + 2*pad`. Fixed dimensions on `opts.box` take precedence.
   - **Arrange Pass**: Each child is positioned within the container's inner rectangle `{rect.x + pad, rect.y + pad, rect.w - 2*pad, rect.h - 2*pad}` respecting the child's `align` and `cross` alignment or default full-bleed fill.
   - Children render in DOM tree order (first child at bottom, last child on top).

### 4. Rust Binding Ergonomics (`lens-rs`)

- `Theme` exposes `.surface()`, `.surface_sunken()`, and `.surface_elevated()`.
- `Frame` exposes `.wrap()` and `.stack()` container builders:
  ```rust
  f.wrap().gap(8.0).show_flat(|f| {
      for tag in &tags {
          f.button(tag);
      }
  });

  f.stack().show_flat(|f| {
      f.image(avatar);
      f.badge("3");
  });
  ```

## Consequences

### Positive
- Completes the layout triad: **1D Linear Flex (Row/Column)**, **2D Bento Grid (Grid)**, **Multi-Line Flow (Wrap)**, and **In-Tree Layering (Stack)**.
- Tags, badges, and responsive toolbars require zero manual coordinate hacking.
- Clean semantic separation between canvas background (`color_bg`), resting surfaces (`color_surface`), sunken controls (`color_surface_sunken`), and floating popups (`color_surface_elevated`).
- Full backward compatibility guarded by `lens_theme.size` and `lensi_theme_normalize`.
