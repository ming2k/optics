# UI Component Governance & Intake Specification

This document details the governance criteria, state machine invariants, and review standards for UI components in `liblens` and `bindings/lens-rs`.

---

## 1. Domain Boundary

`liblens` is an immediate-mode UI façade over a retained-mode layout, animation, and reconciliation core.

- **liblens Core OWNS**:
  - Stable 64-bit ID generation and retained store reconciliation (`store.c`, `id.c`).
  - Strict 4-layer spatial systems (`place.c`, `scroll.c`, `tree.c`, `solve.c`).
  - Atomic widgets with independent data models and interaction state machines.
  - Pluggable context-wide skin dispatch and draw-list generation (`skin.c`, `drawlist.c`).
  - Assistive technology semantic tree production (`semantics.c`).
- **liblens Core FORBIDS**:
  - Multi-widget compound compositions (e.g. data tables, tab bars, dropdown combos, accordions, dialog helpers).
  - Business logic, form validators, or application-specific controllers.
  - OS window management or event loop ownership (owned by `iris`).

---

## 2. The 9 Canonical Orthogonal Primitives

| Component | Kind (`lens_widget_kind`) | Interaction Model | Representation |
| :--- | :--- | :--- | :--- |
| **`lens_label`** | `LENS_WIDGET_LABEL` | 0D Static Read-Only | Typography, Headings, Text-wrapping, Sizing |
| **`lens_icon`** | `LENS_WIDGET_ICON` | 0D Static Vector | SVG Vector Glyphs |
| **`lens_image`** | `LENS_WIDGET_IMAGE` | 0D Static Raster | Bitmaps, Textures, Modulation Tint |
| **`lens_separator`** | `LENS_WIDGET_SEPARATOR`| 0D Static Geometry | Horizontal / Vertical Divider Lines |
| **`lens_button`** | `LENS_WIDGET_BUTTON` | 0D Transient Pulse / Click | Default, Primary, Subtle, Link; Icon/Image content |
| **`lens_checkbox`** | `LENS_WIDGET_CHECKBOX` | 0D Discrete Boolean Toggle | Box (checkbox), Switch (capsule), Radio (circle) |
| **`lens_selectable`**| `LENS_WIDGET_SELECTABLE`| 0D Selected / Active State | List items, Nav rows, Tab items, Tree nodes |
| **`lens_slider`** | `LENS_WIDGET_SLIDER` | 1D Continuous/Discrete Scalar | Horizontal / Vertical dragging, Wheel, Keyboard |
| **`lens_textedit`** | `LENS_WIDGET_TEXTEDIT` | 1D/2D Character Buffer | Single-line field & Multi-line editor, IME, Selection |

---

## 3. Review Gate for New Component Admission

Any proposal for a new component must answer:
1. **Can it be composed?**
   - *Example*: "Can we add `lens_table`?" ➔ **REJECT**. Compose `lens_scroll` + `lens_row` loop + `lens_label`/`lens_button`.
   - *Example*: "Can we add `lens_modal`?" ➔ **REJECT**. Compose `lens_place(BACKDROP)` + `lens_place(CENTERED)`.
   - *Example*: "Can we add `lens_tabs`?" ➔ **REJECT**. Compose `lens_row` + `lens_selectable` items.
2. **Does it represent an unrepresented data type?**
   - *Example*: "Can we add a color picker / 2D area dragging primitive?" ➔ **CONSIDER** (represents a 2D scalar interaction $[x, y] \in [0,1]^2$).
3. **Does it violate Single Opts Descriptor?**
   - Must expose only `lens_response lens_<name>(lens *ui, const lens_<name>_opts *opts);`.
