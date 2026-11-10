---
id: ADR-0156
title: "Slang-first shader architecture — retiring GLSL, modular device code, and clean-break SPIR-V compilation"
status: accepted
date: 2026-11-10
scope: [flux, prism, shaders, build-system, ci]
superseded_by: null
negative_knowledge: true
---

# ADR-0156: Slang-first shader architecture — retiring GLSL, modular device code, and clean-break SPIR-V compilation

- Status: Accepted
- Date: 2026-11-10
- Deciders: Optics Architecture Working Group
- Scope: `libs/flux/include/flux/shaders/`, `libs/flux/src/canvas/shaders/`, `libs/flux/src/effect/shaders/`,
  `libs/flux/src/scene/shaders/`, `libs/prism/src/shaders/`, `examples/flux/shaders/`, `tests/flux/shaders/`,
  `libs/flux/meson.build`, `libs/prism/meson.build`, `examples/flux/meson.build`, `tests/flux/meson.build`,
  `.github/workflows/ci.yml`, `.github/workflows/ci-cross.yml`, `docs/dev/setup.md`.
- Amends: [ADR-0053](0053-shader-embed-fallback.md) (replaces `glslangValidator` with `slangc` for SPIR-V
  bytecode generation; preserves C23 `#embed` and `tools/build/spv2h.py` for binary embedding into C translation units).
- Extends: [ADR-0006](0006-no-runtime-rhi.md) (preserves compile-time SPIR-V Vulkan 1.3 targeting without runtime RHI).

---

## Context and Problem Statement

Optics historically compiled GLSL 460 source files (`.vert`, `.frag`, `.comp`) to Vulkan 1.3 SPIR-V using
`glslangValidator`. While functional during initial bootstrap, GLSL imposes severe architectural liabilities
that actively conflict with Optics' modern C23 engineering standards:

1. **Absence of a Native Module System**: GLSL has no standard module or import model. Sharing code (such as
   distance fields in `flux_sdf.glsl` or color math in `canvas_color.glsl`) relies on preprocessor include path
   concatenation (`-I` flags and `GL_GOOGLE_include_directive`), creating fragile symbol pollution and opaque
   dependency graphs.
2. **Combinatorial `#ifdef` Preprocessor Permutations**: Shader variants (e.g. `EFFECT_STORAGE_RGBA16F`,
   `PRISM_STORAGE_RGBA16F`, analytical vs. approximate curves) require branching preprocessor macros that fragment
   the codebase into unverified permutations.
3. **Impedance Mismatch with Buffer Device Address (BDA) and Bindless**: GLSL's `layout(buffer_reference, std430)`
   requires manual padding hacks (e.g. `uint pad0[2]; /* payload 136 -> 144: glslang 16-aligns the struct */` in
   `liquid_glass.comp`) to match host C structure layouts, creating silent memory corruption hazards across the
   CPU/GPU boundary.
4. **Lack of Language Generics and Interface Polymorphism**: Vector curves, distance field primitives, and physical
   surface BSDFs cannot be expressed as typed abstractions, forcing duplicate code across primitives.

With Slang reaching production stability as the industry-standard next-generation shading language, maintaining
legacy GLSL constitutes architectural debt.

---

## Decision Drivers

1. **Clean-Break Architecture**: Zero backward compatibility, no dual-language harness, and no runtime translation
   shims. The repository transitions completely to Slang.
2. **Strict C23 Host ABI Alignment**: Slang's structured layout rules (`-fvk-use-gl-layout` / explicit struct layout)
   and typed pointers eliminate alignment padding guesswork against C23 `opts` and push-constant descriptors.
3. **First-Class Modularization**: Modern `import` syntax and interface contracts replace textual `#include` hacks.
4. **Forward Compatibility with Differentiable & Compute Pipelines**: Slang natively supports automatic differentiation
   (`[Differentiable]`), generic interface dispatch, and seamless target expansion without restructuring device logic.
5. **Host Embedding Invariance**: The host C translation unit consumption contract established in [ADR-0053](0053-shader-embed-fallback.md)
   (C23 `#embed` with `spv2h.py` header fallback) remains untouched. The input to `#embed` remains binary SPIR-V;
   only the source language and compiler authority change.

---

## Architectural Decisions

### 1. Single Shader Compiler Authority: `slangc`

`slangc` is the sole compiler authority for all device bytecode across Optics. `glslangValidator` is excised from
the repository:

```text
               ┌────────────────────────────────────────────────────────┐
               │                     Build Time                         │
               │                                                        │
               │   src/**/*.slang ───► slangc ───► *.spv                │
               │                                     │                  │
               │                 ┌───────────────────┴─────────────┐    │
               │                 ▼                                 ▼    │
               │        C23 #embed "foo.spv"           spv2h.py    │    │
               │                 │                         │        │
               │                 │                   foo_spv.h     │
               │                 │                         │        │
               └─────────────────┼─────────────────────────┼────────┘
                                 ▼                         ▼
               ┌────────────────────────────────────────────────────────┐
               │                     Host Binary                        │
               │                                                        │
               │   static const uint32_t foo_spirv[] = { ... };         │
               │                        │                               │
               │                        ▼                               │
               │         flux_graphics_pipeline_create()                │
               │         flux_compute_pipeline_create()                 │
               └────────────────────────────────────────────────────────┘
```

The canonical build command for generating Vulkan 1.3 SPIR-V from Slang is:

```bash
slangc <input.slang> \
  -target spirv \
  -profile spirv_1_6 \
  -entry main \
  -stage <vertex|fragment|compute> \
  -I <include_paths> \
  -o <output.spv>
```

### 2. Elimination of GLSL Source Files

All 37 legacy GLSL files across `libs/flux`, `libs/prism`, `examples/flux`, and `tests/flux` are removed.
Staged sources keep their stage suffix and gain a `.slang` extension (`foo.vert` becomes `foo.vert.slang`,
`foo.comp` becomes `foo.comp.slang`); shared modules use `.slang` directly:

- **Shared Mathematical Modules**:
  - `libs/flux/include/flux/shaders/flux_sdf.slang`
  - `libs/flux/include/flux/shaders/flux_vector_eval.slang`
- **Canvas Rasterization Pipelines** (`libs/flux/src/canvas/shaders/`):
  - `canvas_solid.vert.slang`, `canvas_solid.frag.slang`
  - `canvas_gradient.frag.slang`, `canvas_image.frag.slang`, `canvas_glyph.frag.slang`
  - `canvas_sdf.frag.slang`, `canvas_output.vert.slang`, `canvas_output.frag.slang`
  - `canvas_tile_raster.comp.slang`, `canvas_vector.frag.slang`
  - `canvas_color.slang`
- **Effect Compute Pipelines** (`libs/flux/src/effect/shaders/`):
  - `effect_backdrop.comp.slang`, `effect_blur.comp.slang`, `effect_shadow.comp.slang`
- **Scene 3D Pipelines** (`libs/flux/src/scene/shaders/`):
  - `scene_unlit.vert.slang`, `scene_unlit.frag.slang`
  - `scene_phong.vert.slang`, `scene_phong.frag.slang`
  - `scene_unlit_skin.vert.slang`, `scene_phong_skin.vert.slang`
  - `scene_vector.vert.slang`, `scene_vector.frag.slang`
  - `scene_color.slang`
- **Prism Physical Materials** (`libs/prism/src/shaders/`):
  - `liquid_glass.comp.slang`, `acrylic.comp.slang`, `frosted.comp.slang`, `mica.comp.slang`
  - `storage_clear.comp.slang`, `backdrop_frost.comp.slang`, `backdrop_stats.comp.slang`
- **Examples and Test Harnesses**:
  - `examples/flux/shaders/fill.slang`
  - `tests/flux/integration/shaders/sdf_parity.comp.slang`
  - `tests/flux/shaders/{fill_pattern.comp,triangle.vert,triangle.frag,triangle_attr.vert}.slang`

### 3. Native Buffer Device Address and Bindless ABI

In Slang, Buffer Device Address is represented via typed pointers or structured buffer references rather
than ambiguous `layout(buffer_reference)` declarations. Push constants are declared with standard struct
syntax matching C23 memory layout:

```slang
// Canonical Slang Push Constants matching C23 struct
struct CanvasSolidPush {
    uint64_t vertex_buffer_address;
    float4   color;
    float4x4 transform;
};

[[vk::push_constant]]
ConstantBuffer<CanvasSolidPush> push;
```

Bindless textures and storage images are accessed via Slang's resource types (`Texture2D`, `RWTexture2D`)
bound to descriptor set 0, fully consistent with Optics' bindless descriptor heap ([ADR-0003](0003-bindless-handle-packing.md)).

### 4. Build System & CI Integration

1. **Meson Discovery**: `meson.build` probes `slangc` using `find_program('slangc', required: true)`.
2. **CI Toolchain Provisioning**: CI workflows (`ci.yml`, `ci-cross.yml`) install the same pinned official
   `slangc` release on Linux, macOS, and Windows.
3. **No Fallback Compilers**: If `slangc` is absent, the build fails fast. No legacy fallback to `glslang` is permitted.

---

## Consequences

### Positive

- **Type Safety & Maintainability**: Slang provides compile-time type verification, eliminates macro include order bugs,
  and enables modular shader library sharing across `flux` and `prism`.
- **Clean Memory Contracts**: Struct padding and alignment between C23 host code and device code are strictly enforceable.
- **Future-Proofing**: Direct access to Slang's auto-diff capabilities, generics, and multi-backend portability without
  altering repository architecture.

### Negative & Trade-offs

- **External Toolchain Footprint**: `slangc` is a larger binary dependency than `glslangValidator`. Developers must
  have `slangc` installed in their PATH.
- **Migration Surface**: All existing shaders must be translated and regression-tested against golden rasterization targets.

---

## Rejected Alternatives & Negative Knowledge

### 1. Dual GLSL / Slang Compilation Harness
- **Why considered**: Staged rollout where legacy shaders remain GLSL while new shaders use Slang.
- **Why rejected**: Violates the Clean Break policy. Dual toolchains double build-system complexity, permit architectural
  rot, and create confusing dual conventions for contributors.

### 2. Runtime Shader Translation (e.g. Slang runtime / SPIRV-Cross)
- **Why considered**: Allows loading Slang or GLSL source at runtime and dynamically translating.
- **Why rejected**: Strictly prohibited by [ADR-0006](0006-no-runtime-rhi.md). Optics mandates zero runtime compilation
  and deterministic startup. All shaders must be baked at build time.

### 3. Microsoft DXC (DirectX Shader Compiler)
- **Why considered**: Industry standard for HLSL to SPIR-V translation.
- **Why rejected**: DXC does not provide Slang's modern interface polymorphism, modular import system, or auto-differentiation
  capabilities, while carrying a comparable or heavier LLVM toolchain footprint.

---

## Links

- Amends: [ADR-0053](0053-shader-embed-fallback.md)
- Depends on: [ADR-0003](0003-bindless-handle-packing.md), [ADR-0006](0006-no-runtime-rhi.md)
- Index: [ADR Index](index.md)
