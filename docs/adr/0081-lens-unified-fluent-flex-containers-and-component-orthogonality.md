# ADR-0081: Lens unified fluent flex containers and component orthogonality

- Status: Accepted
- Date: 2026-08-26
- Scope: `lens` (C core library), `lens-rs` (Rust binding layer). Extends ADR-0024, ADR-0028, and ADR-0031.

## Context

Previous revisions of the `lens` API separated container creation into scalar and descriptor tiers (`row` vs `row_ex`, `column` vs `column_ex`, `pressable_row` vs `pressable_column`). This design introduced several architectural frictions:

1. **Option Struct Ergonomic Penalty**: In Rust, lack of default arguments forced callers to construct verbose `LayoutOpts { ..Default::default() }` structs even for minor spacing or alignment adjustments.
2. **Interaction/Layout Dichotomy**: Clickable containers were treated as separate widget kinds (`pressable_row`) rather than an intrinsic capability of any layout box with an identity (`id`).
3. **Compound Pseudo-Widget Sprawl**: Specialized compound helpers (such as `setting_switch` and `scroll_column`) hardcoded ad-hoc child arrangement into the engine, violating the micro-kernel principle of orthogonal primitives.
4. **Historical Naming Artifacts**: Legacy terms like `collapsing` (derived from game-engine IMGUI conventions) diverged from modern desktop UI standards (`disclosure` / `expander`).

## Decision

Establish a unified, zero-allocation Fluent Builder architecture for all layout containers and enforce strict component orthogonality across C and Rust layers:

### 1. Unified Container Primitives (`f.row()` and `f.col()`)
All horizontal and vertical layout nodes in the Rust API are initiated via `f.row()` and `f.col()` (or `f.column()`), returning a stack-allocated, zero-heap `FlexBuilder<'_>` value type:

```rust
// Layout, visual styling, and interaction target unified in one chain:
let (response, ()) = f
    .row()
    .gap(8.0)
    .pad(4.0)
    .items_center()
    .bg(palette.material)
    .rounded(6.0)
    .id("nav_crumb")
    .show(|f| {
        f.label("Breadcrumb");
    });
```

- **Compile-Time Inlining**: Builder methods (`.gap()`, `.pad()`, `.items_center()`, `.bg()`, `.rounded()`, `.border()`, `.id()`) mutate stack values in place with zero heap allocations.
- **RAII Lifecycle**: The terminal methods (`.show()`, `.show_flat()`, `.empty()`) guarantee opening and closing of underlying C nodes (`open_flex` / `lens_close`), eliminating container stack leakage.

### 2. Elimination of `_ex` and `pressable_*` Dualities
- Remove `row_ex`, `column_ex`, `pressable_row`, and `pressable_column` from the public Rust API.
- Setting `.id("...")` on a `FlexBuilder` automatically routes the container through interaction detection and returns the resolved `Response` (`.clicked`, `.hovered`, `.pressed`).

### 3. Component Orthogonality and Pruning of Pseudo-Widgets
- **Prune Compound Helpers**: Remove `setting_switch` and `scroll_column` from the core library. Complex settings rows and scroll lists are composed naturally using `f.row()`, `f.col()`, and atomic widgets (`f.switch()`, `f.scroll()`).
- **Standardize on Modern Terminology**: Introduce `f.disclosure(...)` and `f.expander(...)` as the standard names for expandable headers, retaining `collapsing` only as a secondary compatibility alias.

### 4. C Core Alignment
- The C library (`libs/lens`) exports clean descriptor functions (`lens_row_opts`, `lens_col_opts`) accepting `lens_layout_opts`.
- C headers provide C23/C11 macro wrappers (`lens_row(ui, ...)`) supporting designated initializer syntax for optional parameters.

## Consequences

### Positive
- **Reduced Call-Site Boilerplate**: Downstream UI code (e.g. `xdg-desktop-portal-aegis`) achieves a 30–40% reduction in layout ceremony while improving readability.
- **Single Mental Model**: Developers learn one Flexbox container model with chainable modifiers rather than a matrix of specialized container functions.
- **Strict Separation of Concerns**: Core `lens` remains a micro-kernel of orthogonal primitives; domain layout patterns reside exclusively in caller application code.

### Negative
- **Breaking Change**: Existing call sites relying on `row_ex` or `LayoutOpts` structs must migrate to `.row().show(...)` or `.col().show(...)`.
