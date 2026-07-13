/*
 * Internal device layout. Never installed.
 */
#ifndef FLUX_CORE_INTERNAL_H
#define FLUX_CORE_INTERNAL_H

#include <flux/core.h>
#include <flux/vulkan.h> /* flux_bindless_handle */
#include <pthread.h>
#include <stdatomic.h>
#include <vulkan/vulkan.h>

/* Set the structured error info for this thread. Pass __func__ /
 * __FILE__ / __LINE__ at the call site. message and backend_code may
 * be NULL / 0. Defined in src/core/result.c. */
void flux_set_last_error(flux_result code, const char *function, const char *file, int line,
                         const char *message, int32_t backend_code);

#define FLUX_FAIL(code, msg)                                                                       \
    do {                                                                                           \
        flux_set_last_error((code), __func__, __FILE__, __LINE__, (msg), 0);                       \
    } while (0)

#define FLUX_FAIL_VK(code, msg, vk)                                                                \
    do {                                                                                           \
        flux_set_last_error((code), __func__, __FILE__, __LINE__, (msg), (int32_t)(vk));           \
    } while (0)

/* ------------------------------------------------------------------ */
/*  Device                                                            */
/* ------------------------------------------------------------------ */

/* ------------------------------------------------------------------ */
/*  GPU memory allocator (vk_allocator.c)                             */
/*                                                                    */
/*  Block-based sub-allocation. Pools are keyed by the triple         */
/*  (memory_type, is_image_pool, has_dev_addr); each pool is a list   */
/*  of fixed-size blocks with per-block free-list coalescing.         */
/*  Oversize requests fall back to a dedicated VkDeviceMemory.        */
/*  Thread-safe via a single per-allocator mutex.                     */
/*                                                                    */
/*  Buffer-image granularity is handled conservatively: images and    */
/*  buffers live in disjoint pools so adjacent placements never need  */
/*  the granularity check.                                            */
/*                                                                    */
/*  Safety: every dealloc is validated against the owning block's     */
/*  live byte counter; a double-free (or stale flux_vk_alloc copy)    */
/*  is rejected loudly instead of corrupting the free-list.           */
/*                                                                    */
/*  Reclaim: a block whose live count drops to zero is returned to    */
/*  the driver immediately if a same-key peer exists, and on every    */
/*  flux_vk_allocator_reclaim() otherwise. Lost ranges (free-list     */
/*  node OOM during dealloc) are accounted per-block so the reclaim   */
/*  path still recognises them as empty.                              */
/* ------------------------------------------------------------------ */

#define FLUX_VK_BLOCK_SIZE (64ull * 1024 * 1024)          /* 64 MiB */
#define FLUX_VK_DEDICATED_THRESH (FLUX_VK_BLOCK_SIZE / 4) /* 16 MiB */

typedef struct flux_vk_range {
    VkDeviceSize offset;
    VkDeviceSize size;
    struct flux_vk_range *next;
} flux_vk_range;

typedef struct flux_vk_block {
    VkDeviceMemory memory;
    VkDeviceSize size;
    uint32_t memory_type;
    bool is_image_pool;
    bool has_dev_addr;
    void *mapped;             /* non-NULL iff HOST_VISIBLE */
    flux_vk_range *free_list; /* sorted ascending by offset */
    /* Live allocation accounting. Updated under the allocator lock.
     *
     * `allocated_bytes` is the mirror of the free-list: it sums every
     * currently-outstanding sub-allocation in this block. It is the
     * authoritative "is this block empty?" check and the double-free
     * detector.
     *
     * `lost_bytes` counts ranges that were freed but could not be
     * reinserted into the free-list due to host OOM. They count as
     * "not allocated" for reclaim purposes — a block whose
     * allocated_bytes==0 is reclaimable even if some freed bytes were
     * lost — but they are no longer usable for new allocations until
     * the block itself is destroyed and re-created. */
    VkDeviceSize allocated_bytes;
    VkDeviceSize lost_bytes;
    uint32_t live_allocations;
    struct flux_vk_block *next;
} flux_vk_block;

typedef struct flux_vk_allocator {
    flux_vk_block *blocks;
    pthread_mutex_t lock;
    bool lock_initialized;
    /* Diagnostics — counted under the lock. */
    uint64_t bytes_in_use;
    uint64_t bytes_reserved;
    uint64_t lost_ranges_bytes; /* bytes lost due to OOM on free-list node alloc */
    uint32_t live_allocations;
    uint32_t live_blocks;
    bool has_memory_budget;
} flux_vk_allocator;

typedef struct flux_vk_alloc {
    VkDeviceMemory memory; /* The VkDeviceMemory this sub-alloc lives in. */
    VkDeviceSize offset;   /* Offset within `memory`. */
    VkDeviceSize size;     /* Padded size we reserved. */
    void *mapped;          /* CPU pointer at offset; NULL if not host-visible. */
    flux_vk_block *block;  /* NULL when this is a dedicated allocation. */
} flux_vk_alloc;

flux_result flux_vk_allocator_init(flux_device *d);
void flux_vk_allocator_destroy(flux_device *d);
uint32_t flux_vk_allocator_reclaim(flux_device *d);

flux_result flux_vk_allocate(flux_device *d, VkMemoryRequirements mr,
                             VkMemoryPropertyFlags wanted_flags, bool is_image,
                             bool wants_device_address, flux_vk_alloc *out);

void flux_vk_deallocate(flux_device *d, flux_vk_alloc *alloc);

/* ------------------------------------------------------------------ */
/*  Bindless descriptor heap                                          */
/* ------------------------------------------------------------------ */

#define FLUX_BINDLESS_BINDINGS 4
#define FLUX_BINDLESS_BIND_SAMPLED_IMAGE 0
#define FLUX_BINDLESS_BIND_STORAGE_IMAGE 1
#define FLUX_BINDLESS_BIND_SAMPLER 2
#define FLUX_BINDLESS_BIND_STORAGE_BUFFER 3

/* Largest push-constant range the library imposes on a device.
 * Currently driven by flux_canvas_push (canvas pipelines). Every
 * module's push struct is static_assert'd not to exceed this in
 * its own translation unit.
 *
 * Vulkan's minimum guarantee is 128 bytes. We need 160 (the canvas
 * image draw carries both image_dst and image_src rects). Intel ARL,
 * NVIDIA, AMD, Apple, and all of the lavapipe family of drivers
 * report ≥256 bytes; the device-init check below catches the
 * unlikely case of a host that reports the bare 128B minimum. */
#define FLUX_DEVICE_REQUIRED_PUSH_BYTES 160u

typedef struct flux_bindless_pool {
    uint32_t capacity;
    uint32_t free_top;    /* count of indices currently free */
    uint32_t *free_stack; /* indices available for allocation */
} flux_bindless_pool;

typedef struct flux_bindless_heap {
    VkDescriptorSetLayout layout;
    VkDescriptorPool pool;
    VkDescriptorSet set;
    flux_bindless_pool pools[FLUX_BINDLESS_BINDINGS];
    /* Serialises register / release. Descriptor-set writes through
     * the Vulkan API are externally synchronised for the same set,
     * so we cover both the slot allocator and the descriptor write
     * under one lock. */
    pthread_mutex_t lock;
    bool lock_initialized;
} flux_bindless_heap;

struct flux_device {
    atomic_uint ref_count;

    /* Caller-supplied policy */
    flux_allocator allocator;
    flux_log_fn log;
    void *log_user;
    uint32_t frames_in_flight;
    bool headless;
    bool validation_enabled;

    /* Hardware identification */
    uint32_t vendor_id;
    uint32_t device_id;
    bool is_nvidia;
    bool is_amd;
    bool is_intel;
    bool is_apple;
    VkDeviceSize buffer_image_granularity;

    /* Vulkan handles */
    VkInstance instance;
    VkDebugUtilsMessengerEXT debug_messenger; /* VK_NULL_HANDLE if validation off */

    VkPhysicalDevice physical_device;
    VkPhysicalDeviceProperties props;
    VkPhysicalDeviceMemoryProperties mem_props;
    /* Cached at device-init so the bindless heap and future callers
     * don't re-query (the per-binding cap_for_binding loop was hitting
     * vkGetPhysicalDeviceProperties2 N times). */
    VkPhysicalDeviceDescriptorIndexingProperties descriptor_indexing_props;

    VkDevice device;

    bool has_external_memory_fd;
    bool has_external_memory_dma_buf;
    bool has_image_drm_format_modifier;
    bool has_external_semaphore_fd;
    bool has_queue_family_foreign;

    uint32_t graphics_family;
    VkQueue graphics_queue;

    /* Transfer queue family — separate if available, else same as graphics. */
    uint32_t transfer_family;
    VkQueue transfer_queue;
    bool transfer_dedicated;

    /* Vulkan requires VkQueue access to be externally synchronised.
     * The frame path serialises submits per-surface implicitly, but
     * one-shot upload helpers (image_create, mesh_create, anything
     * calling flux_vk_upload_to_*) can be called from worker threads
     * during asset loading. This mutex guards every vkQueueSubmit2
     * issued by those helpers; the frame path acquires it too. */
    pthread_mutex_t queue_lock;
    bool queue_lock_initialized;

    /* Protects publication of lazily-created per-module state slots. The lock
     * is held only while reading or publishing pointers and hooks; allocation,
     * Vulkan calls, and module locks must remain outside it. */
    pthread_mutex_t module_state_lock;
    bool module_state_lock_initialized;

    /* Vulkan requires external synchronisation for every operation that reads
     * or mutates the shared VkPipelineCache, including pipeline creation and
     * vkGetPipelineCacheData. Raw-cache users lock it through vulkan.h. */
    pthread_mutex_t pipeline_cache_lock;
    bool pipeline_cache_lock_initialized;
    VkPipelineCache pipeline_cache;

    /* Consumer-supplied pipeline-cache persistence hooks (Skia
     * PersistentCache model). Copied from flux_device_desc at create
     * time so release can flush without the desc. NULL = no
     * persistence; the in-memory VkPipelineCache is discarded at
     * release. */
    flux_pipeline_cache_load_fn pipeline_cache_load;
    flux_pipeline_cache_save_fn pipeline_cache_save;
    void *pipeline_cache_userdata;

    /* GPU memory allocator — backs every VkDeviceMemory we hand out.
     * Init'd eagerly in flux_device_create after the logical device,
     * destroyed before vkDestroyDevice. Named mem_allocator to avoid
     * a clash with the caller-supplied CPU allocator above. */
    flux_vk_allocator mem_allocator;

    flux_bindless_heap bindless;

    /* Default sampler (linear filtering, clamp-to-edge). Lazily
     * created on first image registration. Registered into the
     * bindless heap at SAMPLER binding slot 0; that handle is
     * accessible via flux_device_default_sampler_handle. */
    VkSampler default_sampler;
    flux_bindless_handle default_sampler_handle;

    /* Per-module state slot. Modules attach a private struct here
     * lazily on first use; the device frees it via the per-module
     * destroy hook below at teardown. Keeps inter-module coupling
     * to a single typed pointer + a destroy callback, no header
     * dependency from core into canvas/scene/compute. */
    void *canvas_state;
    void (*canvas_state_destroy)(flux_device *d);
    void *effect_state;
    void (*effect_state_destroy)(flux_device *d);
};

/* Lazy initialisation of the device-owned default sampler. Returns
 * the bindless handle to use in shaders. Idempotent / thread-safe. */
flux_bindless_handle flux_device_default_sampler_handle(flux_device *d);

flux_result flux_bindless_heap_init(flux_device *d);
void flux_bindless_heap_destroy(flux_device *d);

/* Vulkan helper: find a memory type index satisfying both
 * type_filter (bitmask from VkMemoryRequirements.memoryTypeBits) and
 * the requested property flags. Returns UINT32_MAX on failure. */
uint32_t flux_vk_find_memory_type(flux_device *d, uint32_t type_filter,
                                  VkMemoryPropertyFlags wanted);

/* Allocate a buffer backed by the GPU allocator. Caller destroys the
 * VkBuffer with vkDestroyBuffer and frees the backing memory with
 * flux_vk_deallocate. `wants_device_address` must be true iff the
 * buffer was created with SHADER_DEVICE_ADDRESS_BIT usage. */
flux_result flux_vk_alloc_buffer(flux_device *d, VkDeviceSize size, VkBufferUsageFlags usage,
                                 VkMemoryPropertyFlags props, bool wants_device_address,
                                 VkBuffer *out_buffer, flux_vk_alloc *out_alloc);

/* Same shape for an image. Caller destroys the VkImage and calls
 * flux_vk_deallocate on the backing memory. */
flux_result flux_vk_alloc_image(flux_device *d, const VkImageCreateInfo *ici,
                                VkMemoryPropertyFlags props, VkImage *out_image,
                                flux_vk_alloc *out_alloc);

/* Dedicated-allocate an image whose VkImageCreateInfo pNext chain already
 * carries the external-memory / DRM-modifier structs. Unlike
 * flux_vk_alloc_image (slab sub-alloc), this always issues a dedicated
 * VkDeviceMemory so the bound memory can be exported as a dma-buf fd via
 * VK_KHR_external_memory_fd. `export_info` (may be NULL) is spliced into
 * the VkMemoryAllocateInfo pNext chain — pass a VkMemoryDedicatedAllocateInfo
 * or any external handle-type info the caller needs. */
flux_result flux_vk_alloc_image_dedicated(flux_device *d, const VkImageCreateInfo *ici,
                                          VkMemoryPropertyFlags props, const void *export_info,
                                          VkImage *out_image, flux_vk_alloc *out_alloc);

/* One-shot host -> device upload via a graphics-queue command buffer
 * (uses the graphics queue rather than transfer to avoid queue-family
 * ownership transfer plumbing in this simple path). Allocates a host-
 * visible staging buffer, copies in, records vkCmdCopyBuffer to dst,
 * submits with a fence, waits idle, and frees everything. */
flux_result flux_vk_upload_to_buffer(flux_device *d, VkBuffer dst, VkDeviceSize offset,
                                     const void *data, VkDeviceSize size);

/* One-shot host -> 2D image upload over a sub-region. Allocates a
 * host-visible staging buffer, copies in, records the layout
 * transitions and vkCmdCopyBufferToImage on a transient queue
 * submission. The image transitions from old_layout (UNDEFINED for a
 * first-time upload, SHADER_READ_ONLY_OPTIMAL for a region update)
 * through TRANSFER_DST_OPTIMAL and back to SHADER_READ_ONLY_OPTIMAL. */
flux_result flux_vk_upload_to_image(flux_device *d, VkImage dst, int32_t offset_x, int32_t offset_y,
                                    uint32_t width, uint32_t height, VkImageLayout old_layout,
                                    const void *data, size_t bytes);

/* One-shot layout transition (no data copy). Used when an image has
 * no initial_data but must still be in SHADER_READ_ONLY_OPTIMAL. */
flux_result flux_vk_transition_image_layout(flux_device *d, VkImage img, VkImageLayout old_layout,
                                            VkImageLayout new_layout);

/* One-shot submit infrastructure (bodies in oneshot.c). Shared with
 * flux_surface_read_pixels in surface.c; declared here rather than
 * static so both oneshot.c and surface.c reach the same code. */
bool flux_vk_prefer_transfer_queue(const flux_device *d);
VkResult flux_vk_new_transient_cmd(flux_device *d, uint32_t family, VkCommandPool *out_pool,
                                   VkCommandBuffer *out_cmd);
/* Submit and wait helpers guarantee that, once they return, the submitted
 * command buffer and synchronization objects are no longer pending and may be
 * destroyed. A finite fence timeout falls back to queue-idle before cleanup;
 * the original VK_TIMEOUT is preserved for the caller. */
VkResult flux_vk_submit_and_wait(flux_device *d, VkQueue queue, VkCommandBuffer cmd,
                                 VkSemaphore wait_sem, VkPipelineStageFlags2 wait_stage,
                                 VkSemaphore signal_sem, VkPipelineStageFlags2 signal_stage);
VkResult flux_vk_submit_one_shot_and_wait(flux_device *d, VkCommandBuffer cmd);

/* Allocator helpers — routed through device->allocator if set, else
 * libc. flux_internal_alloc always returns zeroed memory regardless
 * of allocator origin. */
void *flux_internal_alloc(flux_device *d, size_t bytes);
void flux_internal_free(flux_device *d, void *ptr);

#include <string.h> /* memset for flux_internal_alloc zeroing */

/* ------------------------------------------------------------------ */
/*  Surface + per-frame state                                         */
/* ------------------------------------------------------------------ */

#define FLUX_MAX_FRAMES_IN_FLIGHT 3
#define FLUX_MAX_TIMESTAMPS_PER_FRAME 64

typedef struct flux_timestamp_scope {
    const char *label; /* caller-owned string; assumed stable */
    uint32_t begin_query;
    uint32_t end_query; /* UINT32_MAX while open */
} flux_timestamp_scope;

typedef struct flux_per_frame {
    VkCommandPool pool;
    VkCommandBuffer cmd;
    VkSemaphore image_acquired; /* binary; signalled by acquire */
    VkFence in_flight;          /* CPU waits here before reuse  */

    /* Timestamp queries: this frame's region in the pool is
     * [slot * MAX, (slot+1) * MAX). begin/end use even/odd indices. */
    VkQueryPool query_pool;
    uint32_t ts_next;           /* next free query in region   */
    uint32_t ts_open_stack[16]; /* scope index stack for end   */
    uint32_t ts_open_top;
    uint32_t ts_scope_count;
    flux_timestamp_scope ts_scopes[FLUX_MAX_TIMESTAMPS_PER_FRAME];
    bool ts_was_submitted;
    flux_timestamp_result ts_results[FLUX_MAX_TIMESTAMPS_PER_FRAME];
    uint32_t ts_result_count;
} flux_per_frame;

typedef struct flux_transient_ring {
    VkBuffer buffer;
    flux_vk_alloc alloc;
    VkDeviceSize total_size;
    VkDeviceSize per_frame_size;
    uint8_t *mapped;
    VkDeviceAddress device_address;
    /* Per-frame cursor (bytes used in slot, 0 at frame start). */
    VkDeviceSize cursor[FLUX_MAX_FRAMES_IN_FLIGHT];
} flux_transient_ring;

typedef enum flux_frame_state {
    FLUX_FRAME_STATE_INVALID = 0,
    FLUX_FRAME_STATE_RECORDING,
    FLUX_FRAME_STATE_SUBMITTED,
    FLUX_FRAME_STATE_PRESENTED,
} flux_frame_state;

struct flux_frame {
    flux_surface *surface; /* not retained — surface owns the frame slot */
    uint32_t slot;         /* 0..frames_in_flight-1; matches per_frame[] */
    flux_frame_state state;
    bool pass_active; /* true between begin_pass and end_pass */
};

struct flux_surface {
    atomic_uint ref_count;
    flux_device *device; /* retained */

    VkSurfaceKHR vk_surface; /* VK_NULL_HANDLE for offscreen surfaces */
    bool vsync;
    bool hdr_preferred;
    bool offscreen; /* ADR-0013: no swapchain, surface-owned images */

    VkSwapchainKHR swapchain;
    VkFormat format;
    VkColorSpaceKHR color_space;
    VkExtent2D extent;
    uint32_t image_count;
    uint32_t image_capacity;
    VkImage *images;
    VkImageView *image_views;
    VkImageLayout *image_layouts;
    /* Exportable offscreen images are released to FOREIGN on export. The host
     * must not let a frame slot be reused until its external consumer has
     * released the dma-buf; begin_frame then records the matching acquire. */
    bool *image_foreign_owned;
    /* Present-wait semaphores are indexed by acquired swapchain image, not by
     * frame slot. Reacquiring an image proves its prior presentation has
     * finished consuming the corresponding semaphore. */
    VkSemaphore *render_finished;
    flux_vk_alloc *image_allocs; /* offscreen only */
    /* Offscreen dmabuf-export metadata (ADR-0040 follow-on). When
     * offscreen images are created exportable (external-memory + DRM
     * modifier), these record the negotiated modifier so the host can
     * build a matching zwp_linux_buffer_params. exportable == false for
     * windowed surfaces and when the device lacks the needed extensions. */
    bool offscreen_exportable;
    uint64_t offscreen_modifier; /* DRM_FORMAT_MOD_* (0 = invalid) */
    uint32_t offscreen_stride;   /* bytes per row of plane 0 */
    bool hdr_actual;

    /* Offscreen: slot of the most recently submitted frame, or
     * UINT32_MAX before the first submit. flux_surface_read_pixels
     * waits on this slot's fence. */
    uint32_t last_submitted_slot;

    uint32_t frames_in_flight;
    flux_per_frame frames[FLUX_MAX_FRAMES_IN_FLIGHT];
    uint32_t current_frame; /* 0..frames_in_flight-1 */
    uint32_t current_image; /* swapchain image index */

    /* Embedded frame slot returned by flux_surface_begin_frame so the
     * caller gets a stable pointer without a separate allocation.
     * Surface-owned (not thread-local) so a single thread driving
     * multiple surfaces stays correct. Vulkan permits one in-flight
     * frame per surface, so one slot per surface is enough. */
    struct flux_frame frame_slot;
    bool frame_active;
    /* Set when a windowed frame fails after acquiring a swapchain image. Such
     * an image/semaphore pair cannot be portably abandoned; resize recreates
     * the swapchain synchronization before recording resumes. */
    bool needs_recreate;

    /* Transient memory ring (host-visible, mapped). */
    flux_transient_ring transient;
};

flux_result flux_transient_ring_init(flux_transient_ring *r, flux_device *d,
                                     VkDeviceSize per_frame);
void flux_transient_ring_destroy(flux_transient_ring *r, flux_device *d);

flux_result flux_surface_create_swapchain(flux_surface *s, uint32_t w, uint32_t h);
void flux_surface_destroy_swapchain(flux_surface *s);

#endif /* FLUX_CORE_INTERNAL_H */
