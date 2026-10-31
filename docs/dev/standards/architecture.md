# System Architecture Governance & Topology

This document codifies the monorepo topology, library ownership boundaries, the 5-Gate Review Filter, and the non-negotiable architectural red-lines of Optics.

---

## 1. Monorepo Micro-Kernel Topology

Optics is organized into seven focused, orthogonal C23 libraries under `libs/`, plus safe bindings in `bindings/`:

```text
┌──────────────────────────────────────────────────────────────────────┐
│  Application Layer / Integrators / Examples                         │
├──────────────────────────────────────────────────────────────────────┤
│  Layer 3: UI State & Platform Shell                                  │
│  - `libs/lens`: Immediate-mode UI engine, layout (Grid/Flex/Flow),   │
│                 retained store, accessibility tree export (A11y)     │
│  - `libs/iris`: Native OS windowing, event pump, display pacing      │
│                 (Wayland / Win32 / Cocoa), clipboard, IME            │
├──────────────────────────────────────────────────────────────────────┤
│  Layer 2: Visual Capabilities & Material Optics                      │
│  - `libs/prism`: Physical material BRDFs, dielectric Liquid Glass    │
│                  (dispersion & IOR), Frosted Glass, Acrylic, Mica    │
│  - `libs/transit`: Provable motion physics, closed-form analytic     │
│                    spring integration, parametric easing             │
│  - `libs/vista`: Scene graph orchestration, cameras, hierarchical    │
│                  transforms, spatial bounds                          │
├──────────────────────────────────────────────────────────────────────┤
│  Layer 1: Typography & Text Shaping                                  │
│  - `libs/glyph`: HarfBuzz-backed text shaping, layout metrics,       │
│                  dynamic texture atlas caching                       │
├──────────────────────────────────────────────────────────────────────┤
│  Layer 0: Rendering Foundation & Rasterization Core                  │
│  - `libs/flux`: Pure C23 Vulkan 1.3 RHI, 2D vector canvas (GPU &     │
│                 headless CPU software fallback), 3D draw core,       │
│                 compute pipelines, analytic curve kernel             │
└──────────────────────────────────────────────────────────────────────┘
```

Dependencies flow strictly inward from higher layers to lower layers. Reverse dependencies across layer boundaries are prohibited.

---

## 2. The Four Architectural Mandates

All software in Optics adheres to four non-negotiable architectural mandates:

1. **Micro-Kernel Principle & Minimal Orthogonality**:
   - Each library owns exactly one physical domain (Rendering, Typography, Scene, Materials, Motion, UI State, Platform Shell).
   - Core libraries contain **only irreducible primitives**. Compound helpers, pre-baked layouts, and aggregate widgets belong to userland component kits or applications, never the engine core.
2. **Single Opts Descriptor Paradigm**:
   - Every public functional entry point accepts **exactly one** typed descriptor struct pointer (`const module_<item>_opts *opts`) and returns a uniform status or response object.
   - Dual API tiers (`_ex`), parameter-derived suffixes (`_wrapped`, `_primary`, `_vertical`), and floating state modifiers are strictly forbidden.
3. **Strict Layered Separation**:
   - Architectural dependencies flow strictly inward from higher layers to lower layers (`Layer 3: UI & Shell` ➔ `Layer 2: Scene & Motion` ➔ `Layer 1: Typography` ➔ `Layer 0: Rendering Foundation`). Lower layers must never depend on, reference, or leak abstractions from higher layers.
4. **Zero-Baggage Deprecation Policy**:
   - When a design is superseded by an orthogonal primitive, the obsolete API is completely excised. No shims, deprecated aliases, or compatibility redirects are retained in the core library.

---

## 3. Architectural Red-Lines

### 3.1 Material Boundary Separation (ADR-0063)
- **Red-Line**: `flux` (Layer 0) must NEVER reference named materials (`prism_`, `liquid_glass`, frosted glass, mica, acrylic) or higher-level material shaders.
- **Enforcement**: Mechanically verified by `tools/optics check boundary` (via `tools/guardrails/boundary.py`) on every commit and CI run. `flux` exposes generic shader pipelines, bindless handles, and canvas render targets; high-level BRDF models belong exclusively to `prism`.

### 3.2 Vulkan RHI Purity & Headless Software Fallback (ADR-0006, ADR-0019)
- **Red-Line**: Optics maintains no runtime RHI abstraction layer. Vulkan 1.3 is the primary GPU backend.
- **Headless CPU Software Fallback**: `libs/flux` provides `<flux/canvas_cpu.h>` for GPU-free execution, headless rendering, pixel testing, and continuous integration.

### 3.3 Zero Heap Allocation during Frame Walk (ADR-0024)
- **Red-Line**: Hot-path frame traversal, scene evaluation, layout passes, and draw command emission must never invoke libc `malloc`, `calloc`, `realloc`, or `free`.
- **Enforcement**: All temporary allocations borrow strictly from per-frame / transient arenas, reset at frame boundaries. Multi-frame state is keyed in caller handles or designated retained stores.

### 3.4 Library Stdout Hygiene
- **Red-Line**: Shared library translation units under `libs/` must never write to `stdout` (`printf`, `puts`). Standard output is strictly reserved for application CLI or IPC wires. Diagnostic messages use `stderr`.
- **Enforcement**: Mechanically asserted by `tools/optics check stdout` (via `tools/guardrails/stdout_purity.py`).

---

## 4. The 5-Gate Review Matrix

Every PR proposing a new public API, core primitive, or subsystem capability must pass through the 5-Gate Filter:

```text
[ Proposal ] ──▶ [ Gate 1: Irreducibility ] ──▶ [ Gate 2: Orthogonality ]
                        │                              │
                     Pass?                          Pass?
                        ▼                              ▼
                 [ Gate 3: Layer Purity ]   ──▶ [ Gate 4: API Compliance ]
                        │                              │
                     Pass?                          Pass?
                        ▼                              ▼
                 [ Gate 5: Zero-Alloc Execution ] ──▶ [ Merge into Core ]
```

1. **Gate 1: Irreducibility**: Can this functionality be composed from existing primitives in userland or client applications? (If YES ➔ Reject from core).
2. **Gate 2: Orthogonality**: Does this introduce a genuinely new, orthogonal capability or fundamental model rather than a redundant convenience wrapper? (If NO ➔ Reject or configure via existing options).
3. **Gate 3: Layer Purity**: Does the proposed API or module strictly adhere to its architectural layer without creating reverse dependencies or blurring cross-layer boundaries? (If NO ➔ Reject cross-layer hybrid).
4. **Gate 4: API Compliance**: Does it adhere to the Single Opts Descriptor paradigm and repository C23 conventions? (If NO ➔ Reject non-standard signatures).
5. **Gate 5: Zero-Alloc Execution**: Does the hot path or frame walk execute with zero heap allocation (`malloc`), using only transient/frame arena storage? (If NO ➔ Reject).
