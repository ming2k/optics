# API Design Guidelines & Invariants

This document establishes the mandatory conventions and invariants for all C and Rust APIs in Optics.

---

## 1. The Single Opts Descriptor Paradigm

Every public functional entry point for building widgets, issuing draw commands, or configuring containers must follow this exact signature:

```c
// Mandatory Signature Pattern:
<ReturnStruct> <module>_<verb/noun>(<Context> *ctx, const <module>_<noun>_opts *opts);
```

### Key Rules:
1. **Pointers to Descriptors**: The `opts` argument must always be a `const *` pointer to allow passing `{}` or `nullptr` safely.
2. **C23 Compound Literals**: Callers pass options using modern C23 compound literal syntax (`{}` for zero initialization, `nullptr` for null pointers):
   ```c
   // Terse C23 invocation:
   if (lens_button(ui, &(lens_button_opts){.label = "Save"}).clicked) { ... }
   ```
3. **C23 Language Invariants**:
   - Pointers: Always use `nullptr` (never `NULL` or `0`).
   - Zero-initialization: Always use `{}` (never `{0}`).
   - Enums: Explicit fixed underlying types `typedef enum name : uint32_t { ... }`.
   - Attributes: Standard C23 `[[nodiscard]]`, `[[reproducible]]`, `[[unsequenced]]`.
   - Assertions: Standard C23 `static_assert(cond, msg)`.
4. **Struct Embedding**:
   - Widget options structs in `lens` must embed `lens_box box;` as the first field:
     ```c
     typedef struct lens_button_opts {
         lens_box box;
         const char *label;
         lens_button_variant variant;
         ...
     } lens_button_opts;
     ```
4. **Structured Return**:
   - Interactive widgets must return a comprehensive `lens_response` rather than a bare `bool`, exposing `.clicked`, `.hovered`, `.pressed`, `.focused`, `.changed`, `.rect`, and `.state`.

---

## 2. Prohibited API Anti-Patterns

| Anti-Pattern | Description | Correct Alternative |
| :--- | :--- | :--- |
| **`_ex` Suffix** | Creating a terse scalar version and an `_ex` descriptor version. | **Single canonical descriptor function.** |
| **`_wrapped` / `_sized` Suffix** | Adding functions based on parameter combinations. | **Control via `.wrap`, `.size` in the `_opts` struct.** |
| **`_primary` / `_subtle` Suffix** | Encoding visual hierarchy into the function symbol. | **Pass `.variant = LENS_BUTTON_PRIMARY` in `_opts`.** |
| **`_vertical` Suffix** | Encoding layout orientation into the function symbol. | **Pass `.axis = LENS_COLUMN` in `_opts`.** |
| **Floating State Modifiers** | Functions modifying the "next" or "previous" widget (e.g. `next_disabled(true)`). | **Explicitly pass `.box = {.disabled = true}` on that widget.** |
| **Callback Data Pulling** | Forcing callbacks like `cell_fn(row, col)` for core layout. | **Direct loops in the caller emitting standard widgets.** |

---

## 3. Memory & Lifetime Contracts

1. **Per-Frame Arena Borrowing**:
   - Strings and dynamic arrays passed to C entry points are copied into the per-frame arena (`ui->arena`) during the build phase.
   - Callers are free to pass stack-allocated, reused, or scratch buffers.
2. **Zero Heap Allocation during Frames**:
   - No widget, layout pass, or skin dispatch may invoke libc `malloc`/`calloc`/`free` during `lens_begin` ➔ `lens_end`.
3. **Retained Store State**:
   - Widget state spanning multiple frames (cursor position, scroll offsets, drag state) must be requested via `lens_node_state(n, sizeof(State))`, keyed by the node's stable 64-bit ID.
