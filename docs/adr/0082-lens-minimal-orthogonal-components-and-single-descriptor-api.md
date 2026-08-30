# ADR-0082: Lens minimal orthogonal components and single descriptor API

- Status: Accepted
- Date: 2026-08-26
- Scope: `lens` (C core library), `lens-rs` (Rust binding layer). Extends ADR-0024, ADR-0028, ADR-0031, ADR-0059, ADR-0060. Supersedes ADR-0039, ADR-0040, ADR-0041, ADR-0042.

## Context

Over successive development iterations, `liblens` accumulated specialized composite widgets and fragmented API tiers:

1. **API Tier Duality and Suffix Proliferation**: Most widgets maintained two distinct parallel entry points: terse forms (e.g. `lens_button`) and descriptor forms (e.g. `lens_button_ex`), alongside parameter/style variants (`lens_label_wrapped`, `lens_button_primary`, `lens_button_subtle`, `lens_button_mouse`, `lens_slider_vertical`, `lens_image_tinted`, `lens_icon_button_active`). This doubled the library symbol surface, required frequent caller refactoring when adding flags, and produced inconsistent return types (`bool` vs `lens_response`).
2. **Compound Widget Entanglement in the Micro-Kernel**: High-level compound widgets (`table`, `tabs`, `menu`, `menubar`, `dropdown`, `collapsing`, `tree`, `split`, `modal`) were hardcoded inside `liblens` core. These introduced heavy state machines, specialized skin payloads (`lens_tab_item`, `lens_grid_row`, `lens_grid_column`), and duplicated layout logic that fundamentally belong in userland UI kit compositions.
3. **Semantic Duplication**: `switch` and `radio` duplicated the boolean toggle interaction model and accessibility roles of `checkbox`, differing purely in visual presentation.

## Decision

Execute an uncompromising, non-backward-compatible overhaul to establish a **Minimal Orthogonal Component Architecture** governed by a strict **Four-Layer System Model** and the **Single Opts Descriptor Paradigm**:

### 1. Four-Layer Clean System Architecture

`liblens` is organized into four strictly orthogonal, non-overlapping architectural layers:

1. **Layer 1: Overlay & Z-Bands (`lens_place`)**  
   The sole spatial escape mechanism for floating chrome, popups, tooltips, dialogs, and scrims. Manages Z-band rendering order, click-outside / Escape dismissal, and auto-flipping anchored placement.
2. **Layer 2: Viewport & Camera (`lens_scroll`)**  
   The sole viewport translation and GPU scissor clipping system. Manages scroll offsets, wheel/touch routing, and scrollbar handle tracking. Completely decoupled from layout flow.
3. **Layer 3: Geometric Layout Flow (`lens_row`, `lens_column`, `lens_grid`)**  
   The flexbox and multi-column grid solvers that position child elements in 2D space.
4. **Layer 4: Atomic Interactive & Display Primitives**  
   The irreducible set of data-bound widgets.

### 2. Single Opts Descriptor Paradigm

Every atomic widget exports **exactly one** canonical C entry point accepting a typed descriptor struct (`const lens_<widget>_opts *opts`) and returning a complete `lens_response`:

```c
lens_response lens_label(lens *ui, const lens_label_opts *opts);
lens_response lens_icon(lens *ui, const lens_icon_opts *opts);
lens_response lens_image(lens *ui, const lens_image_opts *opts);
lens_response lens_separator(lens *ui, const lens_separator_opts *opts);
lens_response lens_button(lens *ui, const lens_button_opts *opts);
lens_response lens_checkbox(lens *ui, const lens_checkbox_opts *opts);
lens_response lens_selectable(lens *ui, const lens_selectable_opts *opts);
lens_response lens_slider(lens *ui, const lens_slider_opts *opts);
lens_response lens_textfield(lens *ui, const lens_textfield_opts *opts);
lens_response lens_textarea(lens *ui, const lens_textarea_opts *opts);
```

Callers leverage C99 compound literals for single-line ergonomics without sacrificing extensibility:

```c
// Terse single-line invocation:
if (lens_button(ui, &(lens_button_opts){.label = "Save"}).clicked) { ... }

// Extensible in-place configuration without symbol switching:
lens_response r = lens_button(ui, &(lens_button_opts){
    .label = "Delete",
    .variant = LENS_BUTTON_PRIMARY,
    .box = {.disabled = !dirty, .tooltip = "Cannot be undone"}
});
```

### 3. Canonical Widget Primitives & Variant Consolidation

* **`lens_button`**: Unifies plain buttons, primary accents (`LENS_BUTTON_PRIMARY`), subtle/ghost buttons (`LENS_BUTTON_SUBTLE`), text links (`LENS_BUTTON_LINK`), icon buttons, and image tiles.
* **`lens_checkbox`**: Unifies boolean toggles, switch toggles (`LENS_CHECKBOX_SWITCH`), and radio buttons (`LENS_CHECKBOX_RADIO`) via the `.appearance` property.
* **`lens_slider`**: Unifies horizontal and vertical scalar dragging via `.axis`.
* **`lens_label`**: Unifies plain text, headings, point sizes, explicit weights, and line-wrapping via `.size`, `.weight`, `.wrap`, and `box.max_width`.
* **Pruning**: Completely remove `table`, `tabs`, `menu`, `menubar`, `dropdown`, `collapsing`, `tree`, `split`, `modal`, `switch`, `radio`, and `link` from `lens_widget_kind`.

## Consequences

### Positive
- **Drastically Reduced API Surface**: Symbol count reduced by >60%, removing cognitive load and eliminating naming ambiguity across languages.
- **Zero Duplication**: Clear separation between Overlay (Place), Viewport (Scroll), Layout (Flex/Grid), and Widgets (Atoms).
- **Binding Friendly**: Simple, uniform C99 struct layouts map 1:1 into foreign function interfaces (Rust, Zig, Python) without wrapper glue.
- **Maintainability**: Reduced skin dispatch table from 22 kinds to 11 orthogonal kinds; eliminated >20 specialized C source files.

### Negative
- **Breaking Change**: Existing applications must update call sites to pass compound literal option pointers (`&(lens_<widget>_opts){...}`). No legacy compatibility wrappers are retained.
