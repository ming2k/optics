# ADR-0006: No runtime RHI; build-time backend selection if ever needed

- Status: Accepted
- Date: 2026-05-19
- Narrowed by: ADR-0019 (a 2D-canvas software/CPU backend behind a
  canvas-scoped vtable). ADR-0019 does not reintroduce a cross-GPU-API RHI —
  the ban below on that shape still stands; see its "Relationship to ADR-0006".

## Context

A recurring question for any graphics library: should it abstract
the GPU API behind a runtime Render Hardware Interface (vtable +
swappable backends — the bgfx / sokol / Unreal pattern), so the
same binary can run on Vulkan, Metal, D3D12, or WebGPU?

The pressure to add one usually comes framed as "long-term
sustainability." This ADR records why that framing is wrong for
flux, and what to do instead if cross-API targets ever become a
real requirement.

The current state of the code:

- The public API speaks Vulkan types at the seams
  (`<flux/vulkan.h>` exports `VkInstance`, `VkDevice`,
  `VkCommandBuffer`, `VkSurfaceKHR`, the bindless descriptor heap,
  the dynamic-rendering pass descriptor).
- `flux_device_create` requires a Vulkan-1.3 physical device and
  fails fast otherwise (`src/core/device.c`).
- Every modern Vulkan-1.3 feature flux relies on — bindless
  descriptor heap, dynamic rendering, `synchronization2`, timeline
  semaphores, buffer device address, `#embed`'d SPIR-V — is in the
  hot path, not behind an opt-in flag.

## Decision

flux does not, and will not, ship a runtime RHI. There is no
`flux_backend_vtable`, no function-pointer indirection on
hot-path calls, no shader-translation pipeline (HLSL → SPIRV-Cross
→ MSL), no lowest-common-denominator API.

If a non-Vulkan target becomes a real, funded requirement, the
answer is **a parallel library build**, not a runtime backend:

```
libflux.so          built against Vulkan (today)
libflux-metal.so    built against Metal     (only if Apple consumer materialises)
libflux-d3d12.so    built against D3D12     (only if Windows-native consumer materialises)
```

Same `<flux/*.h>` public surface, separate implementation tree
behind it, no runtime cost on the existing build. This is the
Skia model.

## Reasoning

### 1. RHI makes a library *less* sustainable, not more

Runtime RHI is usually proposed as a sustainability win
("future-proof against API shifts"). In practice it does the
opposite:

| Cost                | Vulkan-only flux today | Hypothetical RHI flux |
|---------------------|-------------------------|-----------------------|
| Implementation TUs  | 1 per primitive         | N per primitive       |
| Feature ceiling     | Modern Vulkan 1.3       | LCD across all backends |
| Shader pipeline     | One language (GLSL→SPIR-V) | HLSL+SPIRV-Cross+MSL |
| Bug surface         | One driver family       | All driver families   |
| Velocity            | Ship a feature, done    | Ship N times, test N  |

The empirical pattern across multi-backend libraries: backends
declared >> backends actively maintained. The unsustainable
outcome is dead backends rotting in tree.

### 2. The bindless-first identity becomes incoherent

Bindless descriptor heap (ADR-0001 decision #4), dynamic rendering,
push descriptors, mesh shaders, and ray tracing have no direct
analogues across Metal / D3D12 / WebGPU. An RHI either:

- exposes them only on Vulkan (breaking "write once, run everywhere"
  — the entire point of an RHI), or
- restricts the API to what Metal/D3D12/WebGPU all support, which
  forces flux back to per-draw descriptor sets and pre-declared
  render passes (giving up everything ADR-0001 chose).

There is no third option that keeps flux's identity intact.

### 3. The real question is market access, not sustainability

The concrete reasons to want another backend are platform
constraints, not abstract sustainability:

| Target   | Status                                                       |
|----------|--------------------------------------------------------------|
| Linux    | Vulkan native                                                |
| Windows  | Vulkan native                                                |
| Android  | Vulkan native                                                |
| macOS    | MoltenVK (Vulkan-over-Metal) — works, ships in production    |
| iOS      | MoltenVK — works                                             |
| Web      | WebGPU is itself an RHI; wrap it in a separate library above |
| Consoles | Vendor APIs; out of scope for an open-source library         |

For everything except consoles, "no RHI" does not mean "locked
out." It means MoltenVK on Apple (acceptable for almost all use
cases) and a separate wrapping library for Web.

### 4. The competitive position improves by staying focused

A multi-backend flux competes with bgfx (15 years of
cross-backend plumbing) and sokol (minimal-API,
multi-backend-from-day-one). flux has no advantage there.

A Vulkan-1.3-first flux competes with `vk-bootstrap` + raw
Vulkan (low-level, hand-rolled) and offers a higher-level,
opinionated, bindless-aware alternative. That is a defensible
niche.

## Alternatives considered

- **Add RHI now, pre-1.0.** Cheapest moment to do it ABI-wise,
  most expensive moment velocity-wise: every Stage 4–7 feature
  would have shipped 3× slower. With zero current consumers, the
  cost was paid for benefit that may never be needed.
- **Add RHI after 1.0.** ABI break; requires a flux 2.x. Worse
  than the parallel-build option (below) on every axis.
- **Single shared library with compile-time `#ifdef` backends.**
  Pollutes every source file. Tested-once vs tested-thrice
  illusion. Skia tried this early and moved to separate library
  builds for a reason.
- **Parallel library builds (Skia model).** Chosen as the future
  path *iff* a non-Vulkan target ever becomes a funded
  requirement. No work today; door not nailed shut.
- **Wrap WebGPU as the abstraction.** WebGPU is already an RHI.
  If we want WebGPU, wrap it in a sibling library
  (`flux-web`-or-similar) consuming flux's higher-level types
  where possible; do not invert the dependency.

## Consequences

Positive:

- Vulkan-1.3 features remain first-class in the public API.
- One implementation, one driver family, one bug surface.
- Velocity stays high through 1.0.
- Identity is clear: flux is the opinionated bindless-Vulkan
  library, not "yet another portable graphics layer."

Negative:

- Native macOS / iOS targets depend on MoltenVK (third-party,
  Vulkan-over-Metal translation layer). Acceptable today;
  becomes a real cost if MoltenVK is ever abandoned.
- Web is out of reach without a separate library above flux.
- Console ports are out of scope. (Already out of scope for any
  open-source license; not a regression.)

## Revisiting this decision

This ADR is **not** a forever ban on supporting non-Vulkan
targets. It is a ban on the *runtime-vtable* shape of that
support. If/when a non-Vulkan target becomes funded:

1. Write ADR-NNNN describing the target and the consumer demand
   that justifies it.
2. Stand up `libflux-<backend>.so` as a parallel build, sharing
   `<flux/*.h>` and the module structure but implementing
   `src/<backend>/`.
3. Keep `libflux.so` (Vulkan) as the reference implementation
   that pioneers new features first.

Until step 1 happens, the answer is no.
