# API Design Conventions & C23 Standards

This document establishes the mandatory conventions and invariants for all C and Rust APIs in Optics.

---

## 1. The Single Opts Descriptor Paradigm

Every public functional entry point across all libraries in Optics must follow this exact signature pattern:

```c
// Mandatory Signature Pattern:
<ReturnStruct> <module>_<verb/noun>(<Context> *ctx, const <module>_<noun>_opts *opts);
```

### Key Rules:
1. **Pointers to Descriptors**: The `opts` argument must always be a `const *` pointer to allow passing `{}` or `nullptr` safely.
2. **C23 Compound Literals**: Callers pass options using modern C23 compound literal syntax (`{}` for zero initialization, `nullptr` for null pointers):
   ```c
   // Terse, idiomatic C23 invocation (a real canonical widget call):
   lens_button(ui, &(lens_button_opts){
       .label = "Save",
       .variant = LENS_BUTTON_PRIMARY,
   });
   ```
3. **C23 Language Baseline**:
   - **Pointers**: Always use native `nullptr` (never `NULL` or `0`).
   - **Zero-initialization**: Always use `{}` (never `{0}`).
   - **Typed Enums**: Explicit fixed underlying types `typedef enum name : uint32_t { ... }`.
   - **Attributes**: Standard C23 `[[nodiscard]]`, `[[reproducible]]`, `[[unsequenced]]`.
   - **Static Assertions**: Standard C23 `static_assert(cond, msg)`.
4. **Structured Return**:
   - Fallible public entry points return `flux_result` (the stack-wide status type) or a typed response descriptor; query/accessor entry points may return a value directly. Bare `int` error codes and unchecked out-parameters are prohibited.

### Migration Status

Waves 1–6 (`glyph`, `vista`, `prism`, `iris`, `flux`, `lens`) are all landed;
`reticle` was already compliant. Every operation entry point across the stack
now accepts a typed `const <module>_<op>_opts *` descriptor (or its existing
`*_desc *` descriptor), with folded variants (`glyph_draw_outlined`,
`flux_scene_draw_mesh_{lit,skinned,skinned_lit}`,
`flux_image_update_region_{strided,premultiply}`,
`flux_encoder_draw_geometry_slot`, `flux_canvas_submit_display_list_bound`)
unified into their canonical descriptor fields. Math, pure getters/single-scalar
setters, lifecycle primitives, version accessors, callbacks, and raw Vulkan
handle/readback accessors remain explicit exceptions per
([ADR-0165](../../adr/0165-single-opts-descriptor-migration-by-library.md)).
`tools/check_api.py` enforces the suffix ratchet against regressions.

---

## 2. Prohibited API Anti-Patterns

| Anti-Pattern | Description | Correct Alternative |
| :--- | :--- | :--- |
| **`_ex` Suffix** | Creating a terse scalar version and an `_ex` descriptor version. | **Single canonical descriptor function.** |
| **`_wrapped` / `_sized` Suffix** | Adding functions based on parameter combinations. | **Control via fields in the `_opts` struct.** |
| **`_primary` / `_subtle` Suffix** | Encoding visual hierarchy or operational flavors into the function symbol. | **Pass `.variant` or mode flag in `_opts`.** |
| **`_vertical` Suffix** | Encoding layout or processing orientation into the function symbol. | **Pass `.axis` or orientation enum in `_opts`.** |
| **Floating State Modifiers** | Functions modifying the "next" or "previous" item implicitly (e.g. `next_disabled(true)`). | **Explicitly pass configuration fields in that item's `_opts`.** |
| **Callback Data Pulling** | Forcing intrusive callbacks for basic data iteration. | **Direct loops in the caller emitting standard descriptors.** |

---

## 3. Memory & Lifetime Contracts

1. **Arena & Transient Allocation Borrowing**:
   - Strings, geometry vertices, and dynamic arrays passed to C entry points are copied or borrowed into transient/frame arenas during execution.
   - Callers are free to pass stack-allocated, reused, or scratch buffers without lifetime concerns across frame boundaries.
2. **Zero Heap Allocation in Hot Paths**:
   - No hot path, frame loop, layout pass, or draw command emission may invoke libc `malloc`/`calloc`/`realloc`/`free`.
3. **Explicit State Ownership**:
   - Multi-frame or persistent state must be explicitly managed through caller-held state objects, retained stores, or stable context handles rather than hidden module-static variables.
