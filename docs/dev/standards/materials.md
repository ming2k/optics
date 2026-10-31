# Surface Material Governance & Invariants (Prism)

This document formalizes the intake criteria, physical rendering constraints, and architectural boundaries for surface materials in `libs/prism` (formalizing ADR-0046, ADR-0063, ADR-0096, ADR-0100, ADR-0101, ADR-0102).

---

## 1. Domain Boundary

`libs/prism` is the physical optics and surface material authority in Optics.

- **`libs/prism` OWNS**:
  - Physical optical models: Dielectric Liquid Glass (refraction, chromatic dispersion, contact ambient occlusion, specular rim lighting).
  - Surface visual identities: Frosted Glass, Mica, Acrylic, Smoked Glass, and Metallic finishes.
  - Multi-pass offscreen render graphs and screen-space blur bucketing for background diffusion.
  - Sampling governor policies and dynamic resolution scaling for optical effects.
- **`libs/prism` DEPENDS ON**:
  - `libs/flux` (pure RHI and rasterization foundation).
- **`libs/prism` FORBIDS**:
  - Embedding material-specific logic or symbol names inside `libs/flux` (`[INV-ARCH-MATERIAL]`, ADR-0063).

---

## 2. Invariants

1. **Absolute Material Isolation (ADR-0063)**:
   - `flux` must remain completely material-agnostic. No symbol, header, or shader in `libs/flux` may reference `prism`, `liquid_glass`, `mica`, or `acrylic`.
   - Enforced mechanically by `tools/optics check boundary` (`tools/guardrails/boundary.py`).
2. **Shader Register & Bandwidth Budget**:
   - Material fragment shaders must run within bounded register pressure to maintain target framerates across integrated GPUs and mobile hardware.
   - Screen-space blur passes must utilize downsampled scratch buffers and ping-pong texture buckets rather than full-resolution quadratic taps.
3. **Render Target Bucket Allocation (ADR-0108)**:
   - Backdrop blur and reflection targets must be allocated from pre-bucketed pools to avoid runtime GPU reallocations during animated interactions.
4. **Single Opts Entry Point**:
   - All material construction and configuration adhere to the Single Opts Descriptor paradigm (e.g., `prism_liquid_glass(&(prism_liquid_glass_opts){ ... })`).
