# Explanation & Conceptual Architecture

Discursive documentation explaining the design rationale, mathematical models, physical optics, and architectural boundaries of Optics. Read these documents to understand *why* the stack is built the way it is.

---

## Articles Directory

| Article | Explains |
| :--- | :--- |
| **[Application Architecture](application-architecture.md)** | Subsystem relationships, layer boundaries, and execution models across `flux`, `lens`, and `iris`. |
| **[Design Philosophy](design-philosophy.md)** | Core principles: Pure C23 baseline, Vulkan 1.3 first-class, micro-kernel orthogonality, zero baggage. |
| **[Value-Semantic Rendering (VSR)](rendering-model.md)** | Unification of immediate-mode and retained-mode graphics through pure value semantics and frame arena lifetimes. |
| **[Liquid Glass Optics](liquid-glass-optics.md)** | Physical dielectric optics, refractive dispersion, contact ambient occlusion, and rim lighting in `prism`. |
| **[Geometry & Coverage Algorithms](geometry-and-coverage.md)** | Map of 2D geometry representations: Bézier curves, analytic curve kernels, distance fields, and rasterization coverage. |
| **[Vulkan Backend Architecture](vulkan-backend.md)** | Pure Vulkan 1.3 RHI design, dynamic rendering, timeline semaphores, descriptor sets, and pipeline caches. |
| **[Scene Snapshots & Presentation](scene-snapshots.md)** | Frame snapshot generation, multi-layer compositing, overlay z-bands, and decoupling UI from rendering. |
| **[Rust Bindings Architecture](rust-bindings.md)** | Design of idiomatic, safe Rust wrapper crates in `bindings/` with zero-overhead C ABI interop. |

For ratified architectural decisions and historical context, consult the [Architecture Decision Records](../adr/index.md).
