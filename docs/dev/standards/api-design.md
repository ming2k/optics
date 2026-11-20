# API Design Conventions & C23 Standards

This document establishes the mandatory conventions and invariants for all C and Rust APIs in Optics.

---

## 1. The Descriptor Rule

A descriptor is for configuration. Operands are parameters.

1. **At most four parameters.** A public function that needs more than four
   takes one descriptor instead of growing its list:
   ```c
   <result> <module>_<verb>(<context> *ctx, const <module>_<noun>_opts *opts);
   ```
2. **At most one descriptor.** A function never takes two.
3. **No variant suffixes.** `_ex`, `_wrapped`, `_sized`, `_primary` and the
   like are forbidden. A variant is a field of the descriptor, not a second
   entry point.
4. **Creation and widget calls always take a descriptor**, even when they are
   small today: they are where configuration accumulates.
5. **Positional is correct for operands.** Scalar setters and getters
   (`flux_canvas_rotate(c, radians)`, `lens_set_scale(ui, scale)`), lifecycle
   primitives (`retain` / `release`), and value kernels — pure math and
   geometry in `<optics/gfx/math.h>`, `<optics/gfx/curve.h>`, `<optics/gfx/sdf.h>` and the path
   verbs — keep their arguments positional at any count. Wrapping six control
   point floats in a struct obscures the call without adding anything.

Callers pass descriptors as C23 compound literals; a zeroed field means "the
default":

```c
lens_button(ui, &(lens_button_opts){
    .label = "Save",
    .variant = LENS_BUTTON_PRIMARY,
});
```

Fallible entry points return `flux_result` (the stack-wide status type) or a
typed response; queries may return a value directly. Bare `int` error codes
and unchecked out-parameters are prohibited in new API.

### Enforcement

`tools/check_api.py` (`meson test --suite lint`) parses every public
declaration under `libs/*/include` and fails on a function with more than four
parameters outside the value-kernel headers, on a second descriptor, and on a
variant suffix. Functions that predate the rule are listed in
`tools/config/api_baseline.txt`. That file is a ratchet: a function not on it
fails the check, and an entry that no longer violates fails until it is
deleted. Converting a function means removing its line.

This rule replaces the earlier "every entry point takes exactly one descriptor"
wording ([ADR-0165](../../adr/0165-single-opts-descriptor-migration-by-library.md),
amended by [ADR-0166](../../adr/0166-one-way-library-graph-and-enforced-invariants.md)),
which the headers never satisfied and no tool checked.

### C23 Baseline

- **Pointers**: `nullptr` (never `NULL` or `0`).
- **Zero-initialization**: `{}` (never `{0}`).
- **Typed enums**: fixed underlying types, `typedef enum name : uint32_t { ... }`.
- **Attributes**: `[[nodiscard]]`, `[[reproducible]]`, `[[unsequenced]]`.
- **Static assertions**: `static_assert(cond, msg)`.

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
