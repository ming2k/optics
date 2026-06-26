# Roadmap

Live tracker for the development stages whose design tenets are
fixed in [ADR-0001](../adr/0001-project-foundations.md). Update
when a stage lands, not in advance.

| Stage  | Status   | Ships |
|--------|----------|-------|
| 1      | **Done** | Scaffold: public headers, stub bodies, hello-triangle, smoke test, ADR-0001. `libflux.so` builds and links; every function returns either zeroed values or `FLUX_ERROR_UNSUPPORTED`. |
| 2a     | **Done** | Vulkan 1.3 instance + validation layer wiring + physical-device selection + logical device + queues (graphics + async-transfer-if-available) + pipeline cache. `flux_device_create` returns `FLUX_OK` and hello-triangle reaches device creation cleanly. |
| 2b.1   | **Done** | Surface, swapchain, per-frame sync (binary semaphores + fence), dynamic-rendering pass with sync2 transitions, resize handling, GLFW-windowed hello example presenting an animated clear colour. |
| 2b.2   | **Done** | GLSL→SPIR-V compilation via meson + glslangValidator, C23 `#embed` of `.spv` into the example, raw VkPipeline construction with dynamic-rendering compatibility, real triangle drawn. Vertex data baked into the shader (no vertex buffer); transient ring deferred to 2b.3 where a real consumer needs it. |
| 2b.3   | **Done** | Transient memory ring (host-visible, mapped, per-frame slice, buffer device address). Bindless descriptor heap (sampled image / storage image / sampler / storage buffer; UPDATE_AFTER_BIND + PARTIALLY_BOUND). GPU timestamp query pool with begin/end scopes and collect-prior-frame. Hello example reports live GPU frame time. |
| 3      | **Done** | Math implementations (vec / mat / quat / color / arena) + 50-assertion test_math. Fixed `flux_arena_alloc_aligned` to align against the absolute address. |
| 4.1    | **Done** | Canvas object + state stack + solid-fill pipeline (transient ring + buffer device address) + fill_rect + convex fill_path with curve flattening + path builder (move/line/quad/cubic/close + rect/round_rect/circle). `<flux/flux.h>` propagates module defines to consumers. canvas_hello example. |
| 4.2.1  | **Done** | Stroker: butt/round/square caps; miter/round/bevel joins; miter-limit clamping; closed-contour wrap-around joins. |
| 4.2.2  | **Done** | Ear-clipping tessellator for concave + multi-contour fills; CCW reversal in place; bounded-step guard against self-intersecting input. |
| 4.2.3  | **Done** | Linear + radial gradients via dedicated fragment pipeline; up to 8 colour stops in push constants; premultiplied colour at every stop. |
| 4.2.4  | **Done** | Image upload + draw via bindless `SAMPLED_IMAGE` slot; default sampler created lazily on first image registration; `flux_canvas_draw_image`. |
| 5      | **Done** | flux_mesh (host-staged device-local buffers via one-shot upload), flux_material (unlit; caller supplies color + depth format), flux_scene_draw (indexed draw with MVP + colour push constants). Caller owns the depth image per ADR-0001. scene_cube example renders a spinning indexed cube with depth testing. |
| 6      | **Done** | flux_compute_pipeline_create / dispatch (with VkCommandBuffer for headless support), bindless set bound at slot=0, push constants. Headless example writes a buffer via buffer device address from a compute shader and verifies the result. |
| 7.1    | **Done** | Push-constant size guard: `FLUX_DEVICE_REQUIRED_PUSH_BYTES` checked at device-init, fail-fast on adapters that report only the Vulkan-minimum 128 bytes. |
| 7.2    | **Done** | ADR backfill for Stages 4–6: per-module device state (0002), bindless handle packing (0003), paint-kind pipeline selection (0004), tessellator scope (0005). |
| 7.3    | **Done** | GitHub Actions CI (`.github/workflows/ci.yml`) runs the C suite under plain + ASan/UBSan builds against lavapipe. The two canvas GPU pixel tests that hit the known lavapipe LLVM-JIT crash are surfaced as a warning but allowed to fail without masking real regressions elsewhere in the suite. (The RustBindings CI that used to run here moved to the [flux-rs](https://github.com/ming2k/flux-rs) repository when the bindings were extracted.) |
| 7.4    | **Done** | Hand-rolled GPU slab allocator (ADR-0007) backs every `VkDeviceMemory` — `flux_image`, `flux_mesh`, `flux_transient_ring`, and one-shot staging buffers all sub-allocate from per-`(memory_type, pool_kind)` blocks. `VkPipelineCache` is loaded from disk at `flux_device_create` and persisted at `flux_device_release`. One-shot uploads route through the dedicated transfer queue when available, with explicit QFOT release/acquire. A device-wide queue lock makes concurrent `flux_image_create` / `flux_mesh_create` from worker threads safe. |
| 7.5    | **Done** | API polish: locale sweep, paint discriminated union, `flux_format` decoupled from `VkFormat` in scene, init macros, vec4 symmetry, `FLUX_VERSION_NUMBER` fix. |
| 8      | **Done** | Foundational public primitives: `flux_sampler`, `flux_buffer`, `flux_image_update_region`, `flux_graphics_pipeline`. `hello_triangle` no longer drops into raw Vulkan for pipeline construction. Full suite verified on Intel ARL (Mesa 26) under ASan/UBSan, plus a leak-detected soak test. |
| 8.1    | **Done** | Image-effect module (ADR-0008): `flux_effect_blur` (separable Gaussian, compute-backed, transient-pool outputs) and `flux_effect_promote` (synchronous copy of a transient into a caller-owned `flux_image`). Adds public `flux_bindless_register_storage_image`, `flux_image_vk_image`, `flux_image_vk_image_view`, `flux_image_bindless_handle`; extends `flux_image` with a storage bindless slot and tracked layout. New integration test asserts edge softening, sigma=0 identity, promote round-trip. |
| 8.2    | **Done** | Hole-bridging tessellator (ADR-0011): CW-in-CCW contours connected via bridge edges and ear-clipped in a single pass. No stencil buffer or multi-pass draw required. Path builder grows dynamically from arena (no fixed 1024-segment cap). Vendor detection (NVIDIA/AMD/Intel/Apple), `bufferImageGranularity` cached, `VK_EXT_memory_budget` extension detected and queried via `flux_device_memory_budget`. Allocator block reclamation via `flux_vk_allocator_reclaim`. |
| 9      | Partial  | Reference-doc coverage of every exported symbol: **done** — [docs/reference/symbols.md](../reference/symbols.md) covers all 191 exported functions (verified against `nm -D` on the built library). HDR-surface end-to-end test (rendered + presented): pending; needs a real HDR display. |
| 10     | Pending  | 1.0.0 tag, first downstream consumer. |

## Criteria for 1.0

The 1.0 cut is gated on all of the following. Meeting any one is not
enough.

- ~~`flux_graphics_pipeline`, `flux_buffer`, and `flux_sampler` are
  public.~~ **Done.** Consumers no longer need to drop into raw Vulkan
  for pipeline / buffer / sampler work.
- ~~`flux_material_kind` does not expose unimplemented values.~~
  **Done.** `FLUX_MATERIAL_PHONG` was removed from the public enum at
  7.5; the lighting pipeline shipped in 0.0.8 (ADR-0012) and the value
  is back, implemented and presented on a live compositor.
- ~~Test suite passes on real GPU hardware, not just lavapipe.~~
  **Done on Intel ARL (Mesa 26).** Still unverified on NVIDIA / AMD.
- An HDR-surface path that has been verified rendered and presented
  on a real HDR display. *Status 2026-06-13: blocked on hardware —
  neither attached display (eDP-1 panel, HDMI-A-1 Redmi monitor)
  advertises HDR static metadata or extended colorimetry in its
  EDID.*
- At least one external consumer has built against a tagged release
  and reported back. *Status 2026-06-13: both downstream sibling
  projects verified against `v0.0.8` — [flux-ui](../../flux-ui)
  builds warning-free and passes its full suite (26/26), and the
  `ass` compositor builds against the (now full-surface) Rust
  bindings. flux-ui additionally **adopted** the 0.0.8 glyph-run API
  (its whole text path now batches through
  `flux_canvas_draw_glyph_run`) and reported back clean: the
  texel-space quad fields mapped onto its glyph cache without
  conversion, the tint and sampler contracts matched the per-glyph
  path it replaced, and on-screen output is pixel-equivalent
  (screenshot-verified on a live compositor). No API deficiencies
  surfaced. A third-party consumer is still the bar this criterion
  means; the sibling adoption is recorded as the strongest locally
  obtainable evidence.*

## Release tags

| Tag      | Date       | Notes                                                                |
|----------|------------|----------------------------------------------------------------------|
| `v0.0.1` | 2026-05-20 | First public release. Stages 1–8 complete; verified on Intel ARL. See [CHANGELOG.md](../../CHANGELOG.md) for this and every entry below. |
| `v0.0.2` | 2026-05-25 | `flux_canvas_draw_image_sampled`; pixel-tolerance curve flattening; complete concave triangulation. |
| `v0.0.3` | 2026-05-25 | Stage 8.1: effect module (blur + promote), storage-image bindless registration, image VK accessors. |
| `v0.0.4` | 2026-05-29 | `flux_canvas_clip_rect`, coverage-glyph draws; stroker/flattener robustness; push budget 144 → 160. |
| `v0.0.5` | 2026-05-29 | `flux.pc` carries `FLUX_HAVE_*` module defines. |
| `v0.0.6` | 2026-06-04 | Linux dma-buf import (`<flux/dmabuf.h>`). |
| `v0.0.7` | 2026-06-10 | Portability hotfix: `c_std=c2x` so meson < 1.4 (Ubuntu 24.04 LTS) configures. |
| `v0.0.8` | 2026-06-13 | Phong materials (ADR-0012), offscreen surfaces (ADR-0013), glyph runs (ADR-0010), stencil-then-cover (ADR-0014); effect device-leak + depth-attachment + tessellator fixes. |

## Known gaps

Open items that do NOT block the current release but consumers
should be aware of:

- **Limited GPU coverage.** The full suite (30 tests, incl. ASAN +
  UBSAN + leak-detected soak) passes on Intel ARL (Mesa 26); 28 of 30
  pass on lavapipe. NVIDIA, AMD, and Windows / macOS remain
  unverified.
- **lavapipe (Mesa 26 + LLVM 22.1) crashes JIT-compiling the canvas
  vertex-pulling shader** (`getMaskedGather` widening SIGSEGV inside
  libLLVM during `vkCreateGraphicsPipelines`), failing the two canvas
  GPU pixel tests there. Driver-side; predates the canvas GPU tests
  that exposed it (verified against pre-stencil commits). Scene,
  compute, and effect GPU paths all pass on lavapipe.
- **HDR-surface path is wired but not exercised end-to-end.** Format
  negotiation works; whether the picked format actually presents
  HDR content correctly hasn't been validated against a real HDR
  display.
- **No downstream consumer has pressure-tested the API.** Blocks 1.0.

## Stage-1 done-when

- [x] `meson setup build -Dexamples=true -Dtests=true` configures clean.
- [x] `meson compile -C build` produces `libflux.so.0.0.1`.
- [x] `meson test -C build` passes.
- [x] `examples/hello_triangle` runs and prints "flux_device_create
       -> FLUX_ERROR_UNSUPPORTED".
- [x] ADR-0001 is written and `docs/adr/index.md` lists it.
- [x] README, CHANGELOG, CONTRIBUTING in place.
