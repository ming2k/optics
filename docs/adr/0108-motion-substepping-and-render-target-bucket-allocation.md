---
id: ADR-0108
title: "Motion Sub-stepping Integration and Render Target Bucket Allocation for Realtime Animation Safety"
status: accepted
date: 2026-10-06
scope: libs/transit, libs/flux, bindings/transit-rs, bindings/flux-rs, tests/transit
superseded_by: null
negative_knowledge: true
---

# ADR-0108: Motion Sub-stepping Integration and Render Target Bucket Allocation for Realtime Animation Safety

- Status: Accepted
- Date: 2026-10-06
- Scope: `libs/transit`, `libs/flux`, `bindings/transit-rs`, `bindings/flux-rs`, `tests/transit`.
- Amends: [ADR-0077](0077-anim-motion-vocabulary-library.md), [ADR-0106](0106-monorepo-identity-unification-glyph-vista-transit.md).
- Complements: [ADR-0101](0101-physical-sampling-governor-and-bounded-material-dispatch.md).

## Context

Desktop animation profiles under high-resolution and variable-cadence displays (120Hz/60Hz) exposed two structural friction points across the boundary between `transit`, `flux`, and compositor runtimes:

1. **Discrete Jump Under Frame Delivery Hiccups**:
   `transit_spring_advance` calculates the closed-form analytic solution of the damped harmonic oscillator with a single clamped time delta (`dt <= 1/30s`). However, when host presentation experiences temporary driver stalls (such as kernel page-flip lockups or swapchain hiccups), host delta times exceed 30ms. Simply clamping `dt` to 33.3ms drops real-world progression, causing the spring to fall behind actual time or exhibit a discrete, unnatural leap when a following frame lands.
2. **Dynamic Geometry Sweeps and Render Target Allocation Thrashing**:
   When windows or overlay surfaces animate their bounding rectangles (e.g. Genie warp, window resize, dock/panel morphs), intermediate offscreen capture extents vary by fractional or integer pixels each frame. Without canonical extent bucketing helpers in `flux`, host compositors frequently allocate new `flux_image` render targets every frame, thrashing Vulkan `vkCreateImage` and `vkAllocateMemory`.

## Decision

1. **`libs/transit` Exposes Motion Sub-stepping (`transit_spring_advance_substep`)**:
   - Introduces `transit_spring_advance_substep(s, target, p, dt, max_substep_dt, reduced_motion)`.
   - When `dt` exceeds `max_substep_dt`, the spring advances in bounded sub-steps, ensuring smooth continuous progression and velocity preservation without discrete leaps or slow-motion drift.
   - Capped at a safe ceiling (100ms) to avoid iteration runaway on extreme background pauses.
   - Rust binding exposes `Spring::advance_substep`.
2. **`libs/flux` Exposes Canonical Extent Bucketing (`flux_bucket_extent`)**:
   - Adds `flux_bucket_extent(extent, step)` in `<flux/math.h>` and `flux::bucket_extent(extent, step)` in `flux-rs`.
   - Normalizes dynamic render target allocations to 128-pixel buckets, enabling compositors to stabilize image allocations while clamping drawing passes strictly to actual geometric scissors (`render_area`).

## Rejected Alternatives & Negative Knowledge

### 1. Hard-clamping all springs to 1/60s without sub-stepping
- **Why considered**: Simplest approach requiring no iteration loop.
- **Why rejected**: Under 30Hz or 45Hz hiccups, clamping to 16.6ms discards more than half of the elapsed time, making animations appear to run in slow motion before abruptly teleporting when normal pacing resumes.

### 2. Implicit attachment re-allocation inside `flux` canvas sessions
- **Why considered**: The canvas could invisibly grow user-supplied render targets.
- **Why rejected**: Violates the pure deterministic boundary of `flux`. Callers own images and framebuffers; hidden re-allocations inside a render pass break resource tracking, synchronization barriers, and Vulkan command buffer recording.

## Consequences

- Host applications and compositors can reliably advance springs across irregular frame gaps without trajectory distortion.
- Size-sweeping animations stabilize offscreen render targets at zero reallocation cost per frame.
- Zero runtime regression; all 979 `transit` property checks and `flux` unit tests pass.
