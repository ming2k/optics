# Surface Material & Shader Governance (Prism)

This document formalizes the intake criteria, physical shader models, and composition constraints for surface materials in `libs/prism` and `libs/flux` (formalizing ADR-0046, ADR-0048, ADR-0063, ADR-0100, ADR-0101).

---

## 1. Domain Boundary

- **Prism OWNS**:
  - Physically based and stylised surface materials (e.g. Liquid Glass, Frost, Acrylic, Frosted Metals).
  - Material parameter structs and shader permutations (convex-lens refraction, chromatic aberration, normal maps).
- **Prism FORBIDS**:
  - Direct window management, event handling, or UI state reconciliation.

---

## 2. Invariants

1. **Deterministic Shader Compilation**: Shaders are compiled to SPIR-V at build time; embedded via C23 `#embed` with generated-header fallback.
2. **Explicit Composition Graphs**: When materials require offscreen scratch buffers (e.g. background blur under glass), dependencies are declared via the explicit composition graph (`ADR-0080`), never through implicit hidden passes.
3. **Single Opts Invocation**: Material dispatches accept single typed parameter structs.
4. **Bounded Dispatch & Footprints (ADR-0101)**: Materials must compute and constrain compute dispatches strictly to their active geometric bounding boxes plus shadow/filter reach. Full-screen dispatch without bounding box validation is forbidden.
5. **Sampling Footprint Budget (ADR-0101)**: No material shader may execute unrolled, unbounded texture sampling loops. Single-fragment sampling cost must strictly adhere to the per-material budget (Liquid Glass <= 4 taps; Frosted/Acrylic/Mica <= 1 tap).
