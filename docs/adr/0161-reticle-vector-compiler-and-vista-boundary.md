---
id: ADR-0161
title: "Reticle Vector Compiler and Vista Subsystem Boundary"
status: accepted
date: 2026-11-12
scope: libs/reticle/, libs/lens/, tests/vista/, tests/reticle/, bindings/crates/reticle/
superseded_by: null
negative_knowledge: true
---

# ADR-0161: Reticle Vector Compiler and Vista Subsystem Boundary

- Status: Accepted
- Date: 2026-11-12
- Scope: `libs/reticle/`, `libs/lens/`, `tests/vista/`, `tests/reticle/`, `bindings/crates/reticle/`
- Amends: [ADR-0106](0106-monorepo-identity-unification-glyph-vista-transit.md), [ADR-0150](0150-svg-vector-document-compiler.md), [ADR-0159](0159-unified-cargo-workspace-and-optics-facade-bindings.md), [ADR-0160](0160-unified-optics-c-facade-and-umbrella-distribution.md)

## Context

In the evolution of the Optics C23 engine architecture, two naming and boundary irregularities created cognitive dissonance and code duplication:

1. **Aesthetic & Metaphor Discordance (`svg` vs `reticle`)**:
   Every core library in Optics follows a coherent physical and optical metaphor:
   - `flux`: Luminous energy and radiant flow (Vulkan 1.3 RHI & 2D/3D canvas);
   - `glyph`: Carved letterforms and typographical contours (HarfBuzz text shaping & dynamic atlas);
   - `vista`: Spatial sightline, field of view, and panoramic depth (3D scene graph & skeletal animation);
   - `prism`: Refraction, dispersion, and spectral material BRDFs (dielectric physical surfaces);
   - `transit`: Movement across physical space (closed-form motion physics and easing);
   - `lens`: Focusing and structuring the visible interface (immediate/retained UI layout);
   - `iris`: Aperture controlling exposure to the outside world (OS windowing and event pump).

   In contrast, the sibling library `svg` introduced in ADR-0150 broke this naming system by adopting a raw file extension acronym, marring the poetic coherence of the stack.

2. **Unfinished Migration & 1,350-line Duplicate Parser in `lens`**:
   ADR-0150 established the vector-art document compiler to eliminate ad-hoc SVG parsers across the engine. However, the migration was left incomplete: `libs/lens/src/icon_svg.c` retained an internal 1,350-line legacy tag scanner and path converter inherited from an ancient nanosvg fork. This duplicate parser suffered from legacy impedance mismatches, including fabricating fake cubic Béziers for straight lines and harboring redundant allocations.

3. **Subsystem Boundary & Test Entanglement for `vista`**:
   `vista` is a first-class product subsystem providing glTF 2.0 scene graph orchestration, transform hierarchies, and VRM avatar skeletal animations. However, its unit, integration, and benchmark tests previously resided under `tests/flux/unit/test_glb_parse.c`, `tests/flux/integration/test_scene_graph_animation.c`, and `tests/flux/bench/bench_sg_worlds.c`. This misplaced test structure blurred the separation of concerns between Layer 0 (`flux`) and Layer 2 (`vista`).

## Decision

We execute a clean, uncompromised architectural refactoring without backwards compatibility shims:

1. **Renaming `svg` to `reticle` (分划板/刻线)**:
   - The vector document compiler is officially renamed to **`reticle`**, evoking the precision reticle/graticule etched onto optical lenses.
   - All C source files and directories are relocated from `libs/svg/` to `libs/reticle/`.
   - The public header becomes `<reticle/reticle.h>` with macro prefix `RETICLE_*` and symbol prefix `reticle_*`.
   - Shared library target is `libreticle.so`, configured via meson option `reticle` (default `true`) and pkg-config `reticle.pc`.
   - Rust crates are renamed to `reticle-sys` and `reticle`, with feature flags `reticle` and alias `vector` in the root `optics` facade.

2. **Complete Eradication of `icon_svg.c` in `lens`**:
   - `libs/lens/src/icon_svg.c` and `libs/lens/src/icon_svg.h` are deleted in their entirety.
   - `lens_icon_register_svg` in `libs/lens/src/icon_runtime.c` is rewritten to parse input directly via `reticle_parse` and iterate shapes using `reticle_shape_at`.
   - Straight line commands are faithfully mapped to `lens_icon_cmd` type 1 (`line_to`), eliminating all nanosvg degenerate cubic approximations.
   - `libs/lens` explicitly declares its dependency on `reticle_dep`.

3. **Decoupling `vista` Tests into `tests/vista/`**:
   - glTF parsing, scene graph animation, and avatar world-transform benchmarks are moved from `tests/flux/` to dedicated `tests/vista/`:
     - `tests/vista/test_glb_parse.c`
     - `tests/vista/test_scene_graph_animation.c`
     - `tests/vista/bench_sg_worlds.c`
   - `tests/flux/` no longer links or depends on `vista_dep`, restoring absolute Layer 0 purity.

## Alternatives Considered

- **Keep `svg` name as a pragmatic exception**:
  Rejected. A compromised architecture rots from small inconsistencies. Optics' optical metaphors express the architectural role of each subsystem. "Reticle" captures the exact essence of static vector crosshairs, guides, and glyph templates.
- **Provide backwards-compatible `svg_*` aliases or symlinks**:
  Rejected per the core mandate: *No backwards compatibility, no historical baggage, break clean*. Shims clutter autocompletion and invite technical debt.
- **Fold `reticle` into `flux`**:
  Rejected. `flux` is Layer 0 RHI and vector path rasterizer. XML/SVG document parsing belongs to an orthogonal document compilation phase, not to the rendering hardware interface.

## Consequences

- **Purity**: Zero duplicate SVG parsers anywhere in the repository.
- **Layer Hygiene**: `flux` has zero dependency on `vista`; `vista` tests live in `tests/vista/`; `lens` explicitly consumes `reticle`.
- **API Surface**: `<optics/optics.h>` includes `<reticle/reticle.h>`.
- **Rust Surface**: `cargo` workspace members are `crates/reticle-sys` and `crates/reticle`.
- **Binary Footprint**: Reduced `liblens.so` size by removing 1,350 lines of parser code.

## References

- [ADR-0063](0063-material-boundary-separation.md): Material Boundary Separation
- [ADR-0106](0106-monorepo-identity-unification-glyph-vista-transit.md): Monorepo Subsystem Identity
- [ADR-0150](0150-svg-vector-document-compiler.md): SVG Vector Document Compiler
- [ADR-0159](0159-unified-cargo-workspace-and-optics-facade-bindings.md): Unified Cargo Workspace and Optics Facade Bindings
- [ADR-0160](0160-unified-optics-c-facade-and-umbrella-distribution.md): Unified Optics C Facade and Umbrella Distribution
