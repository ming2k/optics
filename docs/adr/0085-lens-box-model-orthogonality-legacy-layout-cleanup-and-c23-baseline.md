# ADR-0085: Lens box model orthogonality, legacy layout cleanup, and C23 baseline

- Status: Accepted
- Date: 2026-08-26
- Scope: `lens` (C core UI library), `lens-rs` (Rust binding layer). Extends ADR-0028, ADR-0061, ADR-0081, ADR-0082, ADR-0083.

## Context

Following the consolidation of containers (ADR-0081) and minimal orthogonal single-descriptor components (ADR-0082), `liblens` establishes `lens_box` as the unified base model for all layout, geometry, identity, and per-call styling.

However, several architectural debts and transitional redundancies remained:

1. **`lens_layout_opts` Redundant Fields**: `lens_layout_opts` embedded `lens_box box` while continuing to declare historical top-level `min_width`, `max_width`, `min_height`, and `max_height` fields. This introduced dual configuration paths, ambiguity in layout resolution, and redundant branching in container tree building.
2. **Box Model Semantics**: The naming and boundaries of `lens_box` were evaluated against alternative naming schemes (`lens_base`, `lens_node_opts`, `lens_props`). `lens_box` is reaffirmed as the canonical 2D layout box and base descriptor in the core engine.
3. **C23 Baseline and Compile-Time Invariants**: Immediate-mode component descriptors rely on subtyping by memory layout (`offsetof(opts, box) == 0`). Without compile-time static assertions, layout structural regressions could occur silently. Furthermore, modern C23 attributes (`[[nodiscard]]`), zero-initialization semantics, and strict types were not fully enforced.

## Decision

### 1. Legacy Field Elimination in `lens_layout_opts`

Remove all redundant geometry fields from `lens_layout_opts`. All dimension constraints (`width`, `height`, `min_width`, `max_width`, `min_height`, `max_height`, `flex`), stable identity (`id`), state flags (`disabled`, `error`), and per-call style overrides (`style`) are exclusively sourced from `opts.box`:

```c
typedef struct lens_layout_opts {
    lens_box box;       /* sole source of geometry, constraints, identity, style */
    float gap;          /* main-axis inter-child spacing */
    float pad;          /* uniform container padding */
    lens_align align;   /* main-axis distribution */
    lens_align cross;   /* cross-axis alignment */
    flux_color bg;      /* container surface background fill */
    float radius;       /* container corner radius */
    flux_color border;  /* container border stroke */
    float border_width; /* container border thickness */
} lens_layout_opts;
```

Internal layout solvers and container entry points (`lens_row_begin`, `lens_column_begin`, `open_flex`) resolve geometry purely through `lensi_apply_box` and `opts.box`.

### 2. Retention of `lens_box` Model

1. **C Core**: Retain `lens_box` as the standard first member of every widget descriptor struct.
2. **Subtyping Contract**: Every `lens_*_opts` descriptor MUST place `lens_box box;` as its first field at `offset 0`.
3. **Rust Binding (`lens-rs`)**: The `BoxProps` trait encapsulates all `lens_box` manipulation across all component builders and container builders, guaranteeing full ergonomics and type safety.

### 3. C23 Baseline and Invariants

1. **Static Assertions on Struct Subtyping**:
   Enforce compile-time validation that all descriptor options have `lens_box box` at offset 0:
   ```c
   static_assert(offsetof(lens_layout_opts, box) == 0, "lens_layout_opts.box must be at offset 0");
   static_assert(offsetof(lens_grid_opts, box) == 0, "lens_grid_opts.box must be at offset 0");
   static_assert(offsetof(lens_scroll_opts, box) == 0, "lens_scroll_opts.box must be at offset 0");
   static_assert(offsetof(lens_pressable_opts, box) == 0, "lens_pressable_opts.box must be at offset 0");
   static_assert(offsetof(lens_place_opts, box) == 0, "lens_place_opts.box must be at offset 0");
   static_assert(offsetof(lens_label_opts, box) == 0, "lens_label_opts.box must be at offset 0");
   static_assert(offsetof(lens_icon_opts, box) == 0, "lens_icon_opts.box must be at offset 0");
   static_assert(offsetof(lens_image_opts, box) == 0, "lens_image_opts.box must be at offset 0");
   static_assert(offsetof(lens_separator_opts, box) == 0, "lens_separator_opts.box must be at offset 0");
   static_assert(offsetof(lens_button_opts, box) == 0, "lens_button_opts.box must be at offset 0");
   static_assert(offsetof(lens_checkbox_opts, box) == 0, "lens_checkbox_opts.box must be at offset 0");
   static_assert(offsetof(lens_selectable_opts, box) == 0, "lens_selectable_opts.box must be at offset 0");
   static_assert(offsetof(lens_slider_opts, box) == 0, "lens_slider_opts.box must be at offset 0");
   static_assert(offsetof(lens_textedit_opts, box) == 0, "lens_textedit_opts.box must be at offset 0");
   ```
2. **`[[nodiscard]]` on Responses and Life Cycles**:
   Interactive functions returning `lens_response`, `bool`, or `lens_id` are annotated with `[[nodiscard]]` when compiled under C23 (or standard compiler attribute fallbacks).
3. **C23 Zero-Initialization Contract**:
   All options structs guarantee `{}` (all-zero memory) represents valid, deterministic default behavior.

## Consequences

- **Clarity and Orthogonality**: Container dimension options are no longer split across two levels.
- **Robustness**: Compile-time assertion guarantees zero runtime overhead with guaranteed structural memory compatibility.
- **Uncompromised Ergonomics**: Rust and C callers have identical mental models for geometry and styling constraints.
