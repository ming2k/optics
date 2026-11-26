# API Reference

Version-agnostic description of how the current `gfx` public API is shaped.
The installed headers and [symbol reference](symbols.md) are the canonical
lookup surfaces for this checkout.

## Headers

| Header                | Always available | Contents                                                              |
|-----------------------|------------------|-----------------------------------------------------------------------|
| `<optics/gfx.h>`       | Yes              | Umbrella; pulls in every gfx module.                                 |
| `<optics/gfx/core.h>`       | Yes              | Device, surface, frame, buffer, sampled image, allocator, logger, results, version. |
| `<optics/gfx/math.h>`       | Yes              | Vector, matrix, quaternion, colour, arena. No Vulkan dependency.      |
| `<optics/gfx/vulkan.h>`     | Yes              | Raw Vulkan handle accessors, sampler, graphics pipeline, pass.        |
| `<optics/gfx/canvas.h>`     | Yes              | 2D drawing: path, paint, image drawing, canvas.                    |
| `<optics/gfx/canvas_cpu.h>` | Yes              | Headless software (CPU) canvas: `optics_gfx_canvas_create (CPU backend)`, pixel readback. No Vulkan. |
| `<optics/gfx/dmabuf.h>`     | Yes (Linux-only behaviour) | Linux dma-buf import into sampled `optics_gfx_image` objects.            |
| `<optics/gfx/compute.h>`    | Yes              | Compute pipeline + dispatch.                                       |
| `<optics/gfx/effect.h>`     | Yes              | Image-domain effects (blur, shadow). See [effect reference](effect.md). |
| `<optics/text.h>`     | Iff `-Dtext=true`   | Sibling library: shaping, glyph atlas, text measure/draw.          |
| `<optics/ui.h>` | Iff `OPTICS_HAVE_UI` | Immediate-mode UI, layout, widget and snapshot surface. |
| `<optics/window.h>` | Yes | Native window shell, event pump and desktop integration. |
| `<optics/app.h>` | Iff `OPTICS_HAVE_APP` | Application shell joining `window` and `ui`. |
| `<optics/motion.h>` | Iff `OPTICS_HAVE_MOTION` | Header-only closed-form spring and parametric easing vocabulary. |

`<optics/optics.h>` exposes optional layers only when `optics.pc` supplies the
matching `-DOPTICS_HAVE_<MODULE>=1` compile definition. Header presence on the
host is not feature detection; see [ADR-0164](../adr/0164-umbrella-feature-detection-flags.md).

`ui` and `app` require both `text` and `svg`. Disabling either removes the UI
stack from the build rather than leaving unresolved symbols behind. `window`
depends on `gfx` only and is always built.

`<optics/gfx/core.h>` and `<optics/gfx/math.h>` do **not** include `<vulkan/vulkan.h>`.
Include `<optics/gfx/vulkan.h>` at the seam where you hand gfx a `VkSurfaceKHR`
or pull a raw `VkCommandBuffer` back.

## Object model

| Pattern              | Applied to                                                                                  |
|----------------------|---------------------------------------------------------------------------------------------|
| Opaque pointer       | `optics_gfx_device`, `optics_gfx_surface`, `optics_gfx_frame`, `optics_gfx_buffer`, `optics_gfx_target`, `optics_gfx_image`, `optics_gfx_canvas`, `optics_gfx_path`, `optics_gfx_sampler`, `optics_gfx_graphics_pipeline`, `optics_gfx_compute_pipeline`. Never dereferenced by the caller. |
| Atomic refcount      | Every heavy resource above. `_retain` is `[[nodiscard]]` and returns the same pointer; `_release` is null-safe.  |
| Value type           | `optics_gfx_vec2/3/4`, `optics_gfx_mat3x2`, `optics_gfx_mat4`, `optics_gfx_quat`, `optics_gfx_color`, `optics_gfx_point`, `optics_gfx_rect`. Pass by value; live on the stack or in a `optics_gfx_arena`. |
| Arena-owned          | `optics_gfx_path`, `optics_gfx_brush`. Reset/destroy the arena to free; no per-element release.         |

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

When a `_desc` struct holds a pointer to another gfx resource, that
pointer is **retained by the constructor**:

| Desc field                              | Retained? |
|-----------------------------------------|-----------|
| `optics_gfx_canvas_desc.surface`              | Yes       |
| `optics_gfx_surface_desc.vk_surface_khr`      | No (raw Vulkan handle, caller-owned) |
| `optics_gfx_image_desc.initial_data`          | No (copied once at upload time) |
| `optics_gfx_dmabuf_image_desc.planes[].fd`    | Yes, only when `optics_gfx_image_import_dmabuf` returns `OPTICS_GFX_OK` |

The retain happens at `_create`; the matching release happens at the
container's final `_release`. Callers are free to drop their own
reference to the dependency immediately after `_create` returns.

### Per-call references

Pointers passed to a recording function (not stored in a desc) are
**not retained**. The caller must keep the dependency alive at least
until the frame consuming it has been submitted and presented:

| Call                                          | Borrows                |
|-----------------------------------------------|------------------------|
| `optics_gfx_canvas_draw_image(c, image, ...)`       | `image`                |
| `optics_gfx_canvas_draw_image_sampled(c, image, sampler, ...)` | `image`, `sampler` |
| `optics_gfx_canvas_fill_path(c, path, paint)`       | `path`, `paint`        |
| `optics_gfx_canvas_stroke_path(c, path, paint)`     | `path`, `paint`        |
| `optics_gfx_canvas_fill_rect(c, r, paint)`          | `paint`                |
| `optics_gfx_compute_dispatch(cmd, opts)`            | `pipeline`, `push_constants` |

The safest pattern is: keep dependencies retained for the lifetime of
the canvas / scene that uses them, releasing only after
`optics_gfx_device_wait_idle`.

### Arena boundary

`optics_gfx_path` and `optics_gfx_brush` are value-typed objects allocated inside
a `optics_gfx_arena`. Their lifetime is the arena's lifetime; freeing the
arena (`optics_gfx_arena_deinit`) or resetting it (`optics_gfx_arena_reset`)
invalidates every pointer previously returned.

**Passing a stale `optics_gfx_path*` into any canvas call after its arena
has been reset or destroyed is undefined behaviour.** Common safe
pattern: build paths into a frame-local arena, submit the frame, then
reset the arena for the next frame.

## Execution model

gfx supports both immediate target-driven execution and pure CPU command
encoding with immutable publication (ADR-0089 through ADR-0094):

1. **Draw merging** (automatic): consecutive canvas submits with an
   identical pipeline, scissor, and push constants merge into a single
   `vkCmdDraw`, so `recorded_draws <= submit_calls`
   (`optics_gfx_canvas_recorded_draws` / `optics_gfx_canvas_submit_calls` report
   both).
2. **Immutable DisplayLists & Encoder** (ADR-0089, ADR-0090): `optics_gfx_encoder`
   records drawing commands into an owned command stream on CPU with zero
   GPU dependency. `optics_gfx_encoder_finish` transfers buffer ownership to
   `optics_gfx_display_list`. Commands embed deep-copied path segments, inlined
   gradient stops, cloned glyph quads/coverage, and retain GPU image/sampler
   references. Destroying or resetting the encoder leaves published display lists
   fully intact. `optics_gfx_canvas_submit_display_list` submits a published list to
   the active canvas pass.
3. **Algebraic Geometry x Brush Model** (ADR-0091): Primitives are expressed
   strictly as the orthogonal product `optics_gfx_geometry` $\times$ `optics_gfx_brush`
   dispatched via `optics_gfx_canvas_draw_geometry`. Geometries support Rect, RRect,
   G2 continuous curvature Squircle (`optics_gfx_path_add_squircle`), Circle, Line,
   and Path.
4. **Opacity Groups (`save_layer`)** (ADR-0091): `optics_gfx_canvas_save_layer` and
   `optics_gfx_encoder_save_layer` allocate isolated compositing layers. Subtree draws
   render in isolation and are composed back into parent targets with layer
   opacity and premultiplied SRC_OVER upon `restore`, eliminating double-blending
   artifacts on overlapping child elements.
5. **Deferred uploads** (automatic): `optics_gfx_uploads_begin`/`flush`
   accumulate buffer/image copies into one queue submission that is
   ordered before every later batch on the same queue (ADR-0022).
6. **Rust Session Lifetimes** (ADR-0093): `Canvas::begin_session` returns an
   exclusive `CanvasSession<'canvas, 'target, T>` that holds a mutable borrow
   of the target and canvas, statically preventing concurrent mutation,
   presentation, or reference escape during recording. Safe targets implement
   sealed `AsTarget`.

| Layer              | What happens on each call                                                |
|--------------------|--------------------------------------------------------------------------|
| `optics_gfx_encoder_*`   | CPU-only stream recording with owned transitive payload cloning. No GPU device required. |
| `optics_gfx_canvas_draw_geometry` | Direct dispatch of orthogonal Geometry x Brush primitive (ADR-0091). |
| `optics_gfx_canvas_save_layer` | Pushes an isolated layer buffer for group opacity compositing. |
| `optics_gfx_canvas_submit_display_list` | Unpacks and executes an immutable `optics_gfx_display_list`. |
| `optics_gfx_canvas_*`    | Flattens / tessellates geometry, writes vertices into the frame's transient ring, records `vkCmdDraw` against the active pipeline. |
| `optics_gfx_compute_dispatch` | Records `vkCmdBindPipeline` + bindless set + push constants + `vkCmdDispatch`. |
| `optics_gfx_graphics_pipeline_bind` | Records `vkCmdBindPipeline` + bindless set + push constants. |

Pipeline-bound state is cached per-canvas / per-pass so an identical
paint kind drawn N times in a row costs one `vkCmdBindPipeline`; with
unchanged scissor/push constants those draws also merge (see the
execution model above), so a run of like paints is one `vkCmdDraw`.

Recording is bounded by `optics_gfx_surface_begin_frame` → `optics_gfx_frame_submit`.
The frame's command buffer is reset (pool-level) at the start of
every frame, so per-frame allocations and recorded commands do not
accumulate across frames.

### Frame damage

A frame may describe what changed through `optics_gfx_frame_begin_desc.damage`
and `damage_full` (ADR-0155). The two fields are an explicit verdict:

- `damage_full = true` means the whole extent changed or could not be
  localized. `OPTICS_GFX_FRAME_BEGIN_DESC_INIT` defaults to this conservative
  value; a `NULL` desc means the same.
- `damage_full = false` with a non-empty `damage` set is a localized
  change. The array is borrowed only for the `begin_frame` call.
- `damage_full = false` with an empty set is a real no-change frame (pacing
  or capture), not a full repaint.

`optics_gfx_frame_repaint_scope()` returns the pixels the acquired image must
paint, combining the current damage with history the image missed. On a
windowed content-backed surface the engine repairs history internally and
returns only the current damage. The result is copied into the caller's
rectangle buffer; the frame retains no borrow.

`optics_gfx_surface_scanout_damage()` answers a different question: what has the
display not seen since the most recent successful present? It includes
submitted-but-abandoned frames and is independent of the next image
acquired. Use it for KMS/scanout hints, not as a Canvas pass scope.

`optics_gfx_surface_invalidate_damage()` forgets the history after a VT switch or
backend takeover; the next frame repaints in full. Image-history lookup must
key on `optics_gfx_frame_image_index()`, not the frame-in-flight slot returned by
`optics_gfx_frame_index()`.

Multi-pass image dependencies, reverse ROI, forward damage, and target
lifetimes are planned through gfx's RenderPlan compiler (ADR-0089 / ADR-0092),
replacing external graph planning with a single dependency-driven execution path.

## Tagged descriptors

Every struct passed by pointer into a `_create` function begins with a
`optics_gfx_struct_type type; const void *next;` header (Vulkan sType/pNext
pattern). Each desc ships an `_INIT` macro that emits the correct tag
and zero-initializes the rest:

```c
optics_gfx_device_desc d = OPTICS_GFX_DEVICE_DESC_INIT;
d.log         = optics_gfx_console_logger;
d.validation  = OPTICS_GFX_VALIDATION_AUTO;
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
| Any pre-1.0 release (`0.x.y → 0.x.z`) | May break; called out in release notes | May break; **rebuild required** |
| Minor post-1.0 (`1.N → 1.N+1`)| Yes                        | Yes — new fields only via `next`-chained extension structs; base layouts are frozen at 1.0 |
| Major (`N.0 → N+1.0`)        | May break                  | May break                  |

Pre-1.0, fields can be appended directly to a base desc when it makes
sense. The pre-1.0 SONAME is the **full version string**
(`liboptics_gfx.so.0.0.87`), not `major.minor`: releases in this line have
appended descriptor fields and changed pass behavior on patch bumps, so
`0.0.86` and `0.0.87` do not share a binary contract. A binary built against
one release fails to load the other instead of silently reading a smaller
struct. Recompile against the matching headers when you upgrade.

Post-1.0, base desc layouts are frozen. Library-side additions ride a chained
extension struct accessed through `next`. The library walks that chain looking
for `optics_gfx_struct_type` discriminators it knows; unknown extensions are ignored.
This is the same pattern Vulkan uses for ABI-stable feature growth.

### Semantic device capabilities

Chain `optics_gfx_device_features_desc` to request backend-neutral capabilities.
`required` capabilities participate in physical-device filtering. `optional`
capabilities are enabled when the normally selected device supports their
complete implementation. Query the result with
`optics_gfx_device_enabled_features`.

```c
optics_gfx_device_features_desc features = OPTICS_GFX_DEVICE_FEATURES_DESC_INIT;
features.required = OPTICS_GFX_DEVICE_FEATURE_DMABUF;
features.optional = OPTICS_GFX_DEVICE_FEATURE_DMABUF_SYNC_FILE;

optics_gfx_device_desc device = OPTICS_GFX_DEVICE_DESC_INIT;
device.next = &features;
```

The semantic interface owns the Vulkan extension bundle. Use
`required_device_extensions` only for platform or application extensions that
do not have a gfx capability.

### DRM physical-device constraint

Chain `optics_gfx_device_drm_node_desc` to `optics_gfx_device_desc.next` to require the
Vulkan physical device associated with a Linux DRM primary or render node:

```c
optics_gfx_device_drm_node_desc drm = OPTICS_GFX_DEVICE_DRM_NODE_DESC_INIT;
drm.drm_major = 226;
drm.drm_minor = 1;

optics_gfx_device_desc device = OPTICS_GFX_DEVICE_DESC_INIT;
device.next = &drm;
device.headless = true;
```

gfx compares the pair with the primary and render identities reported by
`VK_EXT_physical_device_drm` before applying its normal feature checks and
device scoring. Missing extension support or no exact match returns
`OPTICS_GFX_ERROR_UNSUPPORTED`; selection never falls back to another GPU.
`optics_gfx_device_get_drm_identity` returns the selected device's available primary
and render identities without requiring raw Vulkan access.

## dma-buf Import

`<optics/gfx/dmabuf.h>` ships with core (unconditionally installed; ADR-0052
moved it out from behind the canvas gate). It imports a Linux dma-buf
file descriptor as a sampled `optics_gfx_image` for canvas draws.

| Symbol | Purpose |
|--------|---------|
| `OPTICS_GFX_DMABUF_MAX_PLANES` | Maximum plane slots in `optics_gfx_dmabuf_image_desc`; currently `4`. |
| `optics_gfx_dmabuf_plane` | One plane: `fd`, `offset`, and `stride`. |
| `optics_gfx_dmabuf_image_desc` | Import descriptor tagged with `OPTICS_GFX_TYPE_DMABUF_IMAGE_DESC`. |
| `optics_gfx_image_import_dmabuf` | Creates a refcounted `optics_gfx_image` from a dma-buf. |
| `optics_gfx_canvas_wait_dmabuf_acquire` | Waits a new producer sync-file before the current frame samples an already-imported dma-buf image. |
| `optics_gfx_dmabuf_supported` | Returns whether the complete dma-buf capability is enabled. |
| `optics_gfx_dmabuf_sync_supported` | Returns whether acquire sync-file fds can be imported. |

Request dma-buf import and optional sync-file interoperability through the
semantic device feature extension:

```c
optics_gfx_device_features_desc features = OPTICS_GFX_DEVICE_FEATURES_DESC_INIT;
features.required = OPTICS_GFX_DEVICE_FEATURE_DMABUF;
features.optional = OPTICS_GFX_DEVICE_FEATURE_DMABUF_SYNC_FILE;
```

When `OPTICS_GFX_DEVICE_FEATURE_DMABUF_SYNC_FILE` is enabled, `acquire_sync_fd` is
imported as a Linux sync_file fence and waited before gfx samples the image.

`optics_gfx_image_import_dmabuf` currently accepts single-plane, sampled color
formats only. It validates the exact DRM format modifier against the selected
physical device, verifies that the format/modifier is externally importable,
and acquires image ownership from `VK_QUEUE_FAMILY_FOREIGN_EXT` before
creating the bindless view. Multi-plane YCbCr imports are not implemented and
return `OPTICS_GFX_ERROR_UNSUPPORTED`.

`optics_gfx_dmabuf_format_modifiers` returns only modifiers proven sampleable and
externally importable. Callers must not add an unreported
`DRM_FORMAT_MOD_LINEAR` fallback.

On `OPTICS_GFX_OK`, gfx owns and closes the file descriptors in
`optics_gfx_dmabuf_image_desc.planes` and `acquire_sync_fd` when provided. On any
error, the caller still owns those file descriptors and must close them. The
imported image follows the normal `optics_gfx_image` lifecycle and is released with
`optics_gfx_image_release`.

If no acquire fence is supplied, the caller must ensure the producer has
completed writes and must keep the producer from writing concurrently while
gfx samples the image.

For a producer that recommits an already-imported dma-buf with a new acquire
fence, call `optics_gfx_canvas_wait_dmabuf_acquire` after `optics_gfx_canvas_begin` and
before the first draw that samples that image. The wait is submitted to the
GPU for the current frame; it does not block the calling CPU thread or require
rebuilding the `optics_gfx_image`. On `OPTICS_GFX_OK`, gfx owns and closes the sync-file
descriptor. On error, the caller retains ownership and must close it.

## Error model

| Channel               | Use                                                                  |
|-----------------------|----------------------------------------------------------------------|
| `optics_gfx_result` return  | Every fallible call returns one. Mark `[[nodiscard]]`.               |
| `optics_gfx_get_last_error` | Thread-local diagnostic with function/file/line/message/backend code. Populated on every non-OK return. |
| `optics_gfx_log_fn`         | Caller-installed logger receives validation messages from the Vulkan layer and library-internal warnings. |

A non-`OPTICS_GFX_OK` return on thread A never affects what thread B reads
from `optics_gfx_get_last_error`.

### `optics_gfx_get_last_error` lifetime

`optics_gfx_get_last_error(out)` **copies** the diagnostic into the
caller-supplied `optics_gfx_error_info`. The struct itself becomes the
caller's; gfx retains no reference to it.

The pointer fields *inside* the copy (`function`, `file`, `message`)
reference strings owned by gfx:

- They are **never heap-allocated for the caller** — never free them.
- They remain valid **until the next fallible gfx call returns a
  non-OK result on the same thread**, at which point they may be
  overwritten in place.

In practice these strings come from `__func__`, `__FILE__`, and
string literals at the failure site, so their storage outlives the
program. The "valid until the next non-OK return" rule is the strict
guarantee; the looser observation should not be relied on.

### Void-returning calls: the sticky pass error

A handful of hot-path draw calls return `void` (`optics_gfx_canvas_draw_*`,
`optics_gfx_canvas_fill_*`). Checking a `optics_gfx_result` per call would cost
more than the call; instead the canvas defers error reporting:

- The **first** error during a canvas pass is recorded (the "sticky"
  pass error); later errors this pass do not overwrite it.
- The error surfaces when the pass completes: `optics_gfx_canvas_end` returns it
  as the pass result (it is `[[nodiscard]]`).
- Between the failing call and pass end, `optics_gfx_get_last_error` already
  carries the diagnostic — a caller that needs the reason inline (not
  just the pass outcome) can read it there.

Rules for callers:

1. Treat `void` draw calls as fallible at the *pass boundary* — check
   what `end_frame_checked`/`end_target_checked` returns; do not assume per-call success.
2. Do not try to "retry after error" mid-pass: the pass is poisoned.
   Build a new frame instead.
3. Error reason for logging: read `optics_gfx_get_last_error` right after the
   pass-end call reports non-OK (it still holds the first error).

Rules for the library:

1. Every `void` draw call may record at most one sticky error; it must
   not abort or reorder rendering on error (the bad call is skipped,
   the pass continues).
2. Sticky state lives on the canvas pass object, never in thread-local
   storage (the pass is single-threaded by contract; `optics_gfx_get_last_error`
   remains thread-local for the general case).
3. Any new fallible-but-`void` API must document which call reports its
   error, in its header comment, before landing.

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
   standard C23 `[[deprecated("use X")]]` attribute and the release notes name
   the replacement.
2. Remove at minor `0.N+1` or later.

A one-cycle deprecation is the floor, not the ceiling. Symbols with
no plausible replacement (e.g. a misnamed accessor with no remaining
callers) may be renamed or removed immediately at a minor bump, with
the removal called out in the release notes. Recent example: the
GPU-spelled `optics_gfx_canvas_begin`/`_end`/`_end_checked` wrappers were
removed outright once the backend-agnostic
`optics_gfx_canvas_begin`/`optics_gfx_canvas_end`/`_end_frame_checked`
pair covered every caller, including the CPU backend.

## Vulkan handle stability

`<optics/gfx/vulkan.h>` accessors (`optics_gfx_device_vk_device`,
`optics_gfx_frame_vk_command_buffer`, `optics_gfx_buffer_vk_buffer`,
`optics_gfx_sampler_vk_sampler`, `optics_gfx_graphics_pipeline_vk_pipeline`, …)
return raw Vulkan types. These follow Vulkan's own stability rules,
not gfx's. A Vulkan 1.3 program built against today's loader
continues to work; gfx does not abstract this away.

## Capability queries

Feature discovery is by query, not by failure.

| Query | Answers |
|-------|---------|
| `optics_gfx_device_get_limits` | Image/framebuffer maxima, alignment requirements, timestamp period and validity, the applied frames-in-flight cap. |
| `optics_gfx_device_supports_image_usage` | Whether a format supports sampled / compute-write use — mirrors `optics_gfx_image_create (OPTICS_GFX_IMAGE_USAGE_COMPUTE_WRITE)`'s acceptance policy exactly (pinned by an integration test asserting query/create agreement). |
| `optics_gfx_device_enabled_features` | Semantic features enabled on the logical device. |
| `optics_gfx_dmabuf_supported` / `optics_gfx_dmabuf_sync_supported` | dma-buf import capability. |

## One-shot submission

`optics_gfx_oneshot_begin` / `optics_gfx_oneshot_submit_and_end` / `optics_gfx_oneshot_run`
(in `<optics/gfx/vulkan.h>`) publish the headless submission path the effect
runtime always assumed: transient pool → one-time-submit begin →
graphics-queue submit with a finite fence wait → recycle. On return from
submit-and-end, nothing recorded is pending. Required reading for
`optics_gfx_effect_promote`, which expects prior GPU quiescence.

## Pipeline release semantics

`*_release` is NOT uniform across resource kinds:

- Retire-queued (safe at any time): images, buffers, meshes, samplers,
  targets — destruction is deferred until in-flight batches complete.
- Pipelines and materials: `optics_gfx_compute_pipeline_release`,
  `optics_gfx_graphics_pipeline_release` and `optics_gfx_material_release` park
  the `VkPipeline` on the device's retire queue, so they are safe to call while
  work that uses it is still in flight (VUID-vkDestroyPipeline-00765).

## Library versioning

Every library in the stack (`liboptics_gfx`, `liboptics_text`,
`liboptics_ui`, `liboptics_window`, `liboptics_app`, and header-only
`<optics/motion.h>`) releases as a unit and shares one version:
`meson.project_version()`. Each library exposes the same four accessors under
its own prefix (`optics_gfx_*`, `optics_ui_*`, `optics_window_*`,
`optics_motion_*`, `optics_text_*`) — one versioning scheme, learn once, apply
to every library the consumer links.

| Macro / accessor                | Returns                                                     |
|---------------------------------|-------------------------------------------------------------|
| `<PREFIX>_VERSION_MAJOR/MINOR/PATCH` | Compile-time integer literals.                         |
| `<PREFIX>_VERSION_NUMBER`       | Packed at compile time: bits 16–23 major, 8–15 minor, 0–7 patch. |
| `<prefix>_version_number()`     | Runtime equivalent.                                         |
| `<prefix>_version_string()`     | `"M.m.p"` string for logging.                               |
| `<prefix>_version_check(M, m, p)` | True iff this library is at least `M.m.p`.                |

The lockstep is **machine-enforced at two levels**. `test_version_lockstep`
compiles every library against version numbers injected by
`meson.project_version()` and exercises the runtime accessors.
`tools/check_versions.py` (`meson test --suite lint`) compares Meson, the Cargo
workspace, the umbrella header, and every public header. Bumping the release
version therefore means touching every surface in one commit — the intended
discipline. Hard-coded version strings still fail the accessor test (scene once
shipped "0.0.29" against 0.0.36 headers; motion sat at 0.0.1 for seven months).

## Related

- [Symbol Reference](symbols.md) for a per-function lookup table of every exported symbol
- [Thread safety](thread-safety.md)
- [Glossary](glossary.md)
- [`docs/explanation/application-architecture.md`](../explanation/application-architecture.md) for the design rationale behind the object model.
