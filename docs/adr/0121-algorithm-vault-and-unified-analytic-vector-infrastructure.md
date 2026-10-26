---
id: ADR-0121
title: "Algorithm vault and unified analytic vector infrastructure"
status: accepted
date: 2026-10-25
superseded_by: null
scope: libs/flux, shaders, architecture
negative_knowledge: true
---

# ADR-0121: Algorithm vault and unified analytic vector infrastructure

- Status: Accepted
- Date: 2026-10-25
- Scope: `libs/flux/include/flux/`, `libs/flux/src/canvas/`, `libs/flux/src/scene/`, shaders.
- Depends on: [ADR-0115](0115-analytic-curve-kernel-single-geometry-authority.md),
  [ADR-0116](0116-coverage-contract-canvas-rasterization.md),
  [ADR-0117](0117-perspective-correct-scene-vector-coverage.md),
  [ADR-0118](0118-coverage-strategy-placement-distance-fields-and-curve-kernel.md),
  [ADR-0120](0120-gpu-analytic-coverage-tile-compute-plan.md).
- Amends: [ADR-0005](0005-tessellator-scope.md) and [ADR-0011](0011-hole-bridging-tessellator.md)
  (tessellation algorithms formally reclassified as preserved geometric assets in the Algorithm Vault).

## Context

The convergence across ADR-0115 through ADR-0120 proved the superiority of the
resolution-independent 1× analytic coverage paradigm over legacy CPU triangulation
and 4× MSAA / stencil attachments. On actual hardware, the device path delivers
100+ distinct coverage levels, eliminates multisample buffers, and resolves self-intersecting
paths natively.

However, the staged transition produced four architectural liabilities:

1. **Algorithmic entitlement vs. obsolete code.** Traditional triangulation
   (ear-clipping, convex fan pruning, hole bridging) is obsolete for modern 2D
   fill rasterization, yet premature total deletion discards thousands of lines
   of battle-tested computational geometry needed for future 3D extrusion,
   navmesh generation, or non-GPU fallbacks.
2. **Entangled dispatch.** Modern analytic coverage routing (`submit_cover`)
   was implemented inside `geometry_tess.c`, creating a parasitic dependency
   where the future rasterization engine is hosted inside an ear-clip file.
3. **Duplicated GPU fragment evaluation.** The ray-cast winding walk, segment
   Euclidean boundary distance, and screen-space Jacobian anti-aliasing logic
   were duplicated between `canvas_vector.frag` (2D physical space) and
   `scene_vector.frag` (3D projective space).
4. **Segment structure fragmentation.** Line segments lacked a canonical
   16-byte GPU-aligned memory layout across host subsystems.

## Decision

Establish the **Algorithm Vault** governance pattern, unify the GPU analytic
vector evaluation kernel, and formalize the 2D segment primitive contract:

### 1. The Algorithm Vault: Preserving Algorithms as Stateless Assets

An algorithm superseded in the primary rasterization path is neither left as dead
code in the hot loop nor deleted into git history. Instead, it is **vaulted**:
- **Stateless & Pure:** Vaulted algorithms must be side-effect free, memory-bounded,
  and decoupled from device/canvas state machines.
- **Formal Failure Modes:** The asset documentation must record precisely where
  it fails (e.g., ear clipping degrades to $\Theta(n^2)$ on degenerate vertices).
- **Golden Oracle:** Vaulted algorithms serve as deterministic CPU references
  and differential testing fixtures against parallel GPU implementations.
- **Zero Silent Fallback (INV-VAULT-01):** The primary rendering pipeline must
  never silently degrade to a vaulted algorithm; selection must be explicit.

### 2. Unified Hardware-Level 2D Line Segment Primitive (`flux_segment`)

All line segment representations across tile binning, SSBO transport, and fragment
shaders conform to an exact 16-byte contiguous layout:
```c
typedef struct flux_segment {
    float x0, y0;
    float x1, y1;
} flux_segment;
static_assert(sizeof(flux_segment) == 16, "16-byte GPU SSBO alignment");
```
This guarantees zero-copy transport via 64-bit Buffer Device Address (BDA)
directly into compute and fragment stages.

### 3. Unified GPU Analytic Vector Evaluation (`flux_vector_eval.glsl`)

Consolidate the mathematical core of analytic fragment rasterization into
`libs/flux/include/flux/shaders/flux_vector_eval.glsl`:
- `flux_vector_eval_segment(p, a, b, winding, min_dist_sq)`: Ray-cast winding
  crossing along $+X$ and Euclidean distance calculation in a single pass.
- `flux_vector_eval_coverage(winding, min_dist_sq, fill_rule, pixel_scale)`:
  Continuous $1\times$ analytic coverage ramp.
- `flux_vector_eval_coverage_jacobian(winding, min_dist_sq, fill_rule, obj_pos)`:
  Perspective-correct coverage using hardware partial derivatives (`dFdx`/`dFdy`).

Both `canvas_vector.frag` (2D) and `scene_vector.frag` (3D) include and evaluate
this single authoritative definition, preventing shader divergence.

### 4. Decoupled Path Fill Dispatch

The dispatch authority for path rasterization is structurally decoupled from the
ear-clip tessellator:
- Fast path: Closed-form distance fields (`FLUX_GEOM_RRECT`, `FLUX_GEOM_CIRCLE`,
  `FLUX_GEOM_SQUIRCLE` under similarity transforms) route directly to SDF shaders.
- Primary vector path: Complex paths route to the backend's `submit_cover`
  (Vello-class `TILE_COMPUTE` or Slug-class `ANALYTIC_FRAGMENT`).
- Vaulted fallback: Explicit legacy tessellation occurs only on backends lacking
  analytic coverage capabilities or in dedicated offline test suites.

## Invariants & Behavioral Boundaries

- **`[INV-VAULT-01]` No Silent Degradation:** A canvas pass operating under an
  analytic coverage policy must never silently degrade to ear-clipping or
  stencil-then-cover. Unsupported combinations return an explicit error code.
- **`[INV-VAULT-02]` Stateless Asset Purity:** Algorithms preserved in the vault
  must not allocate unbounded dynamic heap memory or hold references to Vulkan
  device queues.
- **`[INV-VAULT-03]` Single Analytic Shader Authority:** Line segment ray-cast
  and Jacobian coverage evaluation must never be reimplemented inline in
  production shaders; all fragment consumers must include `flux_vector_eval.glsl`.
- **`[INV-VAULT-04]` Aligned Segment Layout:** Any GPU-bound line segment structure
  must maintain strict 16-byte alignment (`vec2 p0, vec2 p1`).

## Rejected Alternatives & Negative Knowledge

- **Option A — Delete ear-clipping and polyline stroking outright.**
  *Why considered:* Removes legacy lines of code immediately.
  *Why rejected:* Ear-clipping and hole-bridging represent extensive investments in
  numerical stability and degenerate vertex handling. Deleting them destroys the
  ability to perform CPU mesh extraction, 3D polygon extrusion, and offline geometry
  baking. Vaulting preserves them as pure computational geometry tools without polluting
  the GPU hot path.
- **Option B — Keep inline mathematical implementations in `scene_vector.frag` and `canvas_vector.frag`.**
  *Why considered:* Avoids cross-module header file dependencies in shaders.
  *Why rejected:* Exact duplicate drift occurred previously with SDF definitions
  (ADR-0118). Duplicating ray-casting and distance evaluation across 2D and 3D
  shaders inevitably causes subtle anti-aliasing edge ramp discrepancies.
- **Option C — Use Slug-class fragment looping for all path sizes on Vulkan.**
  *Why considered:* Simple shader execution without complex tile compute passes.
  *Why rejected:* Evaluating $O(N)$ segments per fragment causes catastrophic
  performance collapse when $N > 128$. Tile-compute binning (ADR-0120) remains
  mandatory for general high-vertex vector paths.
