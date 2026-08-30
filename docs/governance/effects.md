# Image Effect & Operator Governance (Flux Effect)

This document formalizes the intake criteria, mathematical purity rules, and GPU resource governance for visual operators and image effects in `libs/flux/effect` and `libs/prism` (formalizing ADR-0074).

---

## 1. Physical Operator Invariant

An **Effect Operator** in Flux is a pure mathematical/GPU transformation over pixel textures.

### The Inviolable Separation:
- **Flux Effect OWNS**:
  - Pure GPU compute/fragment shaders transforming input textures to output textures (e.g. separable Gaussian blur, dual-Kawase blur, drop shadow convolution, bloom thresholding, color matrix grading).
  - Explicit texture input/output descriptors and bounded scratch buffer allocations.
- **Flux Effect FORBIDS**:
  - **Choreography & Time**: An effect operator must NEVER take a `float time`, manage animation springs, or schedule keyframes. Time and motion choreography belong exclusively to the host application or the `anim` library.
  - **Layout & Geometry**: An effect operator does not calculate UI layouts or position nodes.

---

## 2. Effect Intake Gates (5-Step Evaluation)

```
[ New Effect Proposal ]
          │
          ▼
┌────────────────────────────────────────────────────────┐
│ Gate 1: Is this a stateless pixel operator?            │
│         (No time clocks, no animation curves inside)   │
└──────────────────────────┬─────────────────────────────┘
                           │
                 YES ──────┴────── NO ──▶ [ REJECT / MOVE TO ANIM ]
                  │
                  ▼
┌────────────────────────────────────────────────────────┐
│ Gate 2: Does it have bounded GPU memory overhead?      │
│         (Fixed-size scratch render targets)            │
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
[ ADMIT INTO FLUX EFFECT ]
```
