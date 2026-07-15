/*
 * flux/vulkan.h — raw Vulkan handle accessors and the dynamic-rendering
 * pass descriptor.
 *
 * Include this at the seam where you need to drop down to raw Vulkan:
 * recording your own command-buffer commands, integrating with
 * extensions flux doesn't wrap, or interoperating with another
 * library that wants a VkDevice.
 *
 * Including this header pulls in <vulkan/vulkan.h>; <flux/core.h>
 * does not.
 */

#ifndef FLUX_VULKAN_H
#define FLUX_VULKAN_H

#include <flux/core.h>
#include <flux/math.h>
#include <vulkan/vulkan.h>

#ifdef __cplusplus
extern "C" {
#endif

/* flux_image lives in <flux/canvas.h>; forward-declare here so the
 * accessors below compile without dragging the full canvas header
 * into every vulkan.h consumer. */
typedef struct flux_image flux_image;

/* ================================================================== */
/*  Raw handle accessors                                              */
/* ================================================================== */

FLUX_API VkBuffer flux_buffer_vk_buffer(const flux_buffer *b);
FLUX_API uint64_t flux_buffer_device_address(const flux_buffer *b);

FLUX_API VkInstance flux_device_vk_instance(const flux_device *d);
FLUX_API VkPhysicalDevice flux_device_vk_physical_device(const flux_device *d);
FLUX_API VkDevice flux_device_vk_device(const flux_device *d);
FLUX_API VkQueue flux_device_vk_graphics_queue(const flux_device *d);
FLUX_API uint32_t flux_device_vk_graphics_family(const flux_device *d);
FLUX_API VkQueue flux_device_vk_transfer_queue(const flux_device *d);
FLUX_API uint32_t flux_device_vk_transfer_family(const flux_device *d);
/* The returned cache is shared by Flux's canvas/scene/compute/core pipeline
 * creation. Vulkan requires external synchronisation for every cache access.
 * Callers that pass this handle to Vulkan must hold this lock for the complete
 * Vulkan call; always pair lock/unlock on the same thread. */
FLUX_API VkPipelineCache flux_device_vk_pipeline_cache(const flux_device *d);
FLUX_API void flux_device_vk_pipeline_cache_lock(flux_device *d);
FLUX_API void flux_device_vk_pipeline_cache_unlock(flux_device *d);

FLUX_API VkSurfaceKHR flux_surface_vk_handle(const flux_surface *s);
FLUX_API VkSwapchainKHR flux_surface_vk_swapchain(const flux_surface *s);
FLUX_API VkFormat flux_surface_vk_format(const flux_surface *s);

FLUX_API VkCommandBuffer flux_frame_vk_command_buffer(const flux_frame *f);
FLUX_API VkImage flux_frame_vk_image(const flux_frame *f);
FLUX_API VkImageView flux_frame_vk_image_view(const flux_frame *f);

FLUX_API VkImage flux_image_vk_image(const flux_image *im);
FLUX_API VkImageView flux_image_vk_image_view(const flux_image *im);
/* flux_image_bindless_handle is declared in the bindless section below
 * because it returns flux_bindless_handle, which is typedef'd there. */

/* Render-target image view: hand this to flux_pass_attachment.view /
 * flux_pass_depth_attachment.view. NULL → VK_NULL_HANDLE. */
FLUX_API VkImageView flux_target_vk_view(const flux_target *t);
FLUX_API VkImage flux_target_vk_image(const flux_target *t);

/* The transient ring buffer underlying flux_frame_alloc_transient. */
FLUX_API VkBuffer flux_frame_vk_transient_buffer(const flux_frame *f);

/* ================================================================== */
/*  Format <-> VkFormat                                               */
/* ================================================================== */

FLUX_API VkFormat flux_format_to_vk(flux_format f);
FLUX_API flux_format flux_format_from_vk(VkFormat vf);

/* ================================================================== */
/*  Bindless descriptor heap (device-owned)                           */
/* ================================================================== */

/* Single set on slot 0 with descriptor indexing. Slots are stable for
 * the lifetime of the device until released. */

typedef uint32_t flux_bindless_handle;
#define FLUX_BINDLESS_INVALID UINT32_C(0xffffffff)

FLUX_NODISCARD FLUX_API flux_result flux_bindless_register_image(flux_device *d, VkImageView view,
                                                                 VkImageLayout layout,
                                                                 flux_bindless_handle *out_handle);

/* Register a storage-image view (writable from shaders). The view's
 * underlying image must have been created with VK_IMAGE_USAGE_STORAGE_BIT
 * and the layout must be one valid for storage access (GENERAL is the
 * common choice). */
FLUX_NODISCARD FLUX_API flux_result flux_bindless_register_storage_image(
    flux_device *d, VkImageView view, VkImageLayout layout, flux_bindless_handle *out_handle);

FLUX_NODISCARD FLUX_API flux_result
flux_bindless_register_sampler(flux_device *d, VkSampler sampler, flux_bindless_handle *out_handle);

FLUX_API void flux_bindless_release(flux_device *d, flux_bindless_handle h);

FLUX_API VkDescriptorSet flux_device_bindless_set(flux_device *d);
FLUX_API VkDescriptorSetLayout flux_device_bindless_layout(flux_device *d);

/* The sampled bindless handle the image was registered into at
 * create time. FLUX_BINDLESS_INVALID if the image was created
 * without bindless registration (currently always set). */
FLUX_API flux_bindless_handle flux_image_bindless_handle(const flux_image *im);

/* ================================================================== */
/*  Graphics pipeline                                                 */
/* ================================================================== */

typedef struct flux_graphics_pipeline flux_graphics_pipeline;

typedef enum flux_primitive_topology {
    FLUX_TOPOLOGY_TRIANGLE_LIST = 0,
    FLUX_TOPOLOGY_TRIANGLE_STRIP = 1,
    FLUX_TOPOLOGY_LINE_LIST = 2,
    FLUX_TOPOLOGY_LINE_STRIP = 3,
    FLUX_TOPOLOGY_POINT_LIST = 4,
} flux_primitive_topology;

typedef enum flux_cull_mode {
    FLUX_CULL_NONE = 0,
    FLUX_CULL_BACK = 1,
    FLUX_CULL_FRONT = 2,
} flux_cull_mode;

/* Common blend presets, named for what they do at the surface.
 * NONE       — opaque overwrite (color blend disabled).
 * PREMUL     — Porter-Duff source-over assuming premultiplied src.
 * ADDITIVE   — src + dst (HDR particle-style accumulation). */
typedef enum flux_blend_preset {
    FLUX_BLEND_PRESET_NONE = 0,
    FLUX_BLEND_PRESET_PREMUL = 1,
    FLUX_BLEND_PRESET_ADDITIVE = 2,
} flux_blend_preset;

typedef enum flux_depth_test {
    FLUX_DEPTH_NONE = 0,
    FLUX_DEPTH_TEST_AND_WRITE = 1,
    FLUX_DEPTH_TEST_READONLY = 2,
} flux_depth_test;

typedef struct flux_vertex_attribute {
    uint32_t location;
    flux_format
        format; /* per-attribute format (e.g. RGBA8_UNORM, R32_SFLOAT not currently in enum) */
    uint32_t offset;
} flux_vertex_attribute;

typedef struct flux_vertex_binding {
    uint32_t stride;
    uint32_t attribute_count;
    const flux_vertex_attribute *attributes;
} flux_vertex_binding;

typedef struct flux_graphics_pipeline_desc {
    flux_struct_type type; /* FLUX_TYPE_GRAPHICS_PIPELINE_DESC */
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
    const flux_vertex_binding *vertex_binding;

    /* Rasterizer / topology / blend / depth state. */
    flux_primitive_topology topology;
    flux_cull_mode cull;
    flux_blend_preset blend;
    flux_depth_test depth;

    /* Render-target formats. Must match the pass attachments at draw
     * time. Use FLUX_FORMAT_UNDEFINED for `depth_format` when there
     * is no depth attachment. */
    flux_format color_format;
    flux_format depth_format;

    /* Push constants reachable from vertex + fragment stages. */
    uint32_t push_constant_bytes;
} flux_graphics_pipeline_desc;

#define FLUX_GRAPHICS_PIPELINE_DESC_INIT {.type = FLUX_TYPE_GRAPHICS_PIPELINE_DESC}

FLUX_NODISCARD FLUX_API flux_result flux_graphics_pipeline_create(
    flux_device *d, const flux_graphics_pipeline_desc *desc, flux_graphics_pipeline **out);

FLUX_NODISCARD FLUX_API flux_graphics_pipeline *
flux_graphics_pipeline_retain(flux_graphics_pipeline *p);
FLUX_API void flux_graphics_pipeline_release(flux_graphics_pipeline *p);

FLUX_API VkPipeline flux_graphics_pipeline_vk_pipeline(const flux_graphics_pipeline *p);
FLUX_API VkPipelineLayout flux_graphics_pipeline_vk_layout(const flux_graphics_pipeline *p);

/* Bind the pipeline and push `push_bytes` of `push_constants` (may be
 * NULL/0). Records into the frame's command buffer. The device
 * bindless set is bound at slot 0 to match the pipeline layout. */
FLUX_API void flux_graphics_pipeline_bind(flux_frame *f, flux_graphics_pipeline *pipeline,
                                          const void *push_constants, uint32_t push_bytes);

/* ================================================================== */
/*  Sampler                                                           */
/* ================================================================== */

/* Refcounted, device-owned sampler. Auto-registered in the bindless
 * heap on create — read the handle via flux_sampler_bindless_handle
 * to reference the sampler from any shader. */
typedef struct flux_sampler flux_sampler;

typedef enum flux_filter {
    FLUX_FILTER_NEAREST = 0,
    FLUX_FILTER_LINEAR = 1,
} flux_filter;

typedef enum flux_address_mode {
    FLUX_ADDRESS_REPEAT = 0,
    FLUX_ADDRESS_CLAMP_TO_EDGE = 1,
    FLUX_ADDRESS_MIRRORED_REPEAT = 2,
    FLUX_ADDRESS_CLAMP_TO_BORDER = 3,
} flux_address_mode;

typedef struct flux_sampler_desc {
    flux_struct_type type; /* FLUX_TYPE_SAMPLER_DESC */
    const void *next;
    flux_filter min_filter;
    flux_filter mag_filter;
    flux_filter mipmap_mode; /* used iff the sampled image has mips */
    flux_address_mode address_u;
    flux_address_mode address_v;
    flux_address_mode address_w;
    float max_anisotropy; /* 1.0 = off; clamped to device cap */
} flux_sampler_desc;

#define FLUX_SAMPLER_DESC_INIT {.type = FLUX_TYPE_SAMPLER_DESC}

FLUX_NODISCARD FLUX_API flux_result flux_sampler_create(flux_device *d,
                                                        const flux_sampler_desc *desc,
                                                        flux_sampler **out);

FLUX_NODISCARD FLUX_API flux_sampler *flux_sampler_retain(flux_sampler *s);
FLUX_API void flux_sampler_release(flux_sampler *s);

FLUX_API VkSampler flux_sampler_vk_sampler(const flux_sampler *s);
FLUX_API flux_bindless_handle flux_sampler_bindless_handle(const flux_sampler *s);

/* ================================================================== */
/*  Dynamic-rendering pass                                            */
/* ================================================================== */

typedef enum flux_load_op {
    FLUX_LOAD_DONT_CARE = 0,
    FLUX_LOAD_CLEAR = 1,
    FLUX_LOAD_LOAD = 2
} flux_load_op;
typedef enum flux_store_op { FLUX_STORE_DONT_CARE = 0, FLUX_STORE_STORE = 1 } flux_store_op;

typedef struct flux_pass_attachment {
    VkImageView view; /* may be VK_NULL_HANDLE → use frame's swapchain image */
    VkFormat format;
    flux_load_op load_op;
    flux_store_op store_op;
    flux_vec4 clear_color; /* used iff load_op == FLUX_LOAD_CLEAR */
    /* When true (and `view` is a multisample image), the pass resolves `view`
     * into the frame's swapchain image at end of rendering — for MSAA canvas
     * output. The multisample colour is not stored. */
    bool resolve_to_surface;
    /* When non-NULL, the pass resolves the multisample `view` into this image
     * view instead of the swapchain — used by canvas target capture
     * (ADR-0017), which resolves into a caller-owned flux_image. Takes
     * precedence over resolve_to_surface. */
    VkImageView resolve_view;
} flux_pass_attachment;

typedef struct flux_pass_depth_attachment {
    VkImageView view; /* peer-supplied depth image view */
    VkFormat format;
    flux_load_op load_op;
    flux_store_op store_op;
    float clear_depth;
    uint8_t clear_stencil;
} flux_pass_depth_attachment;

typedef struct flux_pass_desc {
    flux_struct_type type; /* FLUX_TYPE_PASS_DESC */
    const void *next;
    uint32_t color_attachment_count;
    const flux_pass_attachment *color_attachments;
    const flux_pass_depth_attachment *depth; /* nullable */
    /* Stencil-only attachment (nullable). `view` must include the
     * stencil aspect; `clear_stencil` is the clear value, the depth
     * fields are ignored. Bound pipelines must declare a matching
     * stencilAttachmentFormat. The canvas uses this internally for
     * stencil-then-cover fills (ADR-0014). */
    const flux_pass_depth_attachment *stencil;
    /* Optional render extent for caller-supplied attachments. Zero selects
     * the frame surface extent. Attachment views must cover this area. */
    uint32_t width;
    uint32_t height;
} flux_pass_desc;

#define FLUX_PASS_DESC_INIT {.type = FLUX_TYPE_PASS_DESC}

FLUX_API void flux_frame_begin_pass(flux_frame *f, const flux_pass_desc *desc);
FLUX_API void flux_frame_end_pass(flux_frame *f);

/* Backend-neutral wrappers for the dynamic viewport and scissor state used by
 * graphics pipelines. The frame must be recording; calls outside that state
 * are ignored. */
FLUX_API void flux_frame_set_viewport(flux_frame *f, float x, float y, float width, float height,
                                      float min_depth, float max_depth);
FLUX_API void flux_frame_set_scissor(flux_frame *f, int32_t x, int32_t y, uint32_t width,
                                     uint32_t height);

#ifdef __cplusplus
}
#endif

#endif /* FLUX_VULKAN_H */
