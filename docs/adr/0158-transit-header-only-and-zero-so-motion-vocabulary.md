---
id: ADR-0158
title: "Transit is a header-only vocabulary — zero shared object, pure inline math, and FFI elimination"
status: accepted
date: 2026-11-12
scope: libs/transit, bindings/transit-rs, examples/transit, tests/transit
superseded_by: null
negative_knowledge: true
---

# ADR-0158: Transit is a header-only vocabulary — zero shared object, pure inline math, and FFI elimination

- Status: Accepted
- Date: 2026-11-12
- Scope: `libs/transit`, `bindings/transit-rs`, `examples/transit`, `tests/transit`
- Amends: [ADR-0077](0077-anim-motion-vocabulary-library.md), [ADR-0106](0106-monorepo-identity-unification-glyph-vista-transit.md)

## Context

[ADR-0077](0077-anim-motion-vocabulary-library.md) introduced `anim` (renamed `transit` in [ADR-0106](0106-monorepo-identity-unification-glyph-vista-transit.md)) to provide a single authoritative vocabulary of provable motion math: closed-form non-divergent harmonic spring integration, motion-adaptive smoothing, hysteresis latches, and parametric easing.

However, packaging ~490 lines of stateless scalar math as an independent dynamic shared library (`libtransit.so`) introduced significant architectural friction and runtime penalties:

1. **Inhibited Compiler Optimization & Vectorization**:
   Motion calculations (such as advancing a spring or exponential approach) are evaluated per-scalar, per-widget, every frame on the hot animation path. Invoking these small mathematical functions across a dynamic shared object boundary (`.so`) incurs Procedure Linkage Table (PLT) indirection and completely prevents the compiler from performing function inlining, dead-code elimination, and SIMD auto-vectorization.
2. **Maintenance Overhead Inversion**:
   For less than 500 lines of pure math with zero platform dependencies and zero heap allocations, the repository was maintaining:
   - Dynamic library targets and SONAME versioning in `libs/transit/meson.build`;
   - DLL export/import definitions in `include/transit/export.h`;
   - Generated pkg-config metadata (`transit.pc`);
   - Two separate Rust binding crates (`transit-sys` with complex `build.rs` probing and `transit` safe wrapper).
   The packaging ceremony exceeded the complexity of the core mathematical algorithms.
3. **Absence of Internal Consumers**:
   None of the core runtime libraries (`flux`, `glyph`, `lens`, `iris`, `prism`) link `libtransit.so`. `lens` manages its own internal exponential decays, leaving `transit` as a standalone leaf vocabulary for host applications.

## Decision

Transform `libs/transit` from a dynamic shared library into a **pure C23 single-header vocabulary** with zero backward-compatibility shims:

1. **Header-Only Inlining**:
   All public functions in [`include/transit/transit.h`](../../libs/transit/include/transit/transit.h) are declared and defined `static inline` via `TRANSIT_API`. Calling translation units compile the closed-form math directly into their own binary.
2. **Elimination of `libtransit.so`**:
   Excise `libs/transit/src/transit.c` and remove `library('transit', ...)` from `libs/transit/meson.build`. `transit_dep` is re-declared as a pure header-only interface dependency (`declare_dependency(include_directories: transit_inc, dependencies: [m_dep])`). The build graph produces no `libtransit.so`.
3. **Rust Motion Implementation**:
   Within the unified Rust bindings (ADR-0159), motion primitives are provided in pure Safe Rust within the `optics::motion` module. Calling C across FFI for 400 lines of basic arithmetic is permanently retired, granting `rustc` full inlining capability while eliminating `transit-sys`.

## Consequences

Positive:
- **Zero Dynamic Linker Overhead**: No PLT dispatch, no `libtransit.so` runtime search path configuration (`RPATH`/`RUNPATH`), and zero ABI breakage risks across patch releases.
- **Maximum Execution Efficiency**: Compilers can inline spring integration and easing directly into caller frame loops, enabling constant propagation and auto-vectorization.
- **Radical Tooling Simplification**: Deleted C compilation units, symbol visibility maps, and FFI probe scripts.

Negative / Breaking Changes:
- External C consumers must not link `-ltransit`; they only `#include <transit/transit.h>`.
- Rust consumers import `optics::motion` rather than depending on `transit` or `transit-sys`.

## Alternatives Considered

- **Keep `libtransit.so`**: Rejected. Retains PLT overhead and packaging bloat for 490 lines of code.
- **Absorb directly into `libs/lens`**: Rejected. Motion physics (e.g. spring integration) has valid standalone use in window managers, headless compositors, and custom canvas tools without pulling in the entire UI layout engine.
