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
   // Terse, idiomatic C23 invocation:
   flux_canvas_draw_rect(canvas, &(flux_canvas_draw_rect_opts){
       .bounds = {0.0f, 0.0f, 100.0f, 100.0f},
       .color = {1.0f, 1.0f, 1.0f, 1.0f},
   });
   ```
3. **C23 Language Baseline**:
   - **Pointers**: Always use native `nullptr` (never `NULL` or `0`).
   - **Zero-initialization**: Always use `{}` (never `{0}`).
   - **Typed Enums**: Explicit fixed underlying types `typedef enum name : uint32_t { ... }`.
   - **Attributes**: Standard C23 `[[nodiscard]]`, `[[reproducible]]`, `[[unsequenced]]`.
   - **Static Assertions**: Standard C23 `static_assert(cond, msg)`.
4. **Structured Return**:
   - Public entry points return explicit structured types (e.g. status structs, response handles, or result descriptors) rather than bare error codes or out-parameters, providing clear contextual outcome state.

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
