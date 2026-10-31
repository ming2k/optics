# UI Component Governance & Intake Specification

This document details the governance criteria, state machine invariants, and review standards for UI components in `libs/lens` and `bindings/lens-rs`.

---

## 1. Domain Boundary

`liblens` is an immediate-mode UI façade over a retained-mode layout, animation, and reconciliation core.

- **`liblens` Core OWNS**:
  - Stable 64-bit ID generation and retained store reconciliation (`store.c`, `id.c`).
  - Strict 4-layer spatial systems (`place.c`, `scroll.c`, `tree.c`, `solve.c`).
  - Atomic widgets with independent data models and interaction state machines.
  - Pluggable context-wide skin dispatch and draw-list generation (`skin.c`, `drawlist.c`).
  - Assistive technology semantic tree production (`semantics.c`).
- **`liblens` Core FORBIDS**:
  - Multi-widget compound compositions (e.g. data tables, tab bars, dropdown combos, accordions, dialog helpers). These belong to userland component kits.
  - Business logic, form validators, or application-specific controllers.
  - Direct OS window management or raw platform event handling (owned strictly by `iris`).

---

## 2. The 4-Layer Spatial Model

All spatial operations in `liblens` map into exactly one of four orthogonal layers:

```text
┌─────────────────────────────────────────────────────────┐
│ Layer 4: Overlay & Escape (`Place`)                     │
│          Tooltips, context menus, popovers, modals      │
├─────────────────────────────────────────────────────────┤
│ Layer 3: Viewport & Virtualization (`Scroll`)           │
│          Scroll offsets, clipping rects, culling        │
├─────────────────────────────────────────────────────────┤
│ Layer 2: Geometric Layout Flow (`Tree` / `Solve`)       │
│          Flexbox, CSS Grid, Flow wrap, absolute offsets │
├─────────────────────────────────────────────────────────┤
│ Layer 1: Data Interaction Atoms (`Widgets`)             │
│          Buttons, sliders, toggles, text edits, labels  │
└─────────────────────────────────────────────────────────┘
```

A component must never bridge or blur these layers (e.g. a Button must not internally compute viewport scroll offsets or spawn unbounded overlays).

---

## 3. Interaction Primitives Taxonomy

New components proposed for `liblens` core must represent a distinct mathematical interaction type:

| Dimension | Primitive | Data Type | Key States |
| :--- | :--- | :--- | :--- |
| **0D Pulse** | `lens_button` | Event / Trigger | Idle, Hover, Active, Disabled |
| **0D Boolean** | `lens_checkbox`, `lens_toggle` | `bool` | Checked, Unchecked, Indeterminate |
| **1D Scalar** | `lens_slider`, `lens_progress` | `float` / `double` | Normalized range `[min, max]`, step |
| **1D/2D Text** | `lens_textedit`, `lens_label` | String / Buffer | Caret position, selection range, scroll |

If a proposed widget does not introduce an unrepresented fundamental data model, it must be rejected from core or modeled via an existing primitive's `.variant` or `.appearance`.

---

## 4. Retained Store & State Invariants

1. **Deterministic 64-bit IDs**:
   - Every interactive node possesses a stable 64-bit ID hashed from parent context, label/seed, and explicit disambiguators (`lens_id_seed`).
   - Sibling collisions trigger immediate assertion warnings in debug builds.
2. **State Isolation**:
   - Multi-frame states (selection ranges, scroll positions, animations) are allocated from the retained store via `lens_node_state`.
   - Widgets must never retain pointers to external application state across frame boundaries.
3. **Semantic Accessibility (A11y)**:
   - Every core interactive primitive must produce corresponding accessibility roles and actions in `lens_semantics` for AT-SPI / UIA export.
