# ADR-0001: Project foundations

- Status: Accepted
- Date: 2026-05-19

## Context

`flux` is a C23 Vulkan-1.3 graphics library. Before any module
shipped, the project needed a small set of structural decisions
fixed in writing so every subsequent design follows them. This ADR
records those decisions and the trade-offs behind them.

The forces that shaped the choices below:

1. **API identity should match how users describe the library.**
   "A graphics library" is one thing. Shipping it as three linked
   libraries pays a tax on every doc and every consumer's link
   line, and the "subset to 2D-only" use case is theoretical.
2. **Lifecycle objects should match observable lifetime.** A
   `context` separate from a `device` is two-step ceremony when
   no realistic scenario keeps one alive without the other.
3. **Modern graphics is compute-heavy.** Post-process, particle,
   culling, ML inference. Compute being a "module" rather than a
   bolt-on changes the pipeline + descriptor model.
4. **Bindless is the modern descriptor model.**
   `VK_EXT_descriptor_indexing` is universal on the Vulkan 1.3
   baseline. Per-draw descriptor sets are legacy.
5. **C23 enables removing build-system layers.** `#embed` makes
   the `.spv → .spv.h` codegen step unnecessary.
6. **Descriptors need a forward-compat story** that doesn't
   require sizing fields or version sentinels. Vulkan's tagged
   `sType`/`pNext` pattern is the idiomatic answer.
7. **Headless is first-class, not bolted on.** CI, offscreen
   rendering, and compute-only workloads all need a no-surface
   path.

## Decision

1. **One library, one umbrella header.** `libflux.so`, `flux.pc`,
   `<flux/flux.h>`. Per-module subset headers
   (`<flux/canvas.h>`, `<flux/scene.h>`, `<flux/compute.h>`)
   remain for compile-time narrowing.
2. **Modules are build-time options, not separate libraries.**
   `-Dcanvas=`, `-Dscene=`, `-Dcompute=`. The `.so` gets the
   trimmed code; consumers get the trimmed headers.
3. **`flux_device` is the only lifecycle object** for
   instance / device / queue / allocator / logger. No separate
   context. A headless variant of the same object covers
   compute, offscreen, and CI.
4. **Bindless descriptor heap on set 0**
   (`VK_EXT_descriptor_indexing`). Per-draw descriptor sets are
   not in the public API. Stable `flux_bindless_handle` indices
   replace per-frame descriptor-set allocation.
5. **Compute pipelines are first-class.** `<flux/compute.h>`
   from day one, sharing the pipeline cache with graphics.
   Push constants for small per-dispatch state; transient
   slices addressed by 64-bit device address for larger state.
6. **C23 features used as intended.** `nullptr`,
   `[[nodiscard]]`, `constexpr` where it earns its keep,
   `#embed` for shaders. No shader codegen step.
7. **Tagged-struct descriptors.** Every `*_desc` opens with
   `flux_struct_type type; const void *next;`. Extension chains
   work like Vulkan `pNext` chains.
8. **Structured diagnostics.** `flux_get_last_error()` returns
   `flux_error_info { code, function, file, line, message,
   backend_code }`. `flux_result_string()` remains for the
   code-only path.
9. **GPU profiling is built in.** `flux_frame_timestamp_begin/_end`
   and `flux_frame_collect_timestamps` ship with the library,
   not as a separate add-on.
10. **HDR + async-transfer + bindless** are standard surface
    options, not afterthought additions.

Modular consumption from upstream Vulkan: use VMA-style helpers
for the GPU allocator (do not reimplement); use Vulkan's
`VkPipelineCache` blob serialization for persistent cache (the
consumer persists; flux provides the in/out hooks).

## Scope boundaries

Out of scope, deliberately:

- Render graph / frame graph. Belongs in a library on top of
  flux.
- Asset loading (glTF, PNG). Caller's problem; flux accepts
  bytes.
- Window / input. Caller's problem; GLFW / SDL / raw platform.
- Text shaping. Belongs in a sibling library wrapping HarfBuzz.
  *(Superseded by [ADR-0015](0015-text-layering.md), then restored by
  [ADR-0016](0016-pure-rhi-and-draw-primitives.md): shaping lives in
  the `flux-text` sibling, which feeds the `flux_canvas_draw_glyph_run`
  primitive. Layer-1 layout remains in `flux-text-layout`, re-parented
  to `flux-text`.)*
- C++ bindings. Anyone who wants C++ writes the wrapper.

## Consequences

Positive:

- Public API matches mental model. No "which `.pc` do I link?"
  question.
- Compute, bindless, headless, HDR are first-class. Library is
  applicable to current-era workloads without bolt-ons.
- Code organization matches the cohesion that exists in
  practice (one library, modules inside).
- C23 actually used; `#embed` removes a build-system layer.

Negative:

- The "single library" decision is hard to walk back. If
  consumer patterns later demand subsetting beyond `-D` flags,
  splitting into multiple libraries is a real cost. Mitigated:
  internal module boundaries (`src/canvas/` vs `src/scene/`)
  are preserved exactly, so a future split is mechanical.
- The bindless-only public API rules out targets that don't
  expose `descriptor_indexing`. That is the deliberate cost of
  a Vulkan-1.3 baseline.

## Alternatives considered

- **Per-draw descriptor sets in the public API.** Familiar to
  anyone coming from Vulkan tutorials, but it locks per-frame
  allocation costs into every consumer and contradicts the
  bindless-first design.
- **Separate `flux_context` and `flux_device`.** Standard in
  some engines, but here the two would always be created and
  destroyed together. Two objects with one lifetime is
  ceremony without benefit.
- **Shader codegen step (`.spv` → `.spv.h`).** Works on any C
  compiler, but adds a meson custom-target per shader and a
  generated header per shader. `#embed` is the C23 answer.
