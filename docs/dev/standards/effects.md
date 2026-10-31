# Image Effect Governance & Intake Specification

This document formalizes the intake criteria, mathematical purity rules, and GPU resource governance for visual operators and image effects in `libs/flux/effect` and `libs/prism` (formalizing ADR-0008, ADR-0074, ADR-0108).

---

## 1. Mathematical Purity & Operator Taxonomy

An image effect in Optics is a pure spatial or chromatic transformation of an input texture or framebuffer:

1. **Spatial Convolutions**:
   - Separable Gaussians, dual-filtering blurs, box blurs, directional motion blurs.
   - Must expose closed-form analytic kernels with radius and sigma bounds.
2. **Geometric Transformations**:
   - Barrel/pincushion distortion, chromatic aberration dispersion vectors, perspective warps.
3. **Color & Tone Mapping**:
   - Contrast/brightness modulation, color grading LUTs, tone curves, transfer function conversions.

---

## 2. Invariants & Resource Bounds

1. **Zero Dynamic Choreography**:
   - Core effects are static pixel transformers. Temporal animation, physics spring damping, and transition sequencing belong to `transit` or userland controllers.
2. **Bounded Scratch Target Overhead**:
   - Multi-pass operations (such as dual-pass Gaussian or downsampled pyramid blurs) must utilize pooled scratch render targets (`flux_target_pool`).
   - Unbounded dynamic allocations during effect passes are strictly prohibited.
3. **Single Opts Invocation**:
   - Every effect function adheres to the Single Opts Descriptor pattern:
     ```c
     void flux_effect_blur(flux_canvas *canvas, const flux_effect_blur_opts *opts);
     ```

---

## 3. Admission Gate Filter

```text
┌────────────────────────────────────────────────────────┐
│ Gate 1: Is it a pure spatial/chromatic pixel operator? │
│         (No state machines, no temporal animation)     │
└──────────────────────────┬─────────────────────────────┘
                           │
                 YES ──────┴────── NO ──▶ [ REJECT / MOVE TO TRANSIT ]
                  │
                  ▼
┌────────────────────────────────────────────────────────┐
│ Gate 2: Does it have bounded GPU memory overhead?      │
│         (Allocates from pooled scratch targets)        │
└──────────────────────────┬─────────────────────────────┘
                           │
                 YES ──────┴────── NO ──▶ [ REJECT ]
                  │
                  ▼
┌────────────────────────────────────────────────────────┐
│ Gate 3: Does it follow Single Opts Descriptor?         │
│         flux_effect_<name>(canvas, &(opts){ ... })     │
└──────────────────────────┬─────────────────────────────┘
                           │
                 YES ──────┴────── NO ──▶ [ REFACTOR API ]
                  │
                  ▼
[ ADMIT INTO FLUX / PRISM ]
```
