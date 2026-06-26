# Symbol Reference

Lookup table for every exported function, grouped by header. For the
shape of the API (object model, lifecycle, error model), see
[API Reference](api.md); for blocking behavior per call, see
[Thread Safety](thread-safety.md).

Every fallible function returns [`flux_result`](api.md#error-model) and
is marked `[[nodiscard]]`. Functions returning `void` either cannot fail
or report through `flux_get_last_error` as noted.

## `<flux/core.h>`

### Version

| Symbol | Description |
|--------|-------------|
| `flux_version` | Writes the linked library's major, minor, and patch numbers. Any out-pointer may be `NULL`. |
| `flux_version_number` | Returns the packed monotonic version integer (`FLUX_VERSION_NUMBER` of the linked library). |
| `flux_version_check` | Returns `true` if the linked library is at least the given version. |
| `flux_version_string` | Returns the version as a static `"major.minor.patch"` string. Never freed by the caller. |

### Results and diagnostics

| Symbol | Description |
|--------|-------------|
| `flux_result_string` | Returns a static name for a `flux_result` code (`"FLUX_OK"`, …). |
| `flux_get_last_error` | Copies the calling thread's most recent error diagnostic into a caller-supplied `flux_error_info`. String fields are flux-owned and valid until the next non-OK return on the same thread. |
| `flux_console_logger` | Ready-made `flux_log_fn` that prints to `stderr`. Pass as `flux_device_desc.log`. |

### Device

| Symbol | Description |
|--------|-------------|
| `flux_device_create` | Creates the Vulkan instance, picks a physical device, creates the logical device, queues, bindless heap, and pipeline cache. `desc.headless = true` skips all surface and presentation requirements. |
| `flux_device_retain` | Increments the refcount; returns the same handle. |
| `flux_device_release` | Decrements the refcount; destroys the device (persisting the pipeline cache) at zero. Null-safe. |
| `flux_device_wait_idle` | Blocks until every queue on the device is idle. |
| `flux_device_alloc` | Allocates `bytes` through the device's caller-supplied allocator. Use in peer code instead of `malloc`. |
| `flux_device_free` | Frees a pointer obtained from `flux_device_alloc`. |
| `flux_device_memory_budget` | Fills a `flux_memory_budget` snapshot per heap. `has_budget_extension` reports whether `VK_EXT_memory_budget` data is live or totals are estimates. |

### Buffer

| Symbol | Description |
|--------|-------------|
| `flux_buffer_create` | Creates a GPU buffer per `flux_buffer_desc` (usage bits, `GPU_LOCAL` or `HOST_VISIBLE`, optional device address, optional initial upload). |
| `flux_buffer_retain` | Increments the refcount; returns the same handle. |
| `flux_buffer_release` | Decrements the refcount; destroys at zero. Null-safe. |
| `flux_buffer_mapped` | Returns the persistent mapped pointer, or `NULL` for a `FLUX_BUFFER_GPU_LOCAL` buffer. |
| `flux_buffer_size` | Returns the buffer's size in bytes. |

### Surface

| Symbol | Description |
|--------|-------------|
| `flux_surface_create` | Wraps a caller-created `VkSurfaceKHR` in a swapchain with per-frame sync objects. `hdr_preferred` requests an HDR format when available; `vsync` selects FIFO over MAILBOX/IMMEDIATE. A `NULL` `vk_surface_khr` creates an **offscreen** surface instead (ADR-0013): flux-owned RGBA8 images at `width` × `height` (both required non-zero), no window or swapchain, same frame loop. |
| `flux_surface_retain` | Increments the refcount; returns the same handle. |
| `flux_surface_release` | Decrements the refcount; destroys at zero. Null-safe. |
| `flux_surface_resize` | Stalls the device and rebuilds the swapchain (or offscreen images) at the new extent. In-flight `flux_frame` handles from this surface become invalid; an offscreen surface's prior contents are dropped. Returns `FLUX_ERROR_INVALID_ARGUMENT` when either dimension is 0. |
| `flux_surface_get_info` | Fills a `flux_surface_info` (current extent, image count, whether the surface is actually HDR). |
| `flux_surface_read_pixels` | Offscreen surfaces only: waits for the most recently submitted frame and copies it into `dst` as tightly packed RGBA8 (`width * height * 4` bytes minimum). Returns `FLUX_ERROR_UNSUPPORTED` on a windowed surface and `FLUX_ERROR_INVALID_STATE` before the first submitted frame. |

### Frame

| Symbol | Description |
|--------|-------------|
| `flux_surface_begin_frame` | Acquires the next swapchain image and begins the frame's command buffer. `desc.timeout_ns = 0` uses `FLUX_DEFAULT_FRAME_TIMEOUT_NS` (2 s); a hung GPU returns `FLUX_ERROR_TIMEOUT`. Offscreen surfaces skip the acquire; the image is the frame slot's own. |
| `flux_frame_submit` | Ends recording and submits the frame's command buffer to the graphics queue. On an offscreen surface the image is left ready for `flux_surface_read_pixels` instead of presentation. |
| `flux_frame_present` | Queues the frame's swapchain image for presentation. Call after `flux_frame_submit`. On an offscreen surface presents nothing — it completes the frame and advances the ring, so the windowed frame loop runs unchanged. |
| `flux_frame_alloc_transient` | Bump-allocates from the per-frame transient ring (mapped, GPU-visible, recycled after the frames-in-flight window). `alignment` must be a power of two in [1, 256]. Returns `FLUX_ERROR_OUT_OF_RANGE` when the ring is exhausted. |
| `flux_frame_index` | Returns the frame-in-flight slot index of this frame. |

### GPU timestamps

| Symbol | Description |
|--------|-------------|
| `flux_frame_timestamp_begin` | Opens a labeled GPU timestamp scope in the frame's command buffer. |
| `flux_frame_timestamp_end` | Closes the most recently opened timestamp scope. |
| `flux_frame_collect_timestamps` | Copies the resolved timestamps of the most recent completed frame at this slot into a caller buffer; `inout_count` carries capacity in and count out. |

## `<flux/math.h>`

### Vectors

| Symbol | Description |
|--------|-------------|
| `flux_vec2_make`, `flux_vec3_make`, `flux_vec4_make` | Component-wise constructors. |
| `flux_vec2_add`, `flux_vec3_add`, `flux_vec4_add` | Component-wise sum. |
| `flux_vec2_sub`, `flux_vec3_sub`, `flux_vec4_sub` | Component-wise difference. |
| `flux_vec2_scale`, `flux_vec3_scale`, `flux_vec4_scale` | Multiplies every component by a scalar. |
| `flux_vec2_dot`, `flux_vec3_dot`, `flux_vec4_dot` | Dot product. |
| `flux_vec3_cross` | Cross product (3D only). |
| `flux_vec2_length`, `flux_vec3_length`, `flux_vec4_length` | Euclidean length. |
| `flux_vec2_normalize`, `flux_vec3_normalize`, `flux_vec4_normalize` | Unit-length copy; returns the zero vector when the input length is (near) 0. |

### 2D affine matrix (`flux_mat3x2`)

| Symbol | Description |
|--------|-------------|
| `flux_mat3x2_identity` | Identity transform. |
| `flux_mat3x2_translate` | Translation by `(x, y)`. |
| `flux_mat3x2_scale` | Scale by `(x, y)`. |
| `flux_mat3x2_rotate` | Rotation by an angle in radians. |
| `flux_mat3x2_multiply` | Composition `a × b`. |
| `flux_mat3x2_invert` | Inverse transform; a singular input returns identity and sets `FLUX_ERROR_INVALID_ARGUMENT` in the thread-local diagnostic. |
| `flux_mat3x2_is_identity` | Returns `true` when the matrix is exactly the identity. |
| `flux_mat3x2_transform_point` | Applies the transform to a `flux_point`. |
| `flux_mat3x2_transform_rect` | Returns the axis-aligned bounding rect of the transformed corners. |

### 4×4 matrix (`flux_mat4`)

| Symbol | Description |
|--------|-------------|
| `flux_mat4_identity` | Identity matrix. |
| `flux_mat4_translate` | Translation by `(x, y, z)`. |
| `flux_mat4_scale` | Scale by `(x, y, z)`. |
| `flux_mat4_rotation_quat` | Rotation matrix from a unit quaternion. |
| `flux_mat4_multiply` | Matrix product `a × b`. |
| `flux_mat4_transform_vec4` | Matrix-vector product. |
| `flux_mat4_invert` | General inverse; a singular input returns identity and sets `FLUX_ERROR_INVALID_ARGUMENT` in the thread-local diagnostic. |
| `flux_mat4_perspective` | Right-handed perspective projection (vertical FOV in radians, Vulkan depth range). |
| `flux_mat4_orthographic` | Right-handed orthographic projection (Vulkan depth range). |
| `flux_mat4_look_at` | Right-handed view matrix from eye, target, and up vector. |

### Quaternion

| Symbol | Description |
|--------|-------------|
| `flux_quat_identity` | Identity rotation. |
| `flux_quat_axis_angle` | Rotation of `radians` around `axis` (normalized internally). |
| `flux_quat_multiply` | Hamilton product `a × b`. |
| `flux_quat_normalize` | Unit-length copy. |
| `flux_quat_rotate` | Rotates a `flux_vec3` by the quaternion. |
| `flux_quat_slerp` | Spherical linear interpolation from `a` to `b` at parameter `t`. |

### Color

`flux_color` is packed premultiplied BGRA, 8 bits per channel. Decode
only through `flux_color_unpack`; the byte layout is not part of the ABI.

| Symbol | Description |
|--------|-------------|
| `flux_color_rgba` | Packs RGBA components verbatim; the components are assumed to be already premultiplied. |
| `flux_color_rgba_premul` | Packs straight-alpha RGBA components, premultiplying `r`, `g`, `b` by `a` internally. |
| `flux_color_unpack` | Unpacks the four 8-bit components. The only sanctioned decoder. |
| `flux_color_to_linear` | Expands to linear-light `flux_vec4` (sRGB transfer function removed). |
| `flux_color_from_linear` | Packs a linear-light `flux_vec4` back to a `flux_color`. |

### Arena

| Symbol | Description |
|--------|-------------|
| `flux_arena_init` | Initializes a bump arena of `capacity` bytes; `alloc = NULL` uses libc malloc. |
| `flux_arena_destroy` | Frees the arena's backing buffer when the arena owns it. |
| `flux_arena_alloc` | Bump-allocates `bytes`; returns `NULL` when the arena is exhausted. |
| `flux_arena_alloc_aligned` | Bump-allocates with the absolute address aligned to `align` (a power of two). |
| `flux_arena_reset` | Resets `used` to zero; all prior allocations become invalid. |

## `<flux/vulkan.h>`

### Raw handle accessors

Accessors return the live backing Vulkan handle; see
[Vulkan handle stability](api.md#vulkan-handle-stability) for lifetime
rules.

| Symbol | Returns |
|--------|---------|
| `flux_device_vk_instance` | `VkInstance` |
| `flux_device_vk_physical_device` | `VkPhysicalDevice` |
| `flux_device_vk_device` | `VkDevice` |
| `flux_device_vk_graphics_queue` | `VkQueue` (graphics) |
| `flux_device_vk_graphics_family` | Graphics queue family index |
| `flux_device_vk_transfer_queue` | `VkQueue` (dedicated transfer, or the graphics queue when none exists) |
| `flux_device_vk_transfer_family` | Transfer queue family index |
| `flux_device_vk_pipeline_cache` | `VkPipelineCache` (in-memory; cross-session persistence via consumer hooks) |
| `flux_buffer_vk_buffer` | `VkBuffer` |
| `flux_buffer_device_address` | 64-bit buffer device address, or 0 when the buffer was created without `device_address` |
| `flux_surface_vk_handle` | The `VkSurfaceKHR` the surface wraps |
| `flux_surface_vk_swapchain` | `VkSwapchainKHR` (changes across `flux_surface_resize`) |
| `flux_surface_vk_format` | `VkFormat` of the swapchain images |
| `flux_frame_vk_command_buffer` | The frame's recording `VkCommandBuffer` |
| `flux_frame_vk_image` | The acquired swapchain `VkImage` |
| `flux_frame_vk_image_view` | The acquired swapchain `VkImageView` |
| `flux_frame_vk_transient_buffer` | The `VkBuffer` backing `flux_frame_alloc_transient` |
| `flux_image_vk_image` | `VkImage` |
| `flux_image_vk_image_view` | `VkImageView` |

### Format conversion

| Symbol | Description |
|--------|-------------|
| `flux_format_to_vk` | Maps a `flux_format` to the equivalent `VkFormat`. |
| `flux_format_from_vk` | Maps a `VkFormat` back to `flux_format`; unmapped formats return `FLUX_FORMAT_UNDEFINED`. |

### Bindless heap

One device-wide descriptor set bound at `set = 0`; slots are stable
until released. See [ADR-0003](../adr/0003-bindless-handle-packing.md)
for handle packing.

| Symbol | Description |
|--------|-------------|
| `flux_bindless_register_image` | Registers a sampled-image view at a fresh slot; writes the handle. |
| `flux_bindless_register_storage_image` | Registers a storage-image view (image must carry `VK_IMAGE_USAGE_STORAGE_BIT`; layout valid for storage access). |
| `flux_bindless_register_sampler` | Registers a `VkSampler` at a fresh slot. |
| `flux_bindless_release` | Frees a slot for reuse. `FLUX_BINDLESS_INVALID` is a no-op. |
| `flux_device_bindless_set` | Returns the device-wide `VkDescriptorSet`. |
| `flux_device_bindless_layout` | Returns the matching `VkDescriptorSetLayout` for caller-built pipeline layouts. |
| `flux_image_bindless_handle` | Returns the sampled-image handle the image was registered with at create time. |

### Graphics pipeline

| Symbol | Description |
|--------|-------------|
| `flux_graphics_pipeline_create` | Builds a dynamic-rendering `VkPipeline` from SPIR-V vertex + fragment stages, optional vertex input, topology/cull/blend/depth presets, target formats, and a push-constant range. |
| `flux_graphics_pipeline_retain` | Increments the refcount; returns the same handle. |
| `flux_graphics_pipeline_release` | Decrements the refcount; destroys at zero. Null-safe. |
| `flux_graphics_pipeline_bind` | Binds the pipeline into the frame's command buffer, pushes constants, and binds the device bindless set at `set = 0`. |
| `flux_graphics_pipeline_vk_pipeline` | Returns the raw `VkPipeline`. |
| `flux_graphics_pipeline_vk_layout` | Returns the raw `VkPipelineLayout`. |

### Sampler

| Symbol | Description |
|--------|-------------|
| `flux_sampler_create` | Creates a sampler from filter/address/anisotropy settings and auto-registers it in the bindless heap. |
| `flux_sampler_retain` | Increments the refcount; returns the same handle. |
| `flux_sampler_release` | Decrements the refcount; destroys (and releases the bindless slot) at zero. Null-safe. |
| `flux_sampler_vk_sampler` | Returns the raw `VkSampler`. |
| `flux_sampler_bindless_handle` | Returns the sampler's bindless slot for use from shaders. |

### Dynamic-rendering pass

| Symbol | Description |
|--------|-------------|
| `flux_frame_begin_pass` | Begins dynamic rendering with the given color attachments and optional depth and stencil attachments. A `VK_NULL_HANDLE` color view targets the frame's swapchain image. |
| `flux_frame_end_pass` | Ends the current dynamic-rendering pass. |

## `<flux/canvas.h>`

Available iff the library was built with `-Dcanvas=true`.

### Paint

| Symbol | Description |
|--------|-------------|
| `flux_paint_default` | Opaque-black solid paint with default stroke parameters (width 1, miter limit 4, butt cap, miter join, src-over). |
| `flux_paint_solid` | Solid paint in the given color, default stroke parameters. |
| `flux_paint_linear_gradient` | Linear-gradient paint from `from` to `to` (pixel space) with up to `FLUX_GRADIENT_MAX_STOPS` (8) stops. |
| `flux_paint_radial_gradient` | Radial-gradient paint around `center` normalized by `radius` (pixel space), up to 8 stops. |

### Path

Paths are arena-owned; reset or destroy the arena to free them. Mutators
are `void`-returning; segments dropped on arena exhaustion are counted.

| Symbol | Description |
|--------|-------------|
| `flux_path_create` | Allocates an empty path inside `arena`. |
| `flux_path_move_to` | Starts a new contour at `(x, y)`. |
| `flux_path_line_to` | Appends a straight segment. |
| `flux_path_quad_to` | Appends a quadratic Bézier with one control point. |
| `flux_path_cubic_to` | Appends a cubic Bézier with two control points. |
| `flux_path_close` | Closes the current contour back to its start point. |
| `flux_path_add_rect` | Appends a rectangle as a closed contour. |
| `flux_path_add_round_rect` | Appends a rounded rectangle with one corner radius. |
| `flux_path_add_circle` | Appends a circle as a closed contour. |
| `flux_path_dropped_count` | Number of segments rejected because the arena was exhausted. Non-zero means data was silently dropped. |

### Image

| Symbol | Description |
|--------|-------------|
| `flux_image_create` | Creates a sampled GPU image (8-bit color formats), optionally uploading `initial_data`, and registers it in the bindless heap. |
| `flux_image_retain` | Increments the refcount; returns the same handle. |
| `flux_image_release` | Decrements the refcount; destroys at zero. Null-safe. |
| `flux_image_update_region` | Synchronously uploads pixels into an in-bounds sub-region. The bindless handle and view stay valid across the update. |

### Canvas

| Symbol | Description |
|--------|-------------|
| `flux_canvas_create` | Creates the canvas bound to (and retaining) a surface. One canvas per surface. |
| `flux_canvas_destroy` | Destroys the canvas and releases the surface reference. |
| `flux_canvas_begin` | Binds the canvas to a frame; non-`NULL` `clear_color` clears the target, `NULL` loads it. |
| `flux_canvas_end` | Ends the recording session; the canvas detaches from the frame. |
| `flux_canvas_save` | Pushes the current state (transform + clip) onto the state stack. |
| `flux_canvas_restore` | Pops the state stack. |
| `flux_canvas_clip_rect` | Intersects the current clip with a rectangle (scissor). |
| `flux_canvas_translate` | Appends a translation to the current transform. |
| `flux_canvas_scale` | Appends a scale to the current transform. |
| `flux_canvas_rotate` | Appends a rotation (radians) to the current transform. |
| `flux_canvas_transform` | Appends an arbitrary `flux_mat3x2` to the current transform. |
| `flux_canvas_fill_rect` | Fills a rectangle with a paint. |
| `flux_canvas_fill_rect_color` | Fills a rectangle with a solid color, no paint struct needed. |
| `flux_canvas_fill_path` | Fills a path. Concave and multi-contour (holes) tessellate on the CPU; self-intersecting input falls back to GPU stencil-then-cover under the nonzero winding rule (ADR-0014). |
| `flux_canvas_stroke_path` | Strokes a path using the paint's width, cap, join, and miter limit. |
| `flux_canvas_draw_image` | Draws an image into `dst` with the default linear sampler; `optional_paint` tints/blends. |
| `flux_canvas_draw_image_sampled` | Draws an image sampled through a caller-supplied `flux_sampler` (borrowed for the call). |
| `flux_canvas_draw_image_coverage` | Draws an R8 image as alpha coverage multiplied by a premultiplied `tint` (glyph rendering). |
| `flux_canvas_draw_image_coverage_sub` | Coverage draw from a normalized sub-rectangle of a glyph atlas. |
| `flux_canvas_draw_glyph_run` | Draws a pre-shaped glyph run as one batched draw (ADR-0010): each `flux_glyph_quad` is a screen-space rect sampled from a texel sub-rect of the caller-owned R8 atlas, the `.r` channel multiplied by the quad's premultiplied tint. `sampler` is optional (`NULL` = canvas default; pass NEAREST for crisp blits). |
| `flux_canvas_dropped_draws` | Cumulative draws dropped due to transient-ring exhaustion since canvas creation. Non-zero means the ring is too small for the workload. |

## `<flux/dmabuf.h>`

Available iff the library was built with `-Dcanvas=true`. Linux only.

| Symbol | Description |
|--------|-------------|
| `flux_image_import_dmabuf` | Imports a single-plane dma-buf with an explicit DRM format modifier as a sampled `flux_image`. On `FLUX_OK`, flux owns and closes the plane and acquire-sync file descriptors; on error the caller keeps them. `plane_count != 1` returns `FLUX_ERROR_UNSUPPORTED`. |
| `flux_dmabuf_supported` | Returns `true` when the device was created with the external-memory and foreign-queue extensions dma-buf import requires. |
| `flux_dmabuf_sync_supported` | Returns `true` when `acquire_sync_fd` import is available (`VK_KHR_external_semaphore_fd`). |

## `<flux/scene.h>`

Available iff the library was built with `-Dscene=true`.

| Symbol | Description |
|--------|-------------|
| `flux_camera_perspective` | Sets the camera's projection matrix to a perspective projection. |
| `flux_camera_look_at` | Sets the camera's view matrix from eye, target, and up vector. |
| `flux_mesh_create` | Uploads vertices (and optional 32-bit indices) into device-local buffers via a one-shot staging copy. |
| `flux_mesh_retain` | Increments the refcount; returns the same handle. |
| `flux_mesh_release` | Decrements the refcount; destroys at zero. Null-safe. |
| `flux_material_create` | Creates a material pipeline of the given kind (`FLUX_MATERIAL_UNLIT` or `FLUX_MATERIAL_PHONG`) compatible with the stated color and depth target formats. For PHONG, `shininess <= 0` selects the default specular exponent (32) and `specular` 0 disables the highlight. |
| `flux_material_retain` | Increments the refcount; returns the same handle. |
| `flux_material_release` | Decrements the refcount; destroys at zero. Null-safe. |
| `flux_scene_draw_mesh` | Records one mesh + material draw into the frame's active pass. The pass attachments must match the material's formats exactly. No-op when any argument is `NULL`. PHONG materials are lit with `FLUX_SCENE_LIGHT_DEFAULT`. |
| `flux_scene_draw_mesh_lit` | Same as `flux_scene_draw_mesh` with an explicit `flux_scene_light` (`NULL` selects the default). UNLIT materials ignore the light. The light is consumed during the call. |

## `<flux/compute.h>`

Available iff the library was built with `-Dcompute=true`.

| Symbol | Description |
|--------|-------------|
| `flux_compute_pipeline_create` | Builds a compute pipeline from SPIR-V with the device bindless layout at `set = 0` and a push-constant range. |
| `flux_compute_pipeline_retain` | Increments the refcount; returns the same handle. |
| `flux_compute_pipeline_release` | Decrements the refcount; destroys at zero. Null-safe. |
| `flux_compute_pipeline_vk_pipeline` | Returns the raw `VkPipeline`. |
| `flux_compute_pipeline_vk_layout` | Returns the raw `VkPipelineLayout`. |
| `flux_compute_dispatch` | Records bind + push constants + bindless set + `vkCmdDispatch` into a caller-supplied `VkCommandBuffer` (frame-recorded or one-shot). |

## `<flux/effect.h>`

Available iff the library was built with `-Deffect=true` (requires
`-Dcanvas=true -Dcompute=true`). See [Effect Reference](effect.md) for
transient-output lifetime rules.

| Symbol | Description |
|--------|-------------|
| `flux_effect_blur` | Records a separable two-pass Gaussian blur into `cmd`. Sigma is clamped to `[0, FLUX_EFFECT_BLUR_SIGMA_MAX]`; sigma 0 degenerates to a copy. The output image is effect-owned and transient. |
| `flux_effect_promote` | Synchronously copies a transient effect output into a fresh caller-owned `flux_image` with the regular refcounted lifecycle. |
| `flux_effect_reset` | Releases every transient output the effect module holds for the device. Do not call while a command buffer referencing one is in flight. |
