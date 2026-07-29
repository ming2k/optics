# API Reference

Version-agnostic description of how the current `flux` public API is shaped.
The installed headers and [symbol reference](symbols.md) are the canonical
lookup surfaces for this checkout.

## Headers

| Header                | Always available | Contents                                                              |
|-----------------------|------------------|-----------------------------------------------------------------------|
| `<flux/flux.h>`       | Yes              | Umbrella; pulls in every enabled module.                              |
| `<flux/core.h>`       | Yes              | Device, surface, frame, buffer, allocator, logger, results, version.  |
| `<flux/math.h>`       | Yes              | Vector, matrix, quaternion, colour, arena. No Vulkan dependency.      |
| `<flux/vulkan.h>`     | Yes              | Raw Vulkan handle accessors, sampler, graphics pipeline, pass.        |
| `<flux/canvas.h>`     | Iff `-Dcanvas=true`  | 2D drawing: image, path, paint, canvas.                            |
| `<flux/canvas_cpu.h>` | Iff `-Dcanvas=true`  | Headless software (CPU) canvas: `flux_canvas_create_cpu`, pixel readback. No Vulkan. |
| `<flux/dmabuf.h>`     | Iff `-Dcanvas=true`  | Linux dma-buf import into sampled `flux_image` objects.            |
| `<flux/scene.h>`      | Iff `-Dscene=true`   | 3D primitives: camera, mesh, material, draw.                       |
| `<flux/compute.h>`    | Iff `-Dcompute=true` | Compute pipeline + dispatch.                                       |
| `<flux/effect.h>`     | Iff `-Deffect=true`  | Image-domain effects (blur). See [effect reference](effect.md).    |

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

flux is **immediate-recording** — every public draw call records
directly into the active Vulkan command buffer at the moment it is
called. There is no internal display list, no batching layer, no
deferred submission.

| Layer              | What happens on each call                                                |
|--------------------|--------------------------------------------------------------------------|
| `flux_canvas_*`    | Flattens / tessellates geometry, writes vertices into the frame's transient ring, records `vkCmdDraw` against the active pipeline. |
| `flux_scene_draw_mesh` | Records `vkCmdBindPipeline` + push constants + `vkCmdBindVertexBuffers` + `vkCmdDrawIndexed`. |
| `flux_compute_dispatch` | Records `vkCmdBindPipeline` + bindless set + push constants + `vkCmdDispatch`. |
| `flux_graphics_pipeline_bind` | Records `vkCmdBindPipeline` + bindless set + push constants. |

The pipeline-bound state is cached per-canvas / per-pass so an
identical paint kind drawn N times in a row produces N `vkCmdDraw`
calls but only one `vkCmdBindPipeline`. That's the only batching the
library does.

Recording is bounded by `flux_surface_begin_frame` → `flux_frame_submit`.
The frame's command buffer is reset (pool-level) at the start of
every frame, so per-frame allocations and recorded commands do not
accumulate across frames.

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
This is the same pattern Vulkan uses for ABI-stable feature growth. No public
extension structs are defined in the current pre-1.0 API.

## dma-buf Import

`<flux/dmabuf.h>` is available when the canvas module is enabled. It imports
a Linux dma-buf file descriptor as a sampled `flux_image` for canvas draws.

| Symbol | Purpose |
|--------|---------|
| `FLUX_DMABUF_MAX_PLANES` | Maximum plane slots in `flux_dmabuf_image_desc`; currently `4`. |
| `flux_dmabuf_plane` | One plane: `fd`, `offset`, and `stride`. |
| `flux_dmabuf_image_desc` | Import descriptor tagged with `FLUX_TYPE_DMABUF_IMAGE_DESC`. |
| `flux_image_import_dmabuf` | Creates a refcounted `flux_image` from a dma-buf. |
| `flux_canvas_wait_dmabuf_acquire` | Waits a new producer sync-file before the current frame samples an already-imported dma-buf image. |
| `flux_dmabuf_supported` | Returns whether the device was created with the required dma-buf extensions. |
| `flux_dmabuf_sync_supported` | Returns whether acquire sync-file fds can be imported. |

The device must be created with these Vulkan device extensions in
`flux_device_desc.required_device_extensions`:

```c
static const char *const dmabuf_exts[] = {
    VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
    VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME,
    VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME,
    VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME,
};
```

Add `VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME` when
`flux_dmabuf_image_desc.has_acquire_sync_fd` is true. In that mode,
`acquire_sync_fd` is imported as a Linux sync_file fence and waited before
flux samples the image.

`flux_image_import_dmabuf` currently accepts single-plane, sampled color
formats only. It validates the exact DRM format modifier against the selected
physical device, verifies that the format/modifier is externally importable,
and acquires image ownership from `VK_QUEUE_FAMILY_FOREIGN_EXT` before
creating the bindless view. Multi-plane YCbCr imports are not implemented and
return `FLUX_ERROR_UNSUPPORTED`.

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
   `[[deprecated("use X")]]` attribute and the release notes name the
   replacement.
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
