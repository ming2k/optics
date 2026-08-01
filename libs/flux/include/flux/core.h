/*
 * flux/core.h — device, surface, frame, allocator, logger, results.
 *
 * Pure C. No <vulkan/vulkan.h> in this header. Pull <flux/vulkan.h>
 * at the seam where you hand flux a VkSurfaceKHR or pull a raw
 * VkCommandBuffer back.
 *
 * Design contract:
 *   - Opaque handles. Never dereference flux_device, flux_surface,
 *     flux_frame, flux_image. They are forward declarations.
 *   - Refcount the heavy (device, surface, GPU resources);
 *     value types live on the caller's stack or in an arena.
 *   - Every fallible call returns flux_result and is marked
 *     [[nodiscard]].
 *   - Descriptor structs open with `flux_struct_type type;
 *     const void *next;` for extension chains (Vulkan sType/pNext
 *     pattern).
 */

#ifndef FLUX_CORE_H
#define FLUX_CORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================== */
/*  Visibility & attributes                                           */
/* ================================================================== */

#if defined(_WIN32) && !defined(FLUX_STATIC)
#ifdef FLUX_BUILDING
#define FLUX_API __declspec(dllexport)
#else
#define FLUX_API __declspec(dllimport)
#endif
#elif defined(__GNUC__) || defined(__clang__)
#define FLUX_API __attribute__((visibility("default")))
#else
#define FLUX_API
#endif

#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 202311L
#define FLUX_NODISCARD [[nodiscard]]
#elif defined(__GNUC__) || defined(__clang__)
#define FLUX_NODISCARD __attribute__((warn_unused_result))
#elif defined(_MSC_VER)
#define FLUX_NODISCARD _Check_return_
#else
#define FLUX_NODISCARD
#endif

/* ================================================================== */
/*  Versioning                                                        */
/* ================================================================== */

#define FLUX_VERSION_MAJOR 0
#define FLUX_VERSION_MINOR 0
#define FLUX_VERSION_PATCH 7

/* Packed integer version, monotonic: bits 16..23 major, 8..15 minor,
 * 0..7 patch. 0.0.1 == 0x00000001; 1.2.3 == 0x00010203. */
#define FLUX_VERSION_NUMBER                                                                        \
    (((uint32_t)FLUX_VERSION_MAJOR << 16) | ((uint32_t)FLUX_VERSION_MINOR << 8) |                  \
     (uint32_t)FLUX_VERSION_PATCH)

FLUX_API void flux_version(int *major, int *minor, int *patch);
FLUX_API uint32_t flux_version_number(void);
FLUX_API bool flux_version_check(int major, int minor, int patch);
FLUX_API const char *flux_version_string(void);

/* ================================================================== */
/*  Result codes                                                      */
/* ================================================================== */

typedef enum flux_result {
    FLUX_OK = 0,
    FLUX_ERROR_INVALID_ARGUMENT = 1,
    FLUX_ERROR_OUT_OF_MEMORY = 2,
    FLUX_ERROR_OUT_OF_RANGE = 3,
    FLUX_ERROR_INVALID_STATE = 4,
    FLUX_ERROR_UNSUPPORTED = 5,
    FLUX_ERROR_BACKEND_FAILURE = 6,
    FLUX_ERROR_DEVICE_LOST = 7,
    FLUX_ERROR_SURFACE_LOST = 8,
    FLUX_ERROR_TIMEOUT = 9,
} flux_result;

FLUX_API const char *flux_result_string(flux_result r);

/* Structured diagnostic for the most recent error on this thread.
 *
 * Thread-local: each thread keeps its own slot. A non-OK return on
 * thread A does not affect what thread B observes.
 *
 * The string fields are owned by flux (typically literals or static
 * buffers); never free them. The strings remain valid until the next
 * non-OK return on the same thread, after which they may be
 * overwritten in place. Any pointer may be NULL.
 *
 * Call after a non-OK return for context. */
typedef struct flux_error_info {
    flux_result code;
    const char *function; /* "flux_device_create" */
    const char *file;     /* source file inside flux */
    int line;
    const char *message;  /* human-readable detail; may be NULL */
    int32_t backend_code; /* e.g. VkResult cast to int when relevant */
} flux_error_info;

/* Copies the most recent thread-local diagnostic into *out. The
 * `out` struct becomes the caller's; the string pointers inside it
 * follow the lifetime rules documented on flux_error_info above. */
FLUX_API void flux_get_last_error(flux_error_info *out);

/* ================================================================== */
/*  Format                                                            */
/* ================================================================== */

/* Backend-neutral pixel/attachment format used by canvas images and
 * scene materials. Mapped to VkFormat by flux_format_to_vk
 * (declared in <flux/vulkan.h>). The UNORM/SRGB distinction follows
 * Vulkan: UNORM writes data verbatim; SRGB applies the sRGB
 * transfer function on store. */
typedef enum flux_format {
    FLUX_FORMAT_UNDEFINED = 0,

    /* 8-bit colour */
    FLUX_FORMAT_R8_UNORM = 1,
    FLUX_FORMAT_RGBA8_UNORM = 2,
    FLUX_FORMAT_BGRA8_UNORM = 3,
    FLUX_FORMAT_RGBA8_SRGB = 4,
    FLUX_FORMAT_BGRA8_SRGB = 5,

    /* HDR / float colour */
    FLUX_FORMAT_RGBA16_SFLOAT = 6,

    /* Depth */
    FLUX_FORMAT_D32_SFLOAT = 7,
    FLUX_FORMAT_D24_UNORM_S8 = 8,
    FLUX_FORMAT_D32_SFLOAT_S8 = 9,

    /* Float vertex attributes */
    FLUX_FORMAT_R32_SFLOAT = 10,
    FLUX_FORMAT_RG32_SFLOAT = 11,
    FLUX_FORMAT_RGB32_SFLOAT = 12,
    FLUX_FORMAT_RGBA32_SFLOAT = 13,
} flux_format;

/* ================================================================== */
/*  Tagged-struct discriminator                                       */
/* ================================================================== */

typedef enum flux_struct_type {
    FLUX_TYPE_UNKNOWN = 0,
    FLUX_TYPE_DEVICE_DESC = 1,
    FLUX_TYPE_SURFACE_DESC = 2,
    FLUX_TYPE_FRAME_BEGIN_DESC = 3,
    FLUX_TYPE_PASS_DESC = 4,
    FLUX_TYPE_IMAGE_DESC = 5,
    FLUX_TYPE_CANVAS_DESC = 6,
    FLUX_TYPE_MESH_DESC = 7,
    FLUX_TYPE_MATERIAL_DESC = 8,
    FLUX_TYPE_COMPUTE_PIPELINE_DESC = 9,
    FLUX_TYPE_SAMPLER_DESC = 10,
    FLUX_TYPE_BUFFER_DESC = 11,
    FLUX_TYPE_GRAPHICS_PIPELINE_DESC = 12,
    FLUX_TYPE_EFFECT_BLUR_DESC = 13,
    FLUX_TYPE_DMABUF_IMAGE_DESC = 14,
    FLUX_TYPE_GLYPH_RUN_DESC = 15,
    FLUX_TYPE_TARGET_DESC = 16,
    FLUX_TYPE_SURFACE_DMABUF_DESC = 17,
    FLUX_TYPE_SURFACE_READBACK_DESC = 18,
    FLUX_TYPE_CANVAS_PASS_DESC = 19,
    FLUX_TYPE_LIQUID_GLASS_DESC = 20,
    FLUX_TYPE_MESH_SKIN_DESC = 21,
    /* Append only. Never repurpose. */
} flux_struct_type;

/* ================================================================== */
/*  Allocator & logger (caller-supplied)                              */
/* ================================================================== */

typedef struct flux_allocator {
    void *(*alloc)(size_t bytes, void *user);
    void *(*realloc)(void *ptr, size_t old_bytes, size_t new_bytes, void *user);
    void (*free)(void *ptr, void *user);
    void *user;
} flux_allocator;

typedef enum flux_log_level {
    FLUX_LOG_TRACE = 0,
    FLUX_LOG_DEBUG = 1,
    FLUX_LOG_INFO = 2,
    FLUX_LOG_WARN = 3,
    FLUX_LOG_ERROR = 4,
} flux_log_level;

typedef void (*flux_log_fn)(flux_log_level level, const char *file, int line, const char *fmt,
                            const char *msg, void *user);

FLUX_API void flux_console_logger(flux_log_level level, const char *file, int line, const char *fmt,
                                  const char *msg, void *user);

/* Forward declarations — needed early so callback signatures and
 * function prototypes below can reference the opaque handles. */
typedef struct flux_device flux_device;
typedef struct flux_surface flux_surface;
typedef struct flux_frame flux_frame;
typedef struct flux_readback flux_readback;
typedef struct flux_buffer flux_buffer;
typedef struct flux_target flux_target;
/* flux_image lives in <flux/canvas.h> — it's a canvas/2D concept. */

/* Emit a printf-style diagnostic through a device's logger (if any).
 * No-op when the device has no logger wired. Modules that treat
 * `flux_device` as opaque (and consumers) use this instead of
 * dereferencing `flux_device_desc.log` directly; it forwards to the
 * same `flux_log_fn` the desc supplied. `category` is a short tag the
 * logger can filter on (e.g. "flux-text", "vulkan-validation"). */
FLUX_API void flux_device_log(flux_device *d, flux_log_level level, const char *category,
                              const char *fmt, ...)
#if defined(__GNUC__) || defined(__clang__)
    __attribute__((format(printf, 4, 5)))
#endif
    ;

/* Pipeline-cache persistence hooks (caller-supplied, optional).
 *
 * Modelled on Skia's PersistentCache: the library owns the in-memory
 * VkPipelineCache; the consumer owns the cross-session storage
 * strategy (a file, a content store, nothing). flux never touches the
 * filesystem — without these hooks the cache lives only for the
 * device lifetime and is discarded at release.
 *
 * Wire both into flux_device_desc to opt in:
 *
 *   .pipeline_cache_load     = my_load,
 *   .pipeline_cache_save     = my_save,
 *   .pipeline_cache_userdata = &my_state,
 *
 *   load  — return a malloc()-allocated seed blob (the library frees
 *           it with free() after feeding it to VkPipelineCache). Set
 *           *out_size = 0 / return NULL when there is no prior cache;
 *           the cache then starts cold. May be NULL even if save is
 *           set (read-only cache).
 *   save  — receives the blob produced by vkGetPipelineCacheData. The
 *           library owns `data` for the call's duration; copy it if
 *           you need it beyond the return. Called once, at
 *           flux_device_release, after all GPU work has drained. May
 *           be NULL (write-only / fresh cache each run).
 *
 * Both callbacks receive the same `userdata` pointer set on the desc.
 * They are invoked from the thread calling flux_device_create /
 * flux_device_release. */
typedef void *(*flux_pipeline_cache_load_fn)(void *userdata, size_t *out_size);
typedef void (*flux_pipeline_cache_save_fn)(void *userdata, const void *data, size_t size);

/* ================================================================== */
/*  Device                                                            */
/* ================================================================== */

typedef enum flux_validation_mode {
    FLUX_VALIDATION_AUTO = 0, /* on iff debug build */
    FLUX_VALIDATION_OFF = 1,
    FLUX_VALIDATION_ON = 2,
} flux_validation_mode;

typedef struct flux_device_desc {
    flux_struct_type type; /* FLUX_TYPE_DEVICE_DESC */
    const void *next;

    flux_allocator allocator; /* zero-init = libc malloc */
    flux_log_fn log;          /* NULL = silent */
    void *log_user;

    /* Optional pipeline-cache persistence (see the typedefs above).
     * NULL throughout = pure in-memory cache, discarded at release. */
    flux_pipeline_cache_load_fn pipeline_cache_load;
    flux_pipeline_cache_save_fn pipeline_cache_save;
    void *pipeline_cache_userdata;

    /* Vulkan bootstrap */
    const char *const *required_instance_extensions;
    uint32_t required_instance_extension_count;
    const char *const *required_device_extensions;
    uint32_t required_device_extension_count;

    flux_validation_mode validation;

    /* Frames in flight (0 = library default, currently 2) */
    uint32_t frames_in_flight;

    /* Headless if true — no surface, no swapchain, no presentation
     * queue family requirement. */
    bool headless;
} flux_device_desc;

#define FLUX_DEVICE_DESC_INIT {.type = FLUX_TYPE_DEVICE_DESC}

FLUX_NODISCARD FLUX_API flux_result flux_device_create(const flux_device_desc *desc,
                                                       flux_device **out_device);

FLUX_NODISCARD FLUX_API flux_device *flux_device_retain(flux_device *d);
FLUX_API void flux_device_release(flux_device *d);

FLUX_API void flux_device_wait_idle(const flux_device *d);

/* Device-owned allocation passthroughs. Use these instead of libc
 * malloc/free in peer code so the caller's allocator is honoured. */
FLUX_API void *flux_device_alloc(flux_device *d, size_t bytes);
FLUX_API void flux_device_free(flux_device *d, void *ptr);

#define FLUX_MAX_MEMORY_HEAPS 16

typedef struct flux_memory_budget {
    uint64_t heap_bytes_total[FLUX_MAX_MEMORY_HEAPS];
    uint64_t heap_bytes_used[FLUX_MAX_MEMORY_HEAPS];
    uint64_t heap_budget[FLUX_MAX_MEMORY_HEAPS];
    uint32_t heap_count;
    bool has_budget_extension;
} flux_memory_budget;

FLUX_API void flux_device_memory_budget(const flux_device *d, flux_memory_budget *out);

/* Point-in-time snapshot of the GPU memory allocator. bytes_in_use is
 * what live resources (pooled sub-allocations, dedicated and external
 * dma-buf memory) actually occupy; bytes_reserved is what the allocator
 * holds from the driver in total, including empty pool blocks kept for
 * reuse. All counters return to zero once every resource is released
 * and the retire queue has drained (see flux_device_wait_idle). */
typedef struct flux_memory_stats {
    uint64_t bytes_in_use;
    uint64_t bytes_reserved;
    uint64_t lost_ranges_bytes; /* freed bytes unrecoverable until block reclaim */
    uint32_t live_allocations;
    uint32_t live_blocks;
} flux_memory_stats;

/* Sample live allocator counters. Deferred upload work (staging buffers
 * still in flight) is settled before sampling so the numbers do not
 * depend on GPU timing; this makes the call wait on outstanding upload
 * fences, so keep it off hot paths. */
FLUX_API void flux_device_memory_stats(flux_device *d, flux_memory_stats *out);

/* Batched uploads. Between begin and flush, upload work from
 * flux_image_create, flux_image_update_region, flux_mesh_create and
 * flux_buffer_create (GPU_LOCAL with initial_data) accumulates into one
 * queue submission instead of one submission per resource — this is
 * the fast path for bulk asset loading.
 *
 * Upload submissions are deferred: flush (and the one-shot helpers
 * outside a batch) submit the copy work and return without waiting for
 * the GPU. A resource is safe to sample as soon as its creating call
 * returns, because every later batch on the same queue is ordered after
 * the copies; flux_surface_begin_frame flushes any open batch before
 * recording, so an unflushed batch can never be sampled by a frame.
 * Non-frame consumers (compute dispatch, readback) must flush
 * explicitly. begin/flush are device-global; the upload calls
 * themselves may run on any thread. Nested begin returns
 * FLUX_ERROR_INVALID_STATE; flushing with no batch open is a harmless
 * no-op. */
FLUX_NODISCARD FLUX_API flux_result flux_uploads_begin(flux_device *d);
FLUX_NODISCARD FLUX_API flux_result flux_uploads_flush(flux_device *d);

/* ================================================================== */
/*  Buffer                                                            */
/* ================================================================== */

/* Bitmask of intended uses; OR these together in flux_buffer_desc.usage.
 * The library maps them to VkBufferUsageFlags internally and adds
 * TRANSFER_DST automatically when initial_data is provided. */
typedef enum flux_buffer_usage {
    FLUX_BUFFER_USAGE_VERTEX = 1u << 0,
    FLUX_BUFFER_USAGE_INDEX = 1u << 1,
    FLUX_BUFFER_USAGE_UNIFORM = 1u << 2,
    FLUX_BUFFER_USAGE_STORAGE = 1u << 3,
    FLUX_BUFFER_USAGE_TRANSFER_SRC = 1u << 4,
    FLUX_BUFFER_USAGE_TRANSFER_DST = 1u << 5,
} flux_buffer_usage;

typedef enum flux_buffer_location {
    /* DEVICE_LOCAL memory: fastest GPU access; not host-mappable.
     * Use initial_data or a TRANSFER_SRC buffer to populate. */
    FLUX_BUFFER_GPU_LOCAL = 0,
    /* HOST_VISIBLE | HOST_COHERENT: persistent CPU map. Slower GPU
     * access but writable from the CPU every frame (uniforms, dynamic
     * vertex data). */
    FLUX_BUFFER_HOST_VISIBLE = 1,
} flux_buffer_location;

typedef struct flux_buffer_desc {
    flux_struct_type type; /* FLUX_TYPE_BUFFER_DESC */
    const void *next;
    size_t size;
    uint32_t usage; /* OR of flux_buffer_usage bits */
    flux_buffer_location location;
    /* If true, the buffer is created with SHADER_DEVICE_ADDRESS and
     * flux_buffer_device_address() returns the 64-bit address. */
    bool device_address;
    /* Optional: upload `size` bytes at create time. For GPU_LOCAL
     * buffers this routes through the staging path; for HOST_VISIBLE
     * it's a direct memcpy into the mapped pointer. */
    const void *initial_data;
} flux_buffer_desc;

#define FLUX_BUFFER_DESC_INIT {.type = FLUX_TYPE_BUFFER_DESC}

FLUX_NODISCARD FLUX_API flux_result flux_buffer_create(flux_device *d, const flux_buffer_desc *desc,
                                                       flux_buffer **out);

FLUX_NODISCARD FLUX_API flux_buffer *flux_buffer_retain(flux_buffer *b);
/* Destruction is deferred: a released buffer may still be referenced by
 * in-flight frames, so the VkBuffer and its memory are destroyed by the
 * device retire queue only after the GPU provably passed every batch
 * that could reference them. */
FLUX_API void flux_buffer_release(flux_buffer *b);

/* Persistent mapped pointer; NULL on a GPU_LOCAL buffer. */
FLUX_API void *flux_buffer_mapped(const flux_buffer *b);
FLUX_API size_t flux_buffer_size(const flux_buffer *b);

/* ================================================================== */
/*  Render target                                                     */
/*                                                                    */
/*  A refcounted depth or colour image + view that a caller owns and  */
/*  hands to a pass via flux_pass_desc (peer-defined attachments,     */
/*  ADR-0001). This is how consumers get a depth attachment without    */
/*  writing raw Vulkan image/memory/view plumbing: flux owns the      */
/*  image, the backing GPU allocator memory, and the view, mirroring  */
/*  the refcount lifecycle of flux_buffer / flux_mesh.                */
/* ================================================================== */

typedef enum flux_target_usage {
    FLUX_TARGET_DEPTH = 1u << 0, /* depth/stencil attachment image  */
    FLUX_TARGET_COLOR = 1u << 1, /* colour attachment image         */
} flux_target_usage;

typedef struct flux_target_desc {
    flux_struct_type type; /* FLUX_TYPE_TARGET_DESC */
    const void *next;
    uint32_t usage;     /* OR of flux_target_usage */
    flux_format format; /* e.g. FLUX_FORMAT_D32_SFLOAT */
    uint32_t width;
    uint32_t height;
} flux_target_desc;

#define FLUX_TARGET_DESC_INIT {.type = FLUX_TYPE_TARGET_DESC}

FLUX_NODISCARD FLUX_API flux_result flux_target_create(flux_device *d, const flux_target_desc *desc,
                                                       flux_target **out);

FLUX_NODISCARD FLUX_API flux_target *flux_target_retain(flux_target *t);
/* Destruction is deferred: a released target may still be an attachment
 * of in-flight frames, so the image, view, and memory are destroyed by
 * the device retire queue only after the GPU provably passed every batch
 * that could reference them. */
FLUX_API void flux_target_release(flux_target *t);

FLUX_API uint32_t flux_target_width(const flux_target *t);
FLUX_API uint32_t flux_target_height(const flux_target *t);

/* Record the layout transition that makes `target` usable as the attachment
 * selected by its usage. The target's previous contents are discarded, so
 * the following pass must clear or otherwise fully overwrite it. Call once
 * per frame before flux_frame_begin_pass. A target reused across frames must
 * be dedicated to that frame-in-flight slot (or otherwise externally
 * synchronized). */
FLUX_API void flux_frame_prepare_target(flux_frame *f, const flux_target *target);

/* ================================================================== */
/*  Surface                                                           */
/* ================================================================== */

typedef struct flux_surface_desc {
    flux_struct_type type; /* FLUX_TYPE_SURFACE_DESC */
    const void *next;
    void *vk_surface_khr; /* VkSurfaceKHR; caller-created via GLFW/SDL/raw.
                           * NULL selects an OFFSCREEN surface (ADR-0013):
                           * no swapchain or window; flux owns RGBA8 color
                           * images at width x height (both required
                           * non-zero). The frame loop is unchanged —
                           * flux_frame_present completes the frame without
                           * presenting — and flux_surface_read_pixels
                           * reads the result back. */
    uint32_t width;
    uint32_t height;
    bool hdr_preferred; /* request scRGB/HDR10 if available; ignored offscreen */
    bool vsync;         /* true = FIFO, false = MAILBOX/IMMEDIATE; ignored offscreen */
} flux_surface_desc;

#define FLUX_SURFACE_DESC_INIT {.type = FLUX_TYPE_SURFACE_DESC}

/* Optional flux_surface_desc extension for an exportable offscreen surface.
 * `modifiers` is the consumer-supported DRM_FORMAT_MOD_* set. Flux intersects
 * it with the Vulkan device's renderable/exportable single-plane modifiers;
 * surface creation fails with FLUX_ERROR_UNSUPPORTED when the intersection is
 * empty. The array is read only during flux_surface_create. */
typedef struct flux_surface_dmabuf_desc {
    flux_struct_type type; /* FLUX_TYPE_SURFACE_DMABUF_DESC */
    const void *next;
    const uint64_t *modifiers;
    uint32_t modifier_count;
} flux_surface_dmabuf_desc;

#define FLUX_SURFACE_DMABUF_DESC_INIT {.type = FLUX_TYPE_SURFACE_DMABUF_DESC}

/* Optional flux_surface_desc extension for a CPU-readable offscreen surface.
 * On a dma-buf-capable device an offscreen surface is normally made
 * exportable, which transfers each submitted image to the foreign consumer
 * and makes flux_surface_read_pixels refuse it. Setting `require_readback`
 * keeps the surface non-exportable (OPTIMAL-tiling, slab-allocated) so
 * readback always works — the right pick for screenshot/capture targets
 * that never leave the process. Mutually exclusive with
 * flux_surface_dmabuf_desc (FLUX_ERROR_INVALID_ARGUMENT). Ignored on
 * windowed surfaces. */
typedef struct flux_surface_readback_desc {
    flux_struct_type type; /* FLUX_TYPE_SURFACE_READBACK_DESC */
    const void *next;
    bool require_readback;
} flux_surface_readback_desc;

#define FLUX_SURFACE_READBACK_DESC_INIT {.type = FLUX_TYPE_SURFACE_READBACK_DESC}

typedef struct flux_surface_info {
    uint32_t width;
    uint32_t height;
    uint32_t image_count;
    bool hdr; /* surface is actually HDR */
} flux_surface_info;

FLUX_NODISCARD FLUX_API flux_result flux_surface_create(flux_device *device,
                                                        const flux_surface_desc *desc,
                                                        flux_surface **out_surface);

FLUX_NODISCARD FLUX_API flux_surface *flux_surface_retain(flux_surface *s);
FLUX_API void flux_surface_release(flux_surface *s);

/* Recreate the swapchain at the new extent. Stalls the device, drops
 * and rebuilds swapchain images and per-image views; in-flight
 * flux_frame handles obtained from this surface become invalid and
 * the next flux_surface_begin_frame returns the first frame of the
 * new chain. Safe to call from the window resize callback. Returns
 * FLUX_ERROR_INVALID_ARGUMENT if w or h is 0. */
FLUX_API flux_result flux_surface_resize(flux_surface *s, uint32_t w, uint32_t h);
FLUX_API void flux_surface_get_info(const flux_surface *s, flux_surface_info *out);

/* Read back a captured frame as tightly packed RGBA8, row-major, top-left
 * origin (`width * height * 4` bytes; `bytes` must be at least that).
 * Windowed and exportable surfaces require flux_frame_request_readback on the
 * submitted frame. Plain offscreen surfaces retain their legacy behaviour of
 * reading the most recently submitted image. Waits for the relevant GPU work
 * to complete first. Returns FLUX_ERROR_INVALID_STATE when no readable frame
 * is available. */
FLUX_NODISCARD FLUX_API flux_result flux_surface_read_pixels(flux_surface *s, void *dst,
                                                             size_t bytes);

/* Allocate persistent readback staging ahead of the first capture. This is
 * optional (flux_frame_request_readback allocates it lazily) but lets
 * latency-sensitive callers keep allocation out of the trigger frame. Resize
 * retires this staging, so callers may prepare again afterward. */
FLUX_NODISCARD FLUX_API flux_result flux_surface_prepare_readback(flux_surface *s);

/* Non-blocking readiness query for a frame copied to persistent readback
 * staging. `out_ready` becomes true once that frame's image-to-buffer copy has
 * completed. Returns FLUX_ERROR_INVALID_STATE before the first captured frame
 * and FLUX_ERROR_UNSUPPORTED when the surface has no persistent readback
 * staging (a plain offscreen surface before any on-demand request). */
FLUX_NODISCARD FLUX_API flux_result flux_surface_read_pixels_ready(flux_surface *s,
                                                                   bool *out_ready);

/* Detach a completed on-demand snapshot from its surface without copying its
 * pixels. The returned immutable handle owns the mapped staging allocation,
 * can outlive or move to a different thread from the surface, and is consumed
 * with flux_readback_read_pixels before flux_readback_release. This is
 * unsupported for require_readback surfaces, whose staging is continuously
 * surface-owned. The caller should first observe `out_ready == true`. */
FLUX_NODISCARD FLUX_API flux_result flux_surface_take_readback(flux_surface *s,
                                                               flux_readback **out_readback);
FLUX_NODISCARD FLUX_API flux_result flux_readback_read_pixels(const flux_readback *readback,
                                                              void *dst, size_t bytes);
FLUX_API void flux_readback_release(flux_readback *readback);

/* Offscreen surfaces only (and only when the device had the dma-buf
 * extensions enabled at creation): export the most recently submitted
 * frame's image memory as a Linux dma-buf file descriptor. The caller
 * owns and must close() the returned fd. Zero-copy: no GPU->CPU pixel
 * transfer; the fd is a handle to the same GPU memory the compositor
 * composites directly. Waits for the frame's GPU work to complete first.
 *
 * flux_surface_exportable() reports whether the surface was created
 * exportable (device had the needed extensions + a suitable modifier was
 * found). flux_surface_dmabuf_modifier() / _stride() return the DRM
 * modifier and row stride the host pairs with the fd when building a
 * zwp_linux_buffer_params_v1.
 *
 * Export releases the selected image from the graphics queue family to
 * VK_QUEUE_FAMILY_FOREIGN_EXT. Before that frame slot is reused, the host must
 * wait until the external consumer has released the buffer (for Wayland,
 * wl_buffer.release or a signalled explicit-sync release fence). Calling
 * begin_frame early is a caller error the library cannot observe. Likewise,
 * resize/release requires every exported buffer to be back from its consumer.
 * The v1 export contract is one plane, offset 0, BGRA8, with the returned
 * modifier and stride; surfaces that cannot meet it are not exportable.
 *
 * Returns FLUX_ERROR_UNSUPPORTED on a windowed or non-exportable surface,
 * FLUX_ERROR_INVALID_STATE before the first submitted frame. */
FLUX_API bool flux_surface_exportable(const flux_surface *s);
FLUX_API uint64_t flux_surface_dmabuf_modifier(const flux_surface *s);
FLUX_API uint32_t flux_surface_dmabuf_stride(const flux_surface *s);
FLUX_NODISCARD FLUX_API flux_result flux_surface_export_dmabuf(flux_surface *s, int *out_fd);

/* Explicit-sync export for a submitted exportable offscreen frame. Returns a
 * dma-buf fd plus a Linux sync_file fd signalled by the same Vulkan submission
 * that released image ownership to VK_QUEUE_FAMILY_FOREIGN_EXT. Unlike
 * flux_surface_export_dmabuf, this does not wait for the GPU on the CPU.
 * Both descriptors belong to the caller on success. Requires
 * VK_KHR_external_semaphore_fd and may be called once per submitted slot. */
FLUX_NODISCARD FLUX_API flux_result flux_surface_export_dmabuf_explicit(flux_surface *s,
                                                                        int *out_fd,
                                                                        int *out_sync_fd);

/* Offscreen surfaces: the frame slot index of the most recently submitted
 * frame (0..frames_in_flight-1), or UINT32_MAX before the first submit.
 * The host uses this to align a per-slot dma-buf buffer pool with the
 * image whose memory flux_surface_export_dmabuf will export. */
FLUX_API uint32_t flux_surface_last_slot(const flux_surface *s);

/* ================================================================== */
/*  Frame                                                             */
/* ================================================================== */

typedef struct flux_frame_begin_desc {
    flux_struct_type type; /* FLUX_TYPE_FRAME_BEGIN_DESC */
    const void *next;
    uint64_t timeout_ns; /* 0 = FLUX_DEFAULT_FRAME_TIMEOUT_NS (2s) */
} flux_frame_begin_desc;

/* Default fence/acquire timeout for frame begin, image upload, and one-shot
 * GPU submissions. 2 seconds: long enough for any realistic GPU workload,
 * short enough that a hung GPU/driver surfaces as FLUX_ERROR_TIMEOUT rather
 * than freezing the caller thread indefinitely. */
#define FLUX_DEFAULT_FRAME_TIMEOUT_NS (2000000000ull)

#define FLUX_FRAME_BEGIN_DESC_INIT {.type = FLUX_TYPE_FRAME_BEGIN_DESC}

FLUX_NODISCARD FLUX_API flux_result flux_surface_begin_frame(flux_surface *s,
                                                             const flux_frame_begin_desc *desc,
                                                             flux_frame **out_frame);

/* Capture the exact color attachment produced by this recording frame.
 * flux_frame_submit inserts an image-to-buffer copy before the final present
 * or dma-buf ownership transition. The copied pixels remain unchanged by
 * later frames and can be polled with flux_surface_read_pixels_ready, then
 * retrieved with flux_surface_read_pixels. A later request replaces the
 * surface's previous snapshot. */
FLUX_NODISCARD FLUX_API flux_result flux_frame_request_readback(flux_frame *f);

/* Frames follow a strict single-use state machine:
 * begin_frame -> submit -> present. Calling either transition out of order or
 * more than once returns FLUX_ERROR_INVALID_STATE. The frame pointer is a
 * surface-owned borrow and becomes invalid when present returns. */
FLUX_NODISCARD FLUX_API flux_result flux_frame_submit(flux_frame *f);
FLUX_NODISCARD FLUX_API flux_result flux_frame_present(flux_frame *f);

/* Per-frame transient memory — GPU-visible, mapped, recycled after
 * FLUX_FRAMES_IN_FLIGHT frames. Returns aligned pointer + offset
 * relative to the underlying buffer (which lives in <flux/vulkan.h>).
 *
 *   gpu_address  is the buffer-device-address of `cpu`. It is 0 only
 *                if the device was created without the
 *                bufferDeviceAddress feature; on conformant flux
 *                devices it is always non-zero for a valid slice. */
typedef struct flux_transient {
    void *cpu;
    uint64_t gpu_address;
    size_t size;
    size_t alignment;
} flux_transient;

/* `alignment` must be a power of two between 1 and 256 inclusive.
 * Larger alignments are rejected with FLUX_ERROR_INVALID_ARGUMENT.
 * Returns FLUX_ERROR_OUT_OF_RANGE if the per-frame ring is exhausted. */
FLUX_NODISCARD FLUX_API flux_result flux_frame_alloc_transient(flux_frame *f, size_t bytes,
                                                               size_t alignment,
                                                               flux_transient *out);

FLUX_API uint32_t flux_frame_index(const flux_frame *f);

/* ================================================================== */
/*  GPU profiling — timestamp queries                                 */
/* ================================================================== */

FLUX_API void flux_frame_timestamp_begin(flux_frame *f, const char *label);
FLUX_API void flux_frame_timestamp_end(flux_frame *f);

typedef struct flux_timestamp_result {
    const char *label;
    double ms; /* GPU time in milliseconds */
} flux_timestamp_result;

/* Returns timestamps from the most recent COMPLETED frame at this
 * frame's slot. Caller-supplied buffer; out_count receives the actual
 * number written. */
FLUX_API flux_result flux_frame_collect_timestamps(flux_frame *f, flux_timestamp_result *out,
                                                   uint32_t *inout_count);

#ifdef __cplusplus
}
#endif

#endif /* FLUX_CORE_H */
