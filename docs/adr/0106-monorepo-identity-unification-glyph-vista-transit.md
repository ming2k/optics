---
id: ADR-0106
title: "Monorepo Identity Unification — glyph, vista, and transit"
status: accepted
date: 2026-09-27
scope: libs/glyph, libs/vista, libs/transit, libs/flux, libs/lens, tools, bindings
superseded_by: null
negative_knowledge: true
---

# ADR-0106: Monorepo Identity Unification — glyph, vista, and transit

- Status: Accepted
- Date: 2026-09-27
- Scope: `libs/glyph` (formerly `libs/flux/text`), `libs/vista` (formerly `libs/flux/scene_graph`), `libs/transit` (formerly `libs/anim`), `libs/flux`, `libs/lens`, `tools/check-version-lockstep.sh`, `bindings/`.
- Amends: [ADR-0016](0016-pure-rhi-and-draw-primitives.md), [ADR-0077](0077-anim-motion-vocabulary-library.md).

## Context

The Optics monorepo is grounded in a clean, physically evocative optical and spatial metaphor:
- **`flux`**: Luminous flux — foundational Vulkan hardware interface, 2D canvas, 3D core, compute pipelines;
- **`prism`**: Refractive optics — dielectric liquid glass, acrylic, and physical materials;
- **`lens`**: Focusing and view formation — declarative immediate-mode UI facade over a retained layout tree;
- **`iris`**: Aperture and environmental perception — desktop integration, window management, and display event loops.

However, three architectural components carried historical naming anomalies and structural asymmetry:

1. **`flux-text` as an auxiliary hyphenated compound**:
   Extracted in ADR-0016 as a standalone HarfBuzz text shaping sibling with its own `.so` and `.pc`, it remained physically nested under `libs/flux/text/` with the prefix `flux-text`. It functions as the stack's universal text shaper (consumed directly by `lens` and applications), yet its name relegated it to a sub-feature of the rendering engine.

2. **`flux-scene-graph` as a verbose asset container**:
   Responsible for glTF 2.0 loading, TRS transform hierarchies, and skeletal animation, it sat nested under `libs/flux/scene_graph/` and exported fragmented identifiers (`flux_sg_*`).

3. **`anim` as a pedestrian abbreviation**:
   While `flux`, `lens`, and `prism` embody rich physical metaphors, `anim` was a generic, utilitarian abbreviation for animation. It lacked scientific resonance and failed to evoke the library's provable mathematical core: closed-form non-divergent harmonic spring integration and motion-adaptive smoothing.

With the repository enforcing global version lockstep across all libraries and bindings (ADR-0098, `check-version-lockstep.sh`), maintaining nested, hyphenated, and utilitarian names imposes unnecessary cognitive friction and architectural debt.

## Decision

Elevate and unify all components into autonomous, first-class peer libraries with distinct, single-word optical and physical identities under `libs/`:

```text
The Unified Optics Ecosystem:
├── flux    (luminous flux)     : Vulkan RHI, 2D canvas, 3D core, and compute pipelines
├── glyph   (letterform/carve)  : Text shaping, layout metrics, and dynamic texture atlas
├── vista   (perspective/depth) : 3D scene hierarchy, TRS node transforms, and glTF assets
├── prism   (refraction/glass)  : Physical material optics, dielectric glass, and filters
├── transit (state transition)  : Closed-form non-divergent springs and adaptive smoothing
├── lens    (focus/imaging)     : Reactive immediate-mode UI engine and widget hierarchy
└── iris    (aperture/sensing)  : Platform window management and desktop event loops
```

1. **`flux-text` is promoted and renamed to `glyph`**:
   - Location: `libs/glyph/` (formerly `libs/flux/text/`).
   - Public header: `<glyph/glyph.h>`.
   - Export macro: `GLYPH_API`.
   - Symbol prefix: `glyph_*` (e.g. `glyph_ctx`, `glyph_create`, `glyph_draw`, `glyph_measure`).
   - Build artifacts: `libglyph.so`, `glyph.pc`.

2. **`flux-scene-graph` is promoted and renamed to `vista`**:
   - Location: `libs/vista/` (formerly `libs/flux/scene_graph/`).
   - Public header: `<vista/vista.h>`.
   - Export macro: `VISTA_API`.
   - Symbol prefix: `vista_*` (e.g. `vista_scene`, `vista_load_glb`, `vista_scene_draw`).
   - Build artifacts: `libvista.so`, `vista.pc`.

3. **`anim` is renamed to `transit`**:
   - Location: `libs/transit/` (formerly `libs/anim/`).
   - Public header: `<transit/transit.h>`, `<transit/export.h>`.
   - Export macro: `TRANSIT_API`.
   - Symbol prefix: `transit_*` (e.g. `transit_spring`, `transit_spring_advance`, `transit_smoother_step`).
   - Build artifacts: `libtransit.so`, `transit.pc`.

4. **One Directory, One Library Symmetry**:
   All 7 core components sit directly under `libs/` as peers. `libflux` retains its foundational RHI, 2D canvas, and 3D draw primitive core while asset loaders and text shapers interact cleanly through public dependencies.

## Alternatives Considered

- **Keep `flux-` prefix and nested directories (Status Quo)**:
  Rejected. Subordinates universal capabilities (text shaping, scene graphs) under a single renderer and breaks the one-directory-one-library standard.

- **Generic names (`text`, `scene-graph`, `scene`)**:
  Rejected. In C's flat global namespace and system include directories (`/usr/include/text/text.h`), generic names cause severe collisions with third-party utilities and distribution packages.

- **Naming `flux-scene-graph` as `stage`**:
  Rejected. In modern Vulkan graphics, `stage` is heavily overloaded with pipeline stages (`VK_PIPELINE_STAGE_*`) and staging memory allocation (`staging buffer`). `vista` evokes depth and spatial perspective without ambiguity.

- **Naming `anim` as `drift`**:
  Rejected. In systems engineering, "drift" carries negative defect connotations (clock drift, sensor drift, gyro drift), which contradicts the library's core contract of provable, non-divergent analytic convergence.

- **Naming `anim` as `phase` or `phrase`**:
  Rejected. `phrase` suggests natural language processing. `phase` universally denotes compile or build phases in software toolchains. `transit` reflects the fundamental purpose of UI animation: governed physical state transition.

## Consequences

### Positive
- Every library in the Optics monorepo commands equal standing as a clean, single-word entity.
- The directory tree (`libs/{flux,glyph,vista,prism,transit,lens,iris}`) mirrors library outputs (`lib*.so`, `*.pc`) 1:1.
- Eradicates fragmented acronyms (`flux_sg_*`) and utilitarian abbreviations (`anim_*`).

### Negative / Migration Effort
- Requires a clean-break update of include paths, symbol prefixes, and Meson options across tests, examples, and documentation. Pre-1.0 lockstep versioning (0.0.51) makes this the ideal window for atomic execution.
