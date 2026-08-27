# API Reference

Version-agnostic description of how the current `flux` public API is shaped.
The installed headers and [symbol reference](symbols.md) are the canonical
lookup surfaces for this checkout.

## Headers

| Header                | Always available | Contents                                                              |
|-----------------------|------------------|-----------------------------------------------------------------------|
| `<flux/flux.h>`       | Yes              | Umbrella; pulls in every enabled module.                              |
| `<flux/core.h>`       | Yes              | Device, surface, frame, buffer, sampled image, allocator, logger, results, version. |
| `<flux/math.h>`       | Yes              | Vector, matrix, quaternion, colour, arena. No Vulkan dependency.      |
| `<flux/vulkan.h>`     | Yes              | Raw Vulkan handle accessors, sampler, graphics pipeline, pass.        |
| `<flux/canvas.h>`     | Iff `-Dcanvas=true`  | 2D drawing: path, paint, image drawing, canvas.                    |
| `<flux/canvas_cpu.h>` | Iff `-Dcanvas=true`  | Headless software (CPU) canvas: `flux_canvas_create_cpu`, pixel readback. No Vulkan. |
| `<flux/dmabuf.h>`     | Iff `-Dcanvas=true`  | Linux dma-buf import into sampled `flux_image` objects.            |
| `<flux/scene.h>`      | Iff `-Dscene=true`   | 3D primitives: camera, mesh, material, draw.                       |
| `<flux/compute.h>`    | Iff `-Dcompute=true` | Compute pipeline + dispatch.                                       |
| `<flux/effect.h>`     | Iff `-Deffect=true`  | Image-domain effects (blur, shadow). See [effect reference](effect.md). |
| `<flux-text/text.h>`  | Iff `-Dtext=true`    | Sibling library: shaping, glyph atlas, text measure/draw. See [symbols](symbols.md#flux-texth-sibling-library). |
| `<flux-scene-graph/scene-graph.h>` | Iff `-Dscene-graph=true` | Sibling library: glTF/GLB loading, animation, materials. |

`<flux/core.h>` and `<flux/math.h>` do **not** include `<vulkan/vulkan.h>`.
Include `<flux/vulkan.h>` at the seam where you hand flux a `VkSurfaceKHR`
or pull a raw `VkCommandBuffer` back.

## Object model

| Pattern              | Applied to                                                                                  |
|----------------------|---------------------------------------------------------------------------------------------|
| Opaque pointer       | `flux_device`, `flux_surface`, `flux_frame`, `flux_buffer`, `flux_target`, `flux_image`, `flux_canvas`, `flux_path`, `flux_mesh`, `flux_material`, `flux_sampler`, `flux_graphics_pipeline`, `flux_compute_pipeline`. Never dereferenced by the caller. |
| Atomic refcount      | Every heavy resource above. `_retain` is `[[nodiscard]]` and returns the same pointer; `_release` is null-safe.  |
| Value type           | `flux_vec2/3/4`, `flux_mat3x2`, `flux_mat4`, `flux_quat`, `flux_color`, `flux_point`, `flux_rect`, `flux_camera`, `flux_vertex`, `flux_scene_light`. Pass by value; live on the stack or in a `flux_arena`. |
| Arena-owned          | `flux_path`, `flux_paint`. Reset/destroy the arena to free; no per-element release.         |

## Resource lifecycle

### Refcounted resources

Heavy resources are refcounted with atomic counters. The conventions:

- `_create` returns a fresh handle at refcount 1.
- `_retain(handle)` increments the count and returns the same handle.
  Marked `[[nodiscard]]` so the additional reference can't be silently
  discarded.
- `_release(handle)` decrements and destroys when the count hits zero.
  Null-safe.

### Retention by descriptors

When a `_desc` struct holds a pointer to another flux resource, that
pointer is **retained by the constructor**:

| Desc field                              | Retained? |
|-----------------------------------------|-----------|
| `flux_canvas_desc.surface`              | Yes       |
| `flux_surface_desc.vk_surface_khr`      | No (raw Vulkan handle, caller-owned) |
| `flux_image_desc.initial_data`          | No (copied once at upload time) |
| `flux_dmabuf_image_desc.planes[].fd`    | Yes, only when `flux_image_import_dmabuf` returns `FLUX_OK` |
| `flux_mesh_desc.vertices` / `indices`   | No (copied once at upload time) |
| `flux_material_surface_desc.base_color_image` | Yes |
| `flux_material_surface_desc.base_color_sampler` | Yes |

The retain happens at `_create`; the matching release happens at the
container's final `_release`. Callers are free to drop their own
reference to the dependency immediately after `_create` returns.

### Per-call references

Pointers passed to a recording function (not stored in a desc) are
**not retained**. The caller must keep the dependency alive at least
until the frame consuming it has been submitted and presented:

| Call                                          | Borrows                |
|-----------------------------------------------|------------------------|
| `flux_canvas_draw_image(c, image, ...)`       | `image`                |
| `flux_canvas_draw_image_sampled(c, image, sampler, ...)` | `image`, `sampler` |
| `flux_canvas_fill_path(c, path, paint)`       | `path`, `paint`        |
| `flux_canvas_stroke_path(c, path, paint)`     | `path`, `paint`        |
| `flux_canvas_fill_rect(c, r, paint)`          | `paint`                |
| `flux_scene_draw_mesh(f, cam, w, mesh, mat)`  | `mesh`, `material`     |
| `flux_compute_dispatch(cmd, pipe, push, ...)` | `pipe`, `push_constants` |

The safest pattern is: keep dependencies retained for the lifetime of
the canvas / scene that uses them, releasing only after
`flux_device_wait_idle`.

### Arena boundary

`flux_path` and `flux_paint` are value-typed objects allocated inside
a `flux_arena`. Their lifetime is the arena's lifetime; freeing the
arena (`flux_arena_destroy`) or resetting it (`flux_arena_reset`)
invalidates every pointer previously returned.

**Passing a stale `flux_path*` into any canvas call after its arena
has been reset or destroyed is undefined behaviour.** Common safe
pattern: build paths into a frame-local arena, submit the frame, then
reset the arena for the next frame.

## Execution model

flux is **immediate-recording** as its default path — a draw call
tessellates and records into the frame's transient ring right away —
with three explicit batching layers on top:

1. **Draw merging** (automatic): consecutive canvas submits with an
   identical pipeline, scissor, and push constants merge into a single
   `vkCmdDraw`, so `recorded_draws <= submit_calls`
   (`flux_canvas_recorded_draws` / `flux_canvas_submit_calls` report
   both).
2. **Display lists** (opt-in): `flux_canvas_begin_record` /
   `flux_canvas_end_record` capture a subtree's emission into a
   replayable segment; `flux_canvas_replay` re-emits it without
   re-tessellating. lens uses this to skip unchanged subtrees
   (ADR-0030's disabled-subtree-skip decision, delivered).
3. **Deferred uploads** (automatic): `flux_uploads_begin`/`flush`
   accumulate buffer/image copies into one queue submission that is
   ordered before every later batch on the same queue (ADR-0022).

| Layer              | What happens on each call                                                |
|--------------------|--------------------------------------------------------------------------|
| `flux_canvas_*`    | Flattens / tessellates geometry, writes vertices into the frame's transient ring, records `vkCmdDraw` against the active pipeline. |
| `flux_scene_draw_mesh` | Records `vkCmdBindPipeline` + push constants + `vkCmdBindVertexBuffers` + `vkCmdDrawIndexed`. |
| `flux_compute_dispatch` | Records `vkCmdBindPipeline` + bindless set + push constants + `vkCmdDispatch`. |
| `flux_graphics_pipeline_bind` | Records `vkCmdBindPipeline` + bindless set + push constants. |

## Scene-graph content layer

`flux-scene-graph` preserves glTF primitive material indices. C hosts install
an index-aligned material table with `flux_sg_scene_set_materials`; a NULL
`flux_sg_draw_opts.material` selects that table, while a non-NULL value
overrides the complete scene.

The safe Rust binding exposes
`Scene::from_glb_with_materials(device, bytes, MaterialTarget)` and
`Scene::draw_materials`. It supports embedded PNG/JPEG base-color textures,
UV0, `KHR_texture_transform`, `KHR_materials_unlit`, glTF sampler state,
OPAQUE/MASK/BLEND, alpha cutoff, and double-sided materials. Referenced
external images and non-zero texture-coordinate sets return `LoadError`
instead of silently rendering an incomplete material.

Pipeline-bound state is cached per-canvas / per-pass so an identical
paint kind drawn N times in a row costs one `vkCmdBindPipeline`; with
unchanged scissor/push constants those draws also merge (see the
execution model above), so a run of like paints is one `vkCmdDraw`.

Recording is bounded by `flux_surface_begin_frame` → `flux_frame_submit`.
The frame's command buffer is reset (pool-level) at the start of
every frame, so per-frame allocations and recorded commands do not
accumulate across frames.

The Rust [`flux-composition-graph`](composition-graph.md) companion plans
explicit multi-pass image dependencies, reverse ROI, forward damage, and
target lifetimes above this immediate-recording layer. It owns no Flux
resources and does not change the C execution model.

## Tagged descriptors

Every struct passed by pointer into a `_create` function begins with a
`flux_struct_type type; const void *next;` header (Vulkan sType/pNext
pattern). Each desc ships an `_INIT` macro that emits the correct tag
and zero-initializes the rest:

```c
flux_device_desc d = FLUX_DEVICE_DESC_INIT;
d.log         = flux_console_logger;
d.validation  = FLUX_VALIDATION_AUTO;
```

**Always use the `INIT` macro** (or a designated initializer that
zero-fills) to construct a desc. The library treats an unwritten
field as zero, so any future appended field gets a sensible default
when the caller doesn't know about it yet at source level.

### Descriptor ABI policy

Source compatibility and binary compatibility are governed
differently:

| Version transition           | Source compat              | Binary compat              |
|------------------------------|----------------------------|----------------------------|
| Patch (`0.N.x → 0.N.y`)      | Yes                        | Yes                        |
| Minor pre-1.0 (`0.N → 0.N+1`)| May break; called out in release notes | May break; **rebuild required** |
| Minor post-1.0 (`1.N → 1.N+1`)| Yes                        | Yes — new fields only via `next`-chained extension structs; base layouts are frozen at 1.0 |
| Major (`N.0 → N+1.0`)        | May break                  | May break                  |

Pre-1.0, fields can be appended directly to a base desc when it makes
sense. A 0.1 binary linked against a 0.2 `libflux.so` is **not
guaranteed safe**: the library may read past the end of the
caller's smaller struct. Recompile against the matching headers when
you upgrade across a minor bump.

Post-1.0, base desc layouts are frozen. Library-side additions ride a chained
extension struct accessed through `next`. The library walks that chain looking
for `flux_struct_type` discriminators it knows; unknown extensions are ignored.
This is the same pattern Vulkan uses for ABI-stable feature growth.

### Semantic device capabilities

Chain `flux_device_features_desc` to request backend-neutral capabilities.
`required` capabilities participate in physical-device filtering. `optional`
capabilities are enabled when the normally selected device supports their
complete implementation. Query the result with
`flux_device_enabled_features`.

```c
flux_device_features_desc features = FLUX_DEVICE_FEATURES_DESC_INIT;
features.required = FLUX_DEVICE_FEATURE_DMABUF;
features.optional = FLUX_DEVICE_FEATURE_DMABUF_SYNC_FILE;

flux_device_desc device = FLUX_DEVICE_DESC_INIT;
device.next = &features;
```

The semantic interface owns the Vulkan extension bundle. Use
`required_device_extensions` only for platform or application extensions that
do not have a Flux capability.

### DRM physical-device constraint

Chain `flux_device_drm_node_desc` to `flux_device_desc.next` to require the
Vulkan physical device associated with a Linux DRM primary or render node:

```c
flux_device_drm_node_desc drm = FLUX_DEVICE_DRM_NODE_DESC_INIT;
drm.drm_major = 226;
drm.drm_minor = 1;

flux_device_desc device = FLUX_DEVICE_DESC_INIT;
device.next = &drm;
device.headless = true;
```

Flux compares the pair with the primary and render identities reported by
`VK_EXT_physical_device_drm` before applying its normal feature checks and
device scoring. Missing extension support or no exact match returns
`FLUX_ERROR_UNSUPPORTED`; selection never falls back to another GPU.
`flux_device_get_drm_identity` returns the selected device's available primary
and render identities without requiring raw Vulkan access.

## dma-buf Import

`<flux/dmabuf.h>` ships with core (unconditionally installed; ADR-0052
moved it out from behind the canvas gate). It imports a Linux dma-buf
file descriptor as a sampled `flux_image` for canvas draws.

| Symbol | Purpose |
|--------|---------|
| `FLUX_DMABUF_MAX_PLANES` | Maximum plane slots in `flux_dmabuf_image_desc`; currently `4`. |
| `flux_dmabuf_plane` | One plane: `fd`, `offset`, and `stride`. |
| `flux_dmabuf_image_desc` | Import descriptor tagged with `FLUX_TYPE_DMABUF_IMAGE_DESC`. |
| `flux_image_import_dmabuf` | Creates a refcounted `flux_image` from a dma-buf. |
| `flux_canvas_wait_dmabuf_acquire` | Waits a new producer sync-file before the current frame samples an already-imported dma-buf image. |
| `flux_dmabuf_supported` | Returns whether the complete dma-buf capability is enabled. |
| `flux_dmabuf_sync_supported` | Returns whether acquire sync-file fds can be imported. |

Request dma-buf import and optional sync-file interoperability through the
semantic device feature extension:

```c
flux_device_features_desc features = FLUX_DEVICE_FEATURES_DESC_INIT;
features.required = FLUX_DEVICE_FEATURE_DMABUF;
features.optional = FLUX_DEVICE_FEATURE_DMABUF_SYNC_FILE;
```

When `FLUX_DEVICE_FEATURE_DMABUF_SYNC_FILE` is enabled, `acquire_sync_fd` is
imported as a Linux sync_file fence and waited before Flux samples the image.

`flux_image_import_dmabuf` currently accepts single-plane, sampled color
formats only. It validates the exact DRM format modifier against the selected
physical device, verifies that the format/modifier is externally importable,
and acquires image ownership from `VK_QUEUE_FAMILY_FOREIGN_EXT` before
creating the bindless view. Multi-plane YCbCr imports are not implemented and
return `FLUX_ERROR_UNSUPPORTED`.

`flux_dmabuf_format_modifiers` returns only modifiers proven sampleable and
externally importable. Callers must not add an unreported
`DRM_FORMAT_MOD_LINEAR` fallback.

On `FLUX_OK`, flux owns and closes the file descriptors in
`flux_dmabuf_image_desc.planes` and `acquire_sync_fd` when provided. On any
error, the caller still owns those file descriptors and must close them. The
imported image follows the normal `flux_image` lifecycle and is released with
`flux_image_release`.

If no acquire fence is supplied, the caller must ensure the producer has
completed writes and must keep the producer from writing concurrently while
flux samples the image.

For a producer that recommits an already-imported dma-buf with a new acquire
fence, call `flux_canvas_wait_dmabuf_acquire` after `flux_canvas_begin` and
before the first draw that samples that image. The wait is submitted to the
GPU for the current frame; it does not block the calling CPU thread or require
rebuilding the `flux_image`. On `FLUX_OK`, flux owns and closes the sync-file
descriptor. On error, the caller retains ownership and must close it.

## Error model

| Channel               | Use                                                                  |
|-----------------------|----------------------------------------------------------------------|
| `flux_result` return  | Every fallible call returns one. Mark `[[nodiscard]]`.               |
| `flux_get_last_error` | Thread-local diagnostic with function/file/line/message/backend code. Populated on every non-OK return. |
| `flux_log_fn`         | Caller-installed logger receives validation messages from the Vulkan layer and library-internal warnings. |

A non-`FLUX_OK` return on thread A never affects what thread B reads
from `flux_get_last_error`.

### `flux_get_last_error` lifetime

`flux_get_last_error(out)` **copies** the diagnostic into the
caller-supplied `flux_error_info`. The struct itself becomes the
caller's; flux retains no reference to it.

The pointer fields *inside* the copy (`function`, `file`, `message`)
reference strings owned by flux:

- They are **never heap-allocated for the caller** — never free them.
- They remain valid **until the next fallible flux call returns a
  non-OK result on the same thread**, at which point they may be
  overwritten in place.

In practice these strings come from `__func__`, `__FILE__`, and
string literals at the failure site, so their storage outlives the
program. The "valid until the next non-OK return" rule is the strict
guarantee; the looser observation should not be relied on.

## Compatibility surface

The following changes are considered breaking and trigger a minor or
major version bump:

| Change                                                      | Breaking? |
|-------------------------------------------------------------|-----------|
| Renaming a public symbol                                    | Yes       |
| Changing a public function signature                        | Yes       |
| Removing a public symbol                                    | Yes       |
| Reordering enum values                                      | Yes       |
| Restructuring a public struct (field move / union)          | Yes       |
| Adding a field to a tagged-struct descriptor                | No        |
| Adding a new enum value at the end                          | No        |
| Adding a new function                                       | No        |
| Adding a new header                                         | No        |
| Internal refactor without ABI change                        | No        |

### Deprecation

When a public symbol is on the way out, the preferred path is:

1. Mark deprecated at minor `0.N`. The header carries a
   `FLUX_DEPRECATED("use X")` attribute (C23/C++/GNU-fallback spelling,
   defined next to the affected declaration) and the release notes name
   the replacement. Live example: `flux_canvas_begin`/`_end`/`_end_checked`.
2. Remove at minor `0.N+1` or later.

A one-cycle deprecation is the floor, not the ceiling. Symbols with
no plausible replacement (e.g. a misnamed accessor with no remaining
callers) may be renamed immediately at a minor bump, with the rename
called out in the release notes.

## Vulkan handle stability

`<flux/vulkan.h>` accessors (`flux_device_vk_device`,
`flux_frame_vk_command_buffer`, `flux_buffer_vk_buffer`,
`flux_sampler_vk_sampler`, `flux_graphics_pipeline_vk_pipeline`, …)
return raw Vulkan types. These follow Vulkan's own stability rules,
not flux's. A Vulkan 1.3 program built against today's loader
continues to work; flux does not abstract this away.

## Capability queries

Feature discovery is by query, not by failure.

| Query | Answers |
|-------|---------|
| `flux_device_get_limits` | Image/framebuffer maxima, alignment requirements, timestamp period and validity, the applied frames-in-flight cap. |
| `flux_device_supports_image_usage` | Whether a format supports sampled / compute-write use — mirrors `flux_image_create_compute_writable`'s acceptance policy exactly (pinned by an integration test asserting query/create agreement). |
| `flux_device_enabled_features` | Semantic features enabled on the logical device. |
| `flux_dmabuf_supported` / `flux_dmabuf_sync_supported` | dma-buf import capability. |

## One-shot submission

`flux_oneshot_begin` / `flux_oneshot_submit_and_end` / `flux_oneshot_run`
(in `<flux/vulkan.h>`) publish the headless submission path the effect
runtime always assumed: transient pool → one-time-submit begin →
graphics-queue submit with a finite fence wait → recycle. On return from
submit-and-end, nothing recorded is pending. Required reading for
`flux_effect_promote`, which expects prior GPU quiescence.

## Pipeline release semantics

`*_release` is NOT uniform across resource kinds:

- Retire-queued (safe at any time): images, buffers, meshes, samplers,
  targets — destruction is deferred until in-flight batches complete.
- Destroy-inline (caller proves quiescence): `flux_compute_pipeline_release`,
  `flux_graphics_pipeline_release`, `flux_material_release` destroy the
  underlying Vulkan objects immediately (VUID-vkDestroyPipeline-00765).
  The safe-at-any-time spelling for pipelines is
  `flux_pipeline_release_deferred` / `flux_graphics_pipeline_release_deferred`.

## Library versioning

`libflux.so` releases as a single unit. All modules share the same
version string and so-name; independent module versioning is not
supported.

| Macro / accessor              | Returns                                                  |
|-------------------------------|----------------------------------------------------------|
| `FLUX_VERSION_MAJOR/MINOR/PATCH` | Compile-time integer literals.                         |
| `FLUX_VERSION_NUMBER`         | Packed at compile time: bits 16–23 major, 8–15 minor, 0–7 patch. |
| `flux_version_number()`       | Runtime equivalent.                                      |
| `flux_version_string()`       | `"M.m.p"` string for logging.                            |
| `flux_version_check(M, m, p)` | True iff this library is at least `M.m.p`.               |

## Related

- [Symbol Reference](symbols.md) for a per-function lookup table of every exported symbol
- [Thread safety](thread-safety.md)
- [Glossary](glossary.md)
- [`docs/explanation/application-architecture.md`](../explanation/application-architecture.md) for the design rationale behind the object model.
