# ADR-0019: Canvas rendering backend seam + software (CPU) backend

- Status: Accepted
- Date: 2026-07-02
- Relationship: narrows ADR-0006 (No runtime RHI) — see "Relationship to
  ADR-0006" below. Builds on ADR-0004 (paint kind drives pipeline),
  ADR-0014 (stencil-then-cover), ADR-0017 (render-target capture).

## Context

flux's 2D canvas (`libs/flux/src/canvas`) was a monolithic Vulkan
implementation: `canvas.c` and `renderer.c` recorded `vkCmd*` directly,
built `VkPipeline`s, and managed MSAA/stencil `VkImage`s inline. `lens`
already draws only through the backend-neutral `<flux/canvas.h>` verbs
(ADR-0016), so the *front end* was portable, but the *back end* was not
factored out.

Two consumer needs pushed on this:

1. **Headless rendering with no GPU** — CI image diffing, golden tests,
   thumbnailing, and environments without a Vulkan ICD.
2. **A clean internal seam** so the pass/pipeline/draw code is testable and
   the Vulkan specifics live in one place.

## Decision

Introduce an **internal `flux_canvas_backend` vtable** (`src/canvas/backend.h`)
that the canvas front end drives, with two implementations:

- `flux_canvas_backend_vk` (`backend_vk.c`) — the default GPU backend. All
  the previous inline Vulkan (pipeline bind, transient upload + draw, pass
  begin/end, MSAA/stencil targets, barriers, scissor) moved here behind the
  vtable. `struct flux_canvas` now holds **no Vulkan types**; GPU state lives
  in a backend-private struct reached via `backend_data`.
- `flux_canvas_backend_cpu` (`backend_cpu.c`) — a **software rasterizer**:
  4x-supersampled triangle coverage, premultiplied SRC_OVER blending, and
  host ports of the canvas fragment shaders (solid, linear/radial gradient,
  rounded-rect SDF). Renders into a host RGBA framebuffer.

The vtable ops: `canvas_init`/`canvas_destroy`, `begin_pass`/`end_pass`,
`set_scissor`, `bind_program`, `submit`, and an optional `read_pixels`
snapshot (CPU implements it; GPU leaves it NULL).

Public surface (Skia SkSurface-style factory selecting the backend):

- `flux_canvas_desc` gains `backend` (`AUTO`/`GPU`/`CPU`) + `width`/`height`;
  `flux_canvas_create` dispatches. `AUTO` = GPU when a surface is set, else CPU.
- Unified pass bracket `flux_canvas_begin_frame(c, frame_or_null, clear)` /
  `flux_canvas_end_frame(c)`, and a polymorphic `flux_canvas_read_pixels`.
- `<flux/canvas_cpu.h>` adds the headless convenience layer
  (`flux_canvas_create_cpu`, `_cpu_begin/_end/_pixels`).
- The pre-existing `flux_canvas_begin/_end` remain as thin wrappers.

**Scope is the 2D canvas only.** `scene`, `compute`, `effect`, and the
`<flux/vulkan.h>` escape hatch remain Vulkan-only and are *not* abstracted.
The CPU backend does not support image or glyph (text) draws — they sample
GPU-resident textures — and has no offscreen `begin_target`.

## Relationship to ADR-0006

ADR-0006 bans a **runtime RHI**: a function-pointer abstraction across GPU
APIs (Vulkan / Metal / D3D12 / WebGPU) with shader translation and a
lowest-common-denominator feature ceiling. This ADR does **not** reintroduce
that, and the reasoning of ADR-0006 is preserved:

- **No cross-GPU-API abstraction.** There is exactly one GPU backend
  (Vulkan). The second backend is a CPU rasterizer, not another GPU API.
  ADR-0006 explicitly endorses the Skia model, and Skia ships precisely this
  split — `SkSurfaces::Raster` (CPU) alongside the GPU backend.
- **No shader-translation pipeline / no LCD ceiling.** GLSL→SPIR-V stays the
  only shader path; the CPU backend re-implements the five canvas shaders by
  hand. Vulkan 1.3 bindless / dynamic rendering / BDA remain first-class on
  the GPU path.
- **Indirection is confined to the 2D canvas draw layer**, not the device,
  bindless heap, scene, compute, or effect hot paths, and not the
  `<flux/vulkan.h>` seam. `flux_device_create` still requires Vulkan 1.3.

What this ADR *does* revise is the letter of ADR-0006's "there is no
`flux_backend_vtable`, no function-pointer indirection on hot-path calls":
there is now such a vtable, scoped to the canvas, and `submit` is a
per-batch indirect call. The measured cost is one predictable indirect
call per triangle batch (not per vertex/fragment); on the GPU path the draw
itself dominates. This is judged an acceptable, contained trade for the
testability and the headless-CPU capability.

Per ADR-0006's "Revisiting this decision" process, this ADR records the
narrowed exception. ADR-0006 gains a cross-reference; it is not superseded —
its ban on a *cross-GPU-API* runtime RHI stands.

## Alternatives considered

- **Keep the canvas Vulkan-monolithic; add a separate `libflux-cpu`.** The
  ADR-0006 parallel-build model. Rejected for the CPU case: the canvas front
  end (paint/path/tessellation/push assembly) is already backend-neutral and
  large; duplicating it into a parallel library to swap only the ~5
  draw/pass chokepoints is far more code and drift than one internal vtable.
  The parallel-build model remains the answer for a second *GPU* API.
- **`#ifdef` backends in the canvas sources.** Rejected — pollutes every file;
  same objection ADR-0006 raises.
- **CPU backend that reuses `lens`'s software path.** None exists; lens is
  headless-layout only and emits canvas verbs.

## Consequences

Positive:

- Headless, GPU-free 2D rendering (`flux_canvas_create_cpu` +
  `read_pixels`), usable from C and the `flux` Rust crate.
- The Vulkan canvas code is isolated behind a vtable, `struct flux_canvas`
  is Vulkan-free, and the pass/draw path is unit-testable without a device
  (`tests/flux/unit/test_canvas_cpu.c`).
- Backend-agnostic drawing: the same `fill_*`/`path`/`clip` code runs on GPU
  or CPU; only create + (optional) frame + readout differ.

Negative:

- One vtable indirection per triangle batch on the canvas draw path.
- Two implementations of the canvas draw/pass chokepoints to keep in sync;
  the CPU shader ports must track the GLSL shaders.
- CPU backend is vector-only (no image/glyph/text, no offscreen target) until
  a CPU texture/atlas path is added.

## Revisiting this decision

If a CPU **text** path is needed, add a host-side glyph atlas so the CPU
backend can service `draw_glyph_run`/`draw_image`. If a second **GPU** API is
ever funded, ADR-0006's parallel-build model — not this vtable — is still the
route.

## Amendment (2026-08-10): host-atlas lifetime + generation check

The host coverage buffer the CPU backend samples is borrowed by pointer,
never copied, and display-list segments bake glyph UVs against it. Two
rules now keep that borrow sound:

- The buffer must outlive every segment that recorded a run against it
  (release the segments, or destroy the canvas, before the producer frees
  or reallocates the buffer).
- A producer that rearranges texels in place (flux_text's `atlas_clear`)
  tags each run with a content generation via
  `flux_glyph_run_host_atlas_desc`; the canvas remembers the newest
  generation seen per buffer and `flux_canvas_replay` refuses a segment
  recorded against an older one (false → the caller re-emits and
  re-records) instead of silently sampling moved texels. Covered by
  `tests/flux/unit/test_canvas_text_atlas.c`.
