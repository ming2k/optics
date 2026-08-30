# Optics / Lens Design Philosophy

This document articulates the foundational architectural principles and design philosophy of the Optics graphics and UI stack (`flux` ➔ `lens` ➔ `iris`).

---

## 1. Core Principles

### I. Minimal Orthogonality (最小正交集合)
Every system layer and primitive must solve **one independent physical problem**.
- Primitives must never overlap in responsibility.
- Primitives must never combine multiple orthogonal concepts into a compound helper at the core engine level.
- Compound widgets (such as data tables, tab bars, dropdowns, accordions, and modal dialogs) belong to **userland component kits**, never the immediate-mode core micro-kernel.

### II. Single Opts Descriptor Paradigm (单一描述符范式)
Every widget exports **exactly one** canonical C entry point that accepts a single typed options struct pointer (`const lens_<widget>_opts *opts`) and returns a uniform `lens_response`:
```c
lens_response lens_<widget>(lens *ui, const lens_<widget>_opts *opts);
```
- **Zero Dual-API Tiers**: Eliminate the duality of terse vs extended (`_ex`) APIs.
- **Zero Parameter-Derived Suffixes**: Eliminate functions derived purely from argument variations (e.g. `_wrapped`, `_primary`, `_subtle`, `_vertical`, `_active`, `_tinted`).
- **Modern C23 Baseline**: Full standard C23 adoption (`-std=c23`). Native `nullptr`, `{}` zero-initialization, fixed underlying enums (`enum name : uint32_t`), `[[nodiscard]]`, and compound literals (`&(lens_<widget>_opts){...}`). No legacy pre-C23 shims.

### III. Composition Over Specialization (组合优于特化)
- Variants in visual hierarchy (e.g. primary vs subtle vs link) or presentation mode (e.g. checkbox box vs switch vs radio) are expressed as plain data enums inside the option struct, not as separate functions or widget kinds.
- Higher-level widgets are composed by nesting primitives (e.g., Table = `Scroll` + `Row` loop + `Selectable`/`Button`/`Label`).

### IV. Strict Four-Layer Separation (四层物理隔离)
The GUI stack is separated into four strictly isolated layers:

```
┌─────────────────────────────────────────────────────────────┐
│ Layer 1: Overlay System (Place & Z-Bands)                   │
│   Escapes normal layout flow and parent clipping.           │
├─────────────────────────────────────────────────────────────┤
│ Layer 2: Viewport & Camera System (Scroll)                  │
│   GPU scissor clipping, translation matrix, wheel routing.  │
├─────────────────────────────────────────────────────────────┤
│ Layer 3: Layout Flow System (Row / Column / Grid)           │
│   2D geometric constraint solvers (Flexbox & Grid).         │
├─────────────────────────────────────────────────────────────┤
│ Layer 4: Data Atoms (Label, Icon, Image, Button, etc.)      │
│   Irreducible data-bound state machines and pixels.         │
└─────────────────────────────────────────────────────────────┘
```

---

## 2. The Layered System Model

### Layer 1: Overlay System (`lens_place`)
- **Single Responsibility**: Manage elements that escape normal layout flow and parent scissor clipping.
- **Physics**: Maintains Z-band ordering (`BACKDROP`, `CHROME`, `POPUP`, `MODAL`, `TOOLTIP`), automatic collision detection with display boundaries (auto-flip on edge collision), and transient lifecycle (Esc and click-outside dismissal).
- **Invariance**: `place` does not dictate how children are laid out or scrolled; it only manages spatial coordinates and overlay occlusion.

### Layer 2: Viewport & Camera (`lens_scroll`)
- **Single Responsibility**: Transform and clip rendered output when content extent exceeds available bounds.
- **Physics**: Applies GPU scissor clipping and `(-scroll_x, -scroll_y)` world matrix translation. Intercepts mouse wheel/touch drag and manages scrollbar thumb interaction.
- **Invariance**: `scroll` is not a layout system. It does not arrange children; it only provides a moving viewing window over a layout container.

### Layer 3: Layout Flow (`lens_row`, `lens_column`, `lens_grid`)
- **Single Responsibility**: Solve 2D box positioning $(x, y, w, h)$ for child nodes.
- **Physics**: Flexbox main-axis and cross-axis alignment (`gap`, `pad`, `flex`, `cross`, `radius`, `border`, `bg`).
- **Invariance**: Layout containers do not perform GPU viewport clipping and do not escape parent coordinates.

### Layer 4: Data & Interaction Atoms
The 9 canonical orthogonal widget primitives:
1. **`lens_label`**: Static read-only typography (headings, body, wrapped text).
2. **`lens_icon`**: Vector symbol rendering (SVG glyphs).
3. **`lens_image`**: Raster texture rendering.
4. **`lens_separator`**: 1D geometric dividing lines.
5. **`lens_button`**: 0D transient pulse/action trigger (Default, Primary, Subtle, Link, Icon/Image content).
6. **`lens_checkbox`**: 0D discrete boolean toggle (Box, Switch, Radio appearance).
7. **`lens_selectable`**: Row selection and highlight atom (for menus, lists, tabs, trees).
8. **`lens_slider`**: 1D scalar value dragging along an axis.
9. **`lens_textedit`**: 1D/2D string buffer editing (single-line inputs and multi-line editors).

---

## 3. The Uncompromising Quality Invariant

- **No Backward Compatibility Burden**: When an API or component is superseded by an orthogonal design, the old form is completely excised. No shims, deprecated aliases, or compatibility redirects are retained in the core library.
- **ABI & Performance Predictability**: Every widget state machine is retained and reconciled via stable 64-bit ID hashing; every frame produces deterministic draw command streams.
