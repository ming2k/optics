/*
 * gfx/vulkan.h — raw Vulkan handle accessors and the dynamic-rendering
 * pass descriptor.
 *
 * Include this at the seam where you need to drop down to raw Vulkan:
 * recording your own command-buffer commands, integrating with
 * extensions gfx doesn't wrap, or interoperating with another
 * library that wants a VkDevice.
 *
 * Including this header pulls in <vulkan/vulkan.h>; <optics/gfx/core.h>
 * does not.
 */

#ifndef OPTICS_GFX_VULKAN_H
#define OPTICS_GFX_VULKAN_H

#include <optics/gfx/core.h>
#include <optics/gfx/math.h>
#include <stdint.h>
#include <vulkan/vulkan.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================== */
/*  Raw handle accessors                                              */
/* ================================================================== */

OPTICS_GFX_API VkBuffer optics_gfx_buffer_vk_buffer(const optics_gfx_buffer *b);
OPTICS_GFX_API uint64_t optics_gfx_buffer_device_address(const optics_gfx_buffer *b);

OPTICS_GFX_API VkInstance optics_gfx_device_vk_instance(const optics_gfx_device *d);
OPTICS_GFX_API VkPhysicalDevice optics_gfx_device_vk_physical_device(const optics_gfx_device *d);
OPTICS_GFX_API VkDevice optics_gfx_device_vk_device(const optics_gfx_device *d);
OPTICS_GFX_API VkQueue optics_gfx_device_vk_graphics_queue(const optics_gfx_device *d);
OPTICS_GFX_API uint32_t optics_gfx_device_vk_graphics_family(const optics_gfx_device *d);
OPTICS_GFX_API VkQueue optics_gfx_device_vk_transfer_queue(const optics_gfx_device *d);
OPTICS_GFX_API uint32_t optics_gfx_device_vk_transfer_family(const optics_gfx_device *d);
/* True when the logical device enabled Vulkan's optional largePoints
 * feature. A POINT_LIST shader may write gl_PointSize > 1 only when
 * this returns true; otherwise consumers must clamp it to 1. */
OPTICS_GFX_API bool optics_gfx_device_supports_large_points(const optics_gfx_device *d);
/* The returned cache is shared by gfx's canvas/scene/compute/core pipeline
 * creation. Vulkan requires external synchronisation for every cache access.
 * Callers that pass this handle to Vulkan must hold this lock for the complete
 * Vulkan call; always pair lock/unlock on the same thread. */
OPTICS_GFX_API VkPipelineCache optics_gfx_device_vk_pipeline_cache(const optics_gfx_device *d);
OPTICS_GFX_API void optics_gfx_device_vk_pipeline_cache_lock(optics_gfx_device *d);
OPTICS_GFX_API void optics_gfx_device_vk_pipeline_cache_unlock(optics_gfx_device *d);

OPTICS_GFX_API VkSurfaceKHR optics_gfx_surface_vk_handle(const optics_gfx_surface *s);
OPTICS_GFX_API VkSwapchainKHR optics_gfx_surface_vk_swapchain(const optics_gfx_surface *s);
OPTICS_GFX_API VkFormat optics_gfx_surface_vk_format(const optics_gfx_surface *s);

OPTICS_GFX_API VkCommandBuffer optics_gfx_frame_vk_command_buffer(const optics_gfx_frame *f);
OPTICS_GFX_API VkImage optics_gfx_frame_vk_image(const optics_gfx_frame *f);
OPTICS_GFX_API VkImageView optics_gfx_frame_vk_image_view(const optics_gfx_frame *f);

OPTICS_GFX_API VkImage optics_gfx_image_vk_image(const optics_gfx_image *im);
OPTICS_GFX_API VkImageView optics_gfx_image_vk_image_view(const optics_gfx_image *im);
/* optics_gfx_image_bindless_handle is declared in the bindless section below
 * because it returns optics_gfx_bindless_handle, which is typedef'd there. */

/* Render-target image view: hand this to optics_gfx_pass_attachment.view /
 * optics_gfx_pass_depth_attachment.view. NULL → VK_NULL_HANDLE. */
OPTICS_GFX_API VkImageView optics_gfx_target_vk_view(const optics_gfx_target *t);
OPTICS_GFX_API VkImage optics_gfx_target_vk_image(const optics_gfx_target *t);

/* The transient ring buffer underlying optics_gfx_frame_alloc_transient. */
OPTICS_GFX_API VkBuffer optics_gfx_frame_vk_transient_buffer(const optics_gfx_frame *f);

/* ================================================================== */
/*  Format <-> VkFormat                                               */
/* ================================================================== */

OPTICS_GFX_API VkFormat optics_gfx_format_to_vk(optics_gfx_format f);
OPTICS_GFX_API optics_gfx_format optics_gfx_format_from_vk(VkFormat vf);

/* ================================================================== */
/*  Bindless descriptor heap (device-owned)                           */
/* ================================================================== */

/* Single set on slot 0 with descriptor indexing. Slots are stable for
 * the lifetime of the device until released. */

typedef uint32_t optics_gfx_bindless_handle;
#define OPTICS_GFX_BINDLESS_INVALID UINT32_C(0xffffffff)

OPTICS_GFX_NODISCARD OPTICS_GFX_API optics_gfx_result
optics_gfx_bindless_register_image(optics_gfx_device *d, VkImageView view, VkImageLayout layout,
                                   optics_gfx_bindless_handle *out_handle);

/* Register a storage-image view (writable from shaders). The view's
 * underlying image must have been created with VK_IMAGE_USAGE_STORAGE_BIT
 * and the layout must be one valid for storage access (GENERAL is the
 * common choice). */
OPTICS_GFX_NODISCARD OPTICS_GFX_API optics_gfx_result optics_gfx_bindless_register_storage_image(
    optics_gfx_device *d, VkImageView view, VkImageLayout layout,
    optics_gfx_bindless_handle *out_handle);

OPTICS_GFX_NODISCARD OPTICS_GFX_API optics_gfx_result optics_gfx_bindless_register_sampler(
    optics_gfx_device *d, VkSampler sampler, optics_gfx_bindless_handle *out_handle);

OPTICS_GFX_API void optics_gfx_bindless_release(optics_gfx_device *d, optics_gfx_bindless_handle h);

OPTICS_GFX_API VkDescriptorSet optics_gfx_device_bindless_set(optics_gfx_device *d);
OPTICS_GFX_API VkDescriptorSetLayout optics_gfx_device_bindless_layout(optics_gfx_device *d);

/* Lazily creates and returns the bindless handle of the device-owned
 * default sampler (linear filtering, clamp-to-edge). Idempotent and
 * thread-safe. Compute effects use it to sample their inputs. */
OPTICS_GFX_API optics_gfx_bindless_handle
optics_gfx_device_default_sampler_handle(optics_gfx_device *d);

/* ================================================================== */
/*  One-shot submission (public)                                      */
/* ================================================================== */
/* gfx owns the queues, the transient command pools, and the submit/wait
 * plumbing — but before these existed, every headless use of the effect
 * runtime had to either run inside a frame or hand-roll raw Vulkan for
 * exactly the sequence the library already implements internally
 * (effect.h asked callers to "supply a one-shot command buffer" and to
 * have already completed a vkQueueWaitIdle, while exposing no way to do
 * either through gfx).
 *
 * The two-step form covers multi-pass recording (record several operators,
 * submit once, wait once):
 *
 *   optics_gfx_oneshot_begin(device, &cmd);          // begin recording
 *     optics_gfx_compute_dispatch(...)               // record compute work
 *     ...                                            //   into cmd
 *   optics_gfx_oneshot_submit_and_end(device, cmd);  // submit + wait + recycle
 *
 * optics_gfx_oneshot_run() is the single-recording convenience that wraps the
 * same three steps around a caller callback. All three serialize on the
 * device's queue lock exactly like internal submissions, so they are safe
 * to interleave with frame rendering on the same thread; they are NOT
 * thread-safe against concurrent gfx use of the same device (the normal
 * per-device serialization rule).
 *
 * After submit-and-end returns, everything recorded into `cmd` has
 * completed on the GPU; the command buffer and its pool are recycled by
 * the library. Never submit a cmd buffer obtained here through any other
 * path, and never use it after optics_gfx_oneshot_submit_and_end. */

typedef void (*optics_gfx_oneshot_record_fn)(VkCommandBuffer cmd, void *userdata);

/* Begin a one-shot recording. The returned command buffer is already in
 * VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT begin state, on a transient
 * pool owned by the library. Fails with OPTICS_GFX_ERROR_INVALID_ARGUMENT on
 * null args, OPTICS_GFX_ERROR_BACKEND_FAILURE if the pool/buffer cannot be
 * allocated. */
OPTICS_GFX_NODISCARD OPTICS_GFX_API optics_gfx_result
optics_gfx_oneshot_begin(optics_gfx_device *d, VkCommandBuffer *out_cmd);

/* Submit `cmd` on the device's graphics queue and WAIT for completion
 * (fence with a finite timeout, queue-idle fallback), then recycle the
 * buffer. On return, nothing recorded into `cmd` is pending. Preserves
 * a VK_TIMEOUT as OPTICS_GFX_ERROR_TIMEOUT for the caller. */
OPTICS_GFX_NODISCARD OPTICS_GFX_API optics_gfx_result
optics_gfx_oneshot_submit_and_end(optics_gfx_device *d, VkCommandBuffer cmd);

/* Single-recording convenience: begin, call `record` (non-NULL), submit,
 * wait, recycle. */
OPTICS_GFX_NODISCARD OPTICS_GFX_API optics_gfx_result
optics_gfx_oneshot_run(optics_gfx_device *d, optics_gfx_oneshot_record_fn record, void *userdata);

/* The sampled bindless handle the image was registered into at
 * create time. OPTICS_GFX_BINDLESS_INVALID if the image was created
 * without bindless registration (currently always set). */
OPTICS_GFX_API optics_gfx_bindless_handle
optics_gfx_image_bindless_handle(const optics_gfx_image *im);

/* The storage bindless handle of the image, valid only for images created
 * with optics_gfx_image_create (OPTICS_GFX_IMAGE_USAGE_COMPUTE_WRITE) (OPTICS_GFX_BINDLESS_INVALID
 * otherwise). Pass it to a compute shader that writes the image via a storage binding. */
OPTICS_GFX_API optics_gfx_bindless_handle
optics_gfx_image_bindless_storage_handle(const optics_gfx_image *im);

/* ================================================================== */
/*  Graphics pipeline                                                 */
/* ================================================================== */

typedef struct optics_gfx_graphics_pipeline optics_gfx_graphics_pipeline;

typedef enum optics_gfx_primitive_topology : uint32_t {
    OPTICS_GFX_TOPOLOGY_TRIANGLE_LIST = 0,
    OPTICS_GFX_TOPOLOGY_TRIANGLE_STRIP = 1,
    OPTICS_GFX_TOPOLOGY_LINE_LIST = 2,
    OPTICS_GFX_TOPOLOGY_LINE_STRIP = 3,
    OPTICS_GFX_TOPOLOGY_POINT_LIST = 4,
} optics_gfx_primitive_topology;

typedef enum optics_gfx_cull_mode : uint32_t {
    OPTICS_GFX_CULL_NONE = 0,
    OPTICS_GFX_CULL_BACK = 1,
    OPTICS_GFX_CULL_FRONT = 2,
} optics_gfx_cull_mode;

/* Common blend presets, named for what they do at the surface.
 * NONE       — opaque overwrite (color blend disabled).
 * PREMUL     — Porter-Duff source-over assuming premultiplied src.
 * ADDITIVE   — src + dst (HDR particle-style accumulation). */
typedef enum optics_gfx_blend_preset : uint32_t {
    OPTICS_GFX_BLEND_PRESET_NONE = 0,
    OPTICS_GFX_BLEND_PRESET_PREMUL = 1,
    OPTICS_GFX_BLEND_PRESET_ADDITIVE = 2,
} optics_gfx_blend_preset;

typedef enum optics_gfx_depth_test : uint32_t {
    OPTICS_GFX_DEPTH_NONE = 0,
    OPTICS_GFX_DEPTH_TEST_AND_WRITE = 1,
    OPTICS_GFX_DEPTH_TEST_READONLY = 2,
} optics_gfx_depth_test;

typedef struct optics_gfx_vertex_attribute {
    uint32_t location;
    optics_gfx_format format; /* per-attribute format (e.g. RGBA8_UNORM, R32_SFLOAT) */
    uint32_t offset;
} optics_gfx_vertex_attribute;

typedef struct optics_gfx_vertex_binding {
    uint32_t stride;
    uint32_t attribute_count;
    const optics_gfx_vertex_attribute *attributes;
} optics_gfx_vertex_binding;

typedef struct optics_gfx_graphics_pipeline_desc {
    optics_gfx_struct_type type; /* OPTICS_GFX_TYPE_GRAPHICS_PIPELINE_DESC */
    const void *next;

    /* Shaders. Both required. */
    const uint32_t *vertex_spirv;
    size_t vertex_spirv_word_count;
    const char *vertex_entry_point; /* NULL → "main" */

    const uint32_t *fragment_spirv;
    size_t fragment_spirv_word_count;
    const char *fragment_entry_point; /* NULL → "main" */

    /* Vertex input. NULL ⇒ no vertex input state (caller pulls via
     * buffer-device-address or uses gl_VertexIndex). */
    const optics_gfx_vertex_binding *vertex_binding;

    /* Rasterizer / topology / blend / depth state. */
    optics_gfx_primitive_topology topology;
    optics_gfx_cull_mode cull;
    optics_gfx_blend_preset blend;
    optics_gfx_depth_test depth;

    /* Render-target formats. Must match the pass attachments at draw
     * time. Use OPTICS_GFX_FORMAT_UNDEFINED for `depth_format` when there
     * is no depth attachment. */
    optics_gfx_format color_format;
    optics_gfx_format depth_format;

    /* Push constants reachable from vertex + fragment stages. */
    uint32_t push_constant_bytes;
} optics_gfx_graphics_pipeline_desc;

#define OPTICS_GFX_GRAPHICS_PIPELINE_DESC_INIT {.type = OPTICS_GFX_TYPE_GRAPHICS_PIPELINE_DESC}

OPTICS_GFX_NODISCARD OPTICS_GFX_API optics_gfx_result optics_gfx_graphics_pipeline_create(
    optics_gfx_device *d, const optics_gfx_graphics_pipeline_desc *desc,
    optics_gfx_graphics_pipeline **out);

OPTICS_GFX_NODISCARD OPTICS_GFX_API optics_gfx_graphics_pipeline *
optics_gfx_graphics_pipeline_retain(optics_gfx_graphics_pipeline *p);
/* Drop one owned reference. GPU objects retire after submitted use completes.
 * Keep resources alive through submission; raw command buffers follow the
 * same rule. No caller-side device-idle wait is required after submission. */
OPTICS_GFX_API void optics_gfx_graphics_pipeline_release(optics_gfx_graphics_pipeline *p);

OPTICS_GFX_API VkPipeline
optics_gfx_graphics_pipeline_vk_pipeline(const optics_gfx_graphics_pipeline *p);
OPTICS_GFX_API VkPipelineLayout
optics_gfx_graphics_pipeline_vk_layout(const optics_gfx_graphics_pipeline *p);

/* Bind the pipeline and push `push_bytes` of `push_constants` (may be
 * NULL/0). Records into the frame's command buffer. The device
 * bindless set is bound at slot 0 to match the pipeline layout. */
typedef struct optics_gfx_graphics_pipeline_bind_opts {
    optics_gfx_graphics_pipeline *pipeline;
    const void *push_constants;
    uint32_t push_bytes;
} optics_gfx_graphics_pipeline_bind_opts;

OPTICS_GFX_API void
optics_gfx_graphics_pipeline_bind(optics_gfx_frame *f,
                                  const optics_gfx_graphics_pipeline_bind_opts *opts);

/* ================================================================== */
/*  Sampler                                                           */
/* ================================================================== */

/* Refcounted, device-owned sampler. Auto-registered in the bindless
 * heap on create — read the handle via optics_gfx_sampler_bindless_handle
 * to reference the sampler from any shader. */
typedef struct optics_gfx_sampler optics_gfx_sampler;

typedef enum optics_gfx_filter : uint32_t {
    OPTICS_GFX_FILTER_NEAREST = 0,
    OPTICS_GFX_FILTER_LINEAR = 1,
} optics_gfx_filter;

typedef enum optics_gfx_address_mode : uint32_t {
    OPTICS_GFX_ADDRESS_REPEAT = 0,
    OPTICS_GFX_ADDRESS_CLAMP_TO_EDGE = 1,
    OPTICS_GFX_ADDRESS_MIRRORED_REPEAT = 2,
    OPTICS_GFX_ADDRESS_CLAMP_TO_BORDER = 3,
} optics_gfx_address_mode;

typedef struct optics_gfx_sampler_desc {
    optics_gfx_struct_type type; /* OPTICS_GFX_TYPE_SAMPLER_DESC */
    const void *next;
    optics_gfx_filter min_filter;
    optics_gfx_filter mag_filter;
    optics_gfx_filter mipmap_mode; /* used iff the sampled image has mips */
    optics_gfx_address_mode address_u;
    optics_gfx_address_mode address_v;
    optics_gfx_address_mode address_w;
    float max_anisotropy; /* 1.0 = off; clamped to device cap */
} optics_gfx_sampler_desc;

#define OPTICS_GFX_SAMPLER_DESC_INIT {.type = OPTICS_GFX_TYPE_SAMPLER_DESC}

OPTICS_GFX_NODISCARD OPTICS_GFX_API optics_gfx_result optics_gfx_sampler_create(
    optics_gfx_device *d, const optics_gfx_sampler_desc *desc, optics_gfx_sampler **out);

OPTICS_GFX_NODISCARD OPTICS_GFX_API optics_gfx_sampler *
optics_gfx_sampler_retain(optics_gfx_sampler *s);
/* Release is deferred through the device retire queue: the bindless slot
 * and VkSampler are destroyed only once every in-flight batch that could
 * still carry the slot in its push constants has retired, so callers need
 * no keep-alive window after a draw that sampled through it. */
OPTICS_GFX_API void optics_gfx_sampler_release(optics_gfx_sampler *s);

OPTICS_GFX_API VkSampler optics_gfx_sampler_vk_sampler(const optics_gfx_sampler *s);
OPTICS_GFX_API optics_gfx_bindless_handle
optics_gfx_sampler_bindless_handle(const optics_gfx_sampler *s);

/* ================================================================== */
/*  Dynamic-rendering pass                                            */
/* ================================================================== */

typedef enum optics_gfx_load_op : uint32_t {
    OPTICS_GFX_LOAD_DONT_CARE = 0,
    OPTICS_GFX_LOAD_CLEAR = 1,
    OPTICS_GFX_LOAD_LOAD = 2
} optics_gfx_load_op;
typedef enum optics_gfx_store_op : uint32_t {
    OPTICS_GFX_STORE_DONT_CARE = 0,
    OPTICS_GFX_STORE_STORE = 1
} optics_gfx_store_op;

typedef struct optics_gfx_pass_attachment {
    VkImageView view; /* may be VK_NULL_HANDLE → use frame's swapchain image */
    VkFormat format;
    optics_gfx_load_op load_op;
    optics_gfx_store_op store_op;
    optics_gfx_vec4 clear_color; /* used iff load_op == OPTICS_GFX_LOAD_CLEAR */
    /* When true (and `view` is a multisample image), the pass resolves `view`
     * into the frame's swapchain image at end of rendering — for MSAA canvas
     * output. The multisample colour is not stored. */
    bool resolve_to_surface;
    /* When non-NULL, the pass resolves the multisample `view` into this image
     * view instead of the swapchain — used by canvas target capture
     * (ADR-0017), which resolves into a caller-owned optics_gfx_image. Takes
     * precedence over resolve_to_surface. */
    VkImageView resolve_view;
} optics_gfx_pass_attachment;

typedef struct optics_gfx_pass_depth_attachment {
    VkImageView view; /* peer-supplied depth image view */
    VkFormat format;
    optics_gfx_load_op load_op;
    optics_gfx_store_op store_op;
    float clear_depth;
    uint8_t clear_stencil;
} optics_gfx_pass_depth_attachment;

typedef struct optics_gfx_pass_desc {
    optics_gfx_struct_type type; /* OPTICS_GFX_TYPE_PASS_DESC */
    const void *next;
    uint32_t color_attachment_count;
    const optics_gfx_pass_attachment *color_attachments;
    const optics_gfx_pass_depth_attachment *depth; /* nullable */
    /* Stencil-only attachment (nullable). `view` must include the
     * stencil aspect; `clear_stencil` is the clear value, the depth
     * fields are ignored. Bound pipelines must declare a matching
     * stencilAttachmentFormat. The canvas uses this internally for
     * stencil-then-cover fills (ADR-0014). */
    const optics_gfx_pass_depth_attachment *stencil;
    /* Optional render extent for caller-supplied attachments. Zero selects
     * the frame surface extent. Attachment views must cover this area. */
    uint32_t width;
    uint32_t height;
    /* Optional render-area origin (top-left), in framebuffer pixels, for
     * partial-frame passes. Zero (the default) reproduces the legacy
     * full-extent behaviour. Combined with a non-default load_op this lets a
     * caller restrict the cleared/scissored region to a dirty rectangle while
     * preserving the rest of the image: the dynamic-rendering renderArea
     * governs both the clear rectangle and the rasterizer scissor. */
    int32_t render_offset_x;
    int32_t render_offset_y;
    /* Optional debug label for this pass (ADR-0133). Only consumed when
     * OPTICS_GFX_OBSERVE_PASS_LABELS is requested; nullptr falls back to "pass".
     * Must be a literal or outlive the recording call, since it lands in a
     * command buffer that is submitted after this returns. */
    const char *label;
} optics_gfx_pass_desc;

#define OPTICS_GFX_PASS_DESC_INIT {.type = OPTICS_GFX_TYPE_PASS_DESC}

OPTICS_GFX_API void optics_gfx_frame_begin_pass(optics_gfx_frame *f,
                                                const optics_gfx_pass_desc *desc);
OPTICS_GFX_API void optics_gfx_frame_end_pass(optics_gfx_frame *f);

/* Backend-neutral wrappers for the dynamic viewport and scissor state used by
 * graphics pipelines. The frame must be recording; calls outside that state
 * are ignored. */
typedef struct optics_gfx_frame_set_viewport_opts {
    float x;
    float y;
    float width;
    float height;
    float min_depth;
    float max_depth;
} optics_gfx_frame_set_viewport_opts;

OPTICS_GFX_API void optics_gfx_frame_set_viewport(optics_gfx_frame *f,
                                                  const optics_gfx_frame_set_viewport_opts *opts);

typedef struct optics_gfx_frame_set_scissor_opts {
    int32_t x;
    int32_t y;
    uint32_t width;
    uint32_t height;
} optics_gfx_frame_set_scissor_opts;

OPTICS_GFX_API void optics_gfx_frame_set_scissor(optics_gfx_frame *f,
                                                 const optics_gfx_frame_set_scissor_opts *opts);

/* ================================================================== */
/*  Explicit Pipeline Barriers & Queue Transitions (RFC-0094 / ADR-0087) */
/* ================================================================== */

typedef enum optics_gfx_pipeline_stage : uint32_t {
    OPTICS_GFX_STAGE_TOP_OF_PIPE = 1u << 0,
    OPTICS_GFX_STAGE_DRAW_INDIRECT = 1u << 1,
    OPTICS_GFX_STAGE_VERTEX_INPUT = 1u << 2,
    OPTICS_GFX_STAGE_VERTEX_SHADER = 1u << 3,
    OPTICS_GFX_STAGE_FRAGMENT_SHADER = 1u << 4,
    OPTICS_GFX_STAGE_EARLY_FRAGMENT_TESTS = 1u << 5,
    OPTICS_GFX_STAGE_LATE_FRAGMENT_TESTS = 1u << 6,
    OPTICS_GFX_STAGE_COLOR_ATTACHMENT_OUTPUT = 1u << 7,
    OPTICS_GFX_STAGE_COMPUTE_SHADER = 1u << 8,
    OPTICS_GFX_STAGE_TRANSFER = 1u << 9,
    OPTICS_GFX_STAGE_BOTTOM_OF_PIPE = 1u << 10,
} optics_gfx_pipeline_stage;

typedef enum optics_gfx_access_flags : uint32_t {
    OPTICS_GFX_ACCESS_NONE = 0,
    OPTICS_GFX_ACCESS_INDIRECT_COMMAND_READ = 1u << 0,
    OPTICS_GFX_ACCESS_INDEX_READ = 1u << 1,
    OPTICS_GFX_ACCESS_VERTEX_ATTRIBUTE_READ = 1u << 2,
    OPTICS_GFX_ACCESS_UNIFORM_READ = 1u << 3,
    OPTICS_GFX_ACCESS_SHADER_READ = 1u << 4,
    OPTICS_GFX_ACCESS_SHADER_WRITE = 1u << 5,
    OPTICS_GFX_ACCESS_COLOR_ATTACHMENT_READ = 1u << 6,
    OPTICS_GFX_ACCESS_COLOR_ATTACHMENT_WRITE = 1u << 7,
    OPTICS_GFX_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ = 1u << 8,
    OPTICS_GFX_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE = 1u << 9,
    OPTICS_GFX_ACCESS_TRANSFER_READ = 1u << 10,
    OPTICS_GFX_ACCESS_TRANSFER_WRITE = 1u << 11,
} optics_gfx_access_flags;

#define OPTICS_GFX_QUEUE_FAMILY_IGNORED (~0u)

typedef struct optics_gfx_image_barrier {
    optics_gfx_image *image;
    optics_gfx_pipeline_stage src_stage;
    optics_gfx_pipeline_stage dst_stage;
    optics_gfx_access_flags src_access;
    optics_gfx_access_flags dst_access;
    uint32_t src_queue_family;
    uint32_t dst_queue_family;
} optics_gfx_image_barrier;

typedef struct optics_gfx_frame_pipeline_barrier_opts {
    const optics_gfx_image_barrier *image_barriers;
    uint32_t count;
} optics_gfx_frame_pipeline_barrier_opts;

OPTICS_GFX_API void
optics_gfx_frame_pipeline_barrier(optics_gfx_frame *f,
                                  const optics_gfx_frame_pipeline_barrier_opts *opts);

/* ================================================================== */
/*  Deferred pass registration (ADR-0147)                             */
/* ================================================================== */

/* This API takes a VkCommandBuffer, so it lives here rather than in core.h: a
 * backend-agnostic caller must never see it. Libraries that own compute work
 * and already depend on <optics/gfx/vulkan.h> — material is the one that does — use it to
 * put their dispatches under the frame's plan, so work nothing samples is
 * pruned instead of recorded. */

/* Opaque handle to one registered pass. Callers never dereference it; it exists
 * so a pass can name an input as "the output of that pass" without exposing
 * indices or Vulkan handles. */
typedef struct optics_gfx_deferred_token optics_gfx_deferred_token;

/* Inputs and outputs one registered pass may declare. A material reads a
 * capture plus a blur and writes one slab; two outputs leaves room without
 * inviting a variadic API. */
#define OPTICS_GFX_DEFERRED_MAX_INPUTS 8u
#define OPTICS_GFX_DEFERRED_MAX_OUTPUTS 2u

/* Records the pass's dispatches into a secondary command buffer owned by the
 * frame. Must leave every output readable by a compute sampling stage, and must
 * not end the buffer. */
typedef void (*optics_gfx_deferred_record_fn)(void *params, VkCommandBuffer cmd);

typedef struct optics_gfx_deferred_pass_desc {
    optics_gfx_struct_type type; /* OPTICS_GFX_TYPE_DEFERRED_PASS_DESC */
    const void *next;            /* must be nullptr */

    optics_gfx_deferred_record_fn record;
    /* Copied into the frame's plan arena by gfx before registration returns, so
     * this may point at the caller's stack. gfx owns the copy; a material never
     * sees the arena, which is what keeps `optics_gfx_frame` opaque. */
    const void *params;
    size_t params_size;
    const char *label; /* optional debug label for the frame's pass scope */

    /* Images this pass reads, each with the token of the pass that writes it
     * (nullptr when no pass produces it, e.g. a canvas capture). The token is
     * stated rather than inferred from the image pointer: that is what lets a
     * pass order itself after an unrelated one, and what stops two passes
     * writing the same image from silently claiming each other's output. */
    optics_gfx_image *inputs[OPTICS_GFX_DEFERRED_MAX_INPUTS];
    optics_gfx_deferred_token *input_tokens[OPTICS_GFX_DEFERRED_MAX_INPUTS];
    uint32_t input_count;

    /* Images this pass writes. At least one is required. */
    optics_gfx_image *outputs[OPTICS_GFX_DEFERRED_MAX_OUTPUTS];
    uint32_t output_count;
} optics_gfx_deferred_pass_desc;

#define OPTICS_GFX_DEFERRED_PASS_DESC_INIT {.type = OPTICS_GFX_TYPE_DEFERRED_PASS_DESC}

/* Register a pass at the current pass boundary.
 *
 * It is recorded only if something requires one of its outputs: a
 * optics_gfx_frame_materialize_image call on an output, a downstream pass naming this
 * token as an input, or an explicit optics_gfx_frame_require_pass. A pass nothing
 * requires is pruned at submit and its dispatches never reach the GPU.
 *
 * `out_token` may be nullptr when the caller has no downstream pass to link.
 * A returned token stays valid until the frame is presented.
 *
 * Errors:
 *   OPTICS_GFX_ERROR_INVALID_ARGUMENT — null frame/desc, wrong `type`, non-null
 *     `next`, no record callback, counts above OPTICS_GFX_DEFERRED_MAX_*, no output,
 *     or a null output image.
 *   OPTICS_GFX_ERROR_INVALID_STATE    — frame not recording, or a canvas pass is
 *     active (compute cannot be recorded inside a render pass).
 *   OPTICS_GFX_ERROR_OUT_OF_MEMORY    — the plan arena cannot hold `params_size`.
 *   OPTICS_GFX_ERROR_OUT_OF_RANGE     — the frame's plan is full. */
OPTICS_GFX_NODISCARD OPTICS_GFX_API optics_gfx_result
optics_gfx_frame_register_pass(optics_gfx_frame *f, const optics_gfx_deferred_pass_desc *desc,
                               optics_gfx_deferred_token **out_token);

/* Mark a registered pass's output as consumed. This is the seam for a consumer
 * that is not itself a registered pass — typically the host drawing the output
 * in a canvas pass. Idempotent.
 *
 * Errors:
 *   OPTICS_GFX_ERROR_INVALID_ARGUMENT — null frame/token, or the token does not
 *     belong to this frame's plan.
 *   OPTICS_GFX_ERROR_INVALID_STATE    — frame not recording. */
OPTICS_GFX_NODISCARD OPTICS_GFX_API optics_gfx_result
optics_gfx_frame_require_pass(optics_gfx_frame *f, optics_gfx_deferred_token *token);

/* Fail if any of `count` images is a deferred pass output that has not executed.
 *
 * A recording path that bypasses the plan — a raw `optics_gfx_effect_*` primitive, a
 * custom compute pass that records straight into the command buffer — has no way
 * to know that an image it is about to sample belongs to a pass nobody required.
 * Reading it would silently consume last frame's pixels under this frame's
 * handle. This is the check that makes that a named error instead.
 *
 * Every one-shot effect primitive calls it on its input, so the unmanaged path
 * cannot read a pending image even though it does not participate in pruning.
 *
 * Errors:
 *   OPTICS_GFX_ERROR_INVALID_ARGUMENT — null frame, or null `images` with count > 0.
 *   OPTICS_GFX_ERROR_INVALID_STATE    — an image is a pending deferred output, or the
 *     frame is not recording. */
typedef struct optics_gfx_frame_reject_pending_inputs_opts {
    const optics_gfx_image *const *images;
    uint32_t count;
} optics_gfx_frame_reject_pending_inputs_opts;

OPTICS_GFX_NODISCARD OPTICS_GFX_API optics_gfx_result optics_gfx_frame_reject_pending_inputs(
    optics_gfx_frame *f, const optics_gfx_frame_reject_pending_inputs_opts *opts);

#ifdef __cplusplus
}
#endif

#endif /* OPTICS_GFX_VULKAN_H */
