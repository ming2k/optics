/*
 * Internal device layout. Never installed.
 */
#ifndef FLUX_CORE_INTERNAL_H
#define FLUX_CORE_INTERNAL_H

#include <flux/core.h>
#include <flux/math.h>   /* flux_mat4 (flux_frame scene caches) */
#include <flux/vulkan.h> /* flux_bindless_handle */
#include <stdatomic.h>
#include <vulkan/vulkan.h>

#include "platform.h" /* flux_platform_mutex + friends (OS shims) */

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
/* prefersDedicated is honoured only at or above this size: some drivers
 * set the preference liberally, and dedicating every small allocation
 * would burn the per-process vkAllocateMemory count the pool exists to
 * protect. requiresDedicated is always honoured regardless of size. */
#define FLUX_VK_PREFER_DEDICATED_MIN (1ull * 1024 * 1024) /* 1 MiB */

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
    flux_platform_mutex lock;
    bool lock_initialized;
    /* Diagnostics — counted under the lock. */
    uint64_t bytes_in_use;
    uint64_t bytes_reserved;
    uint64_t lost_ranges_bytes; /* bytes lost due to OOM on free-list node alloc */
    uint32_t live_allocations;
    uint32_t live_blocks;
    uint32_t block_seq; /* monotonically increasing, for debug names */
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

/* Account for a VkDeviceMemory living outside the slab (dma-buf import
 * or export): note_external counts it up, unnote_external counts it
 * back down. Keeps bytes_in_use / live_allocations honest so the
 * teardown leak warning and flux_device_memory_stats see external
 * memory too. Thread-safe. */
void flux_vk_allocator_note_external(flux_device *d, VkDeviceSize bytes);
void flux_vk_allocator_unnote_external(flux_device *d, VkDeviceSize bytes);

/* Driver dedication hint for flux_vk_allocate, sourced from
 * VkMemoryDedicatedRequirements chained on the resource's
 * vkGet*MemoryRequirements2. When the driver requires or prefers a
 * dedicated allocation for the resource, the request bypasses the slab
 * and chains VkMemoryDedicatedAllocateInfo on the resource handle. */
typedef struct flux_vk_dedication {
    VkBuffer buffer; /* At most one of buffer/image set. */
    VkImage image;
    bool required;
    bool preferred;
} flux_vk_dedication;

flux_result flux_vk_allocate(flux_device *d, VkMemoryRequirements mr,
                             VkMemoryPropertyFlags wanted_flags, bool is_image,
                             bool wants_device_address, const flux_vk_dedication *dedication,
                             flux_vk_alloc *out);

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
    flux_platform_mutex lock;
    bool lock_initialized;
} flux_bindless_heap;

/* One released resource parked until the graphics queue has provably
 * retired every batch that could still reference it. All fields are
 * destroyed exactly as the owning release path used to destroy them
 * inline; members the resource does not own stay zeroed. */
typedef struct flux_retire_zombie {
    VkImageView view;
    VkImage image;
    VkBuffer buffer;
    VkSampler sampler;
    VkPipeline pipeline; /* destroy-inline resources (pipelines) parked by
                          * flux_pipeline_release_deferred */
    flux_vk_alloc alloc;
    VkDeviceMemory imported_memory;
    VkDeviceSize imported_size; /* bytes to uncount from allocator stats
                                 * when imported_memory is freed; 0 when
                                 * imported_memory is VK_NULL_HANDLE */
    uint32_t bindless;          /* FLUX_BINDLESS_INVALID when unset */
    uint32_t bindless_storage;  /* FLUX_BINDLESS_INVALID when unset */
    uint64_t retire_after;      /* destroy once completed_serial >= this */
    struct flux_retire_zombie *next;
} flux_retire_zombie;

/* Backpressure bound for the retire FIFO (see flux_device and
 * zombie_park in device.c). Sized so normal operation — a few frames
 * in flight times the per-frame release churn of pools and atlases —
 * never reaches it; only a frozen watermark (host stops submitting
 * while still releasing, or the GPU is wedged) can, and then the
 * forced full drain is the intended behaviour. */
#define FLUX_RETIRE_MAX_PENDING 4096u

/* One idle staging buffer in the device cache (see flux_device).
 * Buffer + allocation stay alive and mapped while cached. */
typedef struct flux_staging_buf {
    VkBuffer buffer;
    flux_vk_alloc alloc;
    VkDeviceSize capacity;
    VkBufferUsageFlags usage;
    struct flux_staging_buf *next;
} flux_staging_buf;

/* One idle transient command pool in the device cache (see
 * flux_device). The pool is parked already reset, ready for
 * flux_vk_new_transient_cmd to hand out again. */
typedef struct flux_transient_pool {
    VkCommandPool pool;
    uint32_t family; /* queue family the pool was created for */
    struct flux_transient_pool *next;
} flux_transient_pool;

/* Command buffers recorded from a transient pool that must be freed
 * (not merely reset) once the batch that submitted them retires.
 * Drivers keep per-command-buffer driver-side state alive until the
 * buffer is explicitly freed or its pool destroyed; resetting the pool
 * returns the buffers to the initial state but keeps them allocated,
 * so a reset-and-reallocate cycle grows driver memory without bound
 * (observed on Intel ANV: ~72 KiB per cycle). Callers therefore pass
 * each submitted command buffer here so the recycle path can free it
 * before the pool returns to (and is re-acquired from) the cache. */
typedef struct flux_transient_cmdbufs {
    VkCommandBuffer cmd;  /* primary; VK_NULL_HANDLE allowed */
    VkCommandBuffer cmd2; /* QFOT graphics-side buffer; usually NULL */
} flux_transient_cmdbufs;

/* One submitted-but-not-yet-retired upload batch. Upload submissions are
 * deferred: the one-shot helpers and flux_uploads_flush hand the graphics
 * (or transfer) queue their copy commands and return without a fence wait,
 * parking everything the batch references here. The fence signals when the
 * GPU has retired the batch; entries are then recycled lazily by the
 * non-blocking sweep at the next upload call, or forcibly by
 * flux_vk_upload_pending_drain (diagnostics + device teardown). Recycling
 * any earlier is a use-after-submit: the staging buffers are still being
 * read and the command pools still executing. */
typedef struct flux_upload_pending {
    VkFence fence;              /* signals when the copy batch retired */
    VkCommandPool pool;         /* returned to the transient pool cache on
                                 * recycle; VK_NULL_HANDLE allowed */
    VkCommandPool pool2;        /* QFOT graphics-side pool; usually NULL */
    uint32_t pool_family;       /* queue family of pool; UINT32_MAX when
                                 * unknown (destroy instead of caching) */
    uint32_t pool2_family;      /* same for pool2 */
    VkCommandBuffer pool_cmd;   /* command buffer allocated from `pool`;
                                 * freed on recycle (see flux_transient_cmdbufs) */
    VkCommandBuffer pool2_cmd;  /* same for `pool2` */
    VkSemaphore sem;            /* QFOT handoff semaphore; usually NULL */
    flux_staging_buf *stagings; /* returned to the staging cache on recycle */
    uint64_t serial;            /* graphics submission serial; 0 for non-graphics batches */
    struct flux_upload_pending *next;
} flux_upload_pending;

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
    bool large_points_enabled;
    VkDeviceSize buffer_image_granularity;

    /* Vulkan handles */
    VkInstance instance;
    VkDebugUtilsMessengerEXT debug_messenger; /* VK_NULL_HANDLE if validation off */
    /* VK_EXT_debug_utils is enabled whenever the instance advertises it
     * (validation or not) so objects stay nameable under RenderDoc and
     * validation captures alike; pfn_set_name is NULL when unavailable. */
    bool has_debug_utils;
    /* VK_EXT_hdr_metadata enabled at device creation when advertised
     * (ADR-0069); gates vkSetHdrMetadataEXT on HDR swapchains. */
    bool has_hdr_metadata;
    PFN_vkSetDebugUtilsObjectNameEXT pfn_set_name;
    PFN_vkSetHdrMetadataEXT pfn_set_hdr_metadata; /* NULL unless has_hdr_metadata */

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
    flux_device_feature_flags enabled_features;
    flux_drm_device_identity drm_identity;

    uint32_t graphics_family;
    uint32_t graphics_queue_timestamp_valid_bits; /* 0 = timestamps unsupported */
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
     * issued by those helpers; the frame path acquires it too.
     * vkDeviceWaitIdle / vkQueueWaitIdle also count as host access to
     * the queues (observed as an ANV double-free when a wait raced a
     * concurrent submit), so they must go through flux_vk_wait_idle,
     * which funnels them through this same lock. */
    flux_platform_mutex queue_lock;
    bool queue_lock_initialized;

    /* Reusable external SYNC_FD semaphores for per-frame dma-buf acquire
     * waits. A semaphore enters this pool only after the frame-slot fence
     * proves its temporary imported payload was consumed. */
    flux_platform_mutex dmabuf_acquire_pool_lock;
    bool dmabuf_acquire_pool_lock_initialized;
    VkSemaphore *dmabuf_acquire_pool;
    uint32_t dmabuf_acquire_pool_count;
    uint32_t dmabuf_acquire_pool_capacity;

    /* Deferred resource destruction (retire queue). A released image or
     * buffer may still be referenced by batches in flight on the
     * graphics queue; destroying the Vulkan handle or freeing its memory
     * at release time can fault the engine mid-batch (observed on i915
     * as a GPU hang and context reset, surfaced to hosts as a fence
     * timeout followed by VK_ERROR_DEVICE_LOST). Released resources are
     * parked as zombies tagged with the graphics submission serial and
     * destroyed only once a fence wait proves the queue passed every
     * batch that could reference them. */
    atomic_uint_fast64_t submit_serial;    /* graphics-queue batch counter */
    atomic_uint_fast64_t completed_serial; /* highest batch proven done    */
    flux_platform_mutex retire_lock;
    bool retire_lock_initialized;
    flux_retire_zombie *retire_head; /* FIFO; tags are non-decreasing */
    flux_retire_zombie **retire_tail;
    /* Live entries on the FIFO. Backpressure bound: parking more than
     * FLUX_RETIRE_MAX_PENDING forces a full drain (see zombie_park), so
     * a host that releases resources while submitting no frames cannot
     * grow GPU memory without bound. Frames-in-flight operation stays
     * orders of magnitude below it. */
    uint32_t retire_pending;

    /* Batched uploads (public flux_uploads_begin/flush). While a batch
     * is open, the one-shot upload helpers record their copies into
     * batch_cmd instead of submitting individually; flush submits once.
     * Staging buffers are checked out of the staging cache per upload
     * (memcpy stays parallel across threads) and returned once the
     * flush's parked fence proves the GPU retired the batch.
     * upload_lock serialises batch state and all vkCmd* recording into
     * batch_cmd. */
    flux_platform_mutex upload_lock;
    bool upload_lock_initialized;
    bool upload_batch_open;
    /* Nesting depth of flux_uploads_begin without a matching flush. A
     * host that opens several upload scopes per frame (a compositor
     * batching per draw pass) reuses the single open batch instead of
     * submitting and later recycling one command pool per scope: each
     * recycled pool costs a vkResetCommandPool on the next frame's
     * sweep, which measured as a top CPU consumer on the frame path. */
    uint32_t upload_batch_depth;
    VkCommandPool upload_batch_pool;         /* VK_NULL_HANDLE when no batch ever opened */
    VkCommandBuffer upload_batch_cmd;        /* recording while open */
    flux_staging_buf *upload_batch_stagings; /* checked-out list, `next` chained */

    /* Deferred upload retirement. Every one-shot upload submission parks
     * a flux_upload_pending here instead of blocking the caller on a
     * fence. Guarded by its own lock; recycled by sweep (non-blocking,
     * called from the upload entry points) or drain (waits, called from
     * flux_device_memory_stats and device teardown). */
    flux_platform_mutex upload_pending_lock;
    bool upload_pending_lock_initialized;
    flux_upload_pending *upload_pending_head;

    /* Staging buffer cache. The one-shot upload / readback helpers reuse
     * a host-visible staging buffer per copy; buffers are checked out
     * while an upload is in flight and returned by the pending-upload
     * sweep once the copy's fence signals (see above), so the cache
     * avoids a vkCreateBuffer + vkAllocateMemory pair per upload without
     * ever handing a buffer back while the GPU still reads it.
     * Idle entries are matched by usage and smallest-fit capacity, and
     * the cache is capped (see FLUX_VK_STAGING_CACHE_CAP in oneshot.c).
     * Checked-out entries are owned by the caller; the idle list is
     * drained at device teardown, after the device is known idle. */
    flux_platform_mutex staging_lock;
    bool staging_lock_initialized;
    flux_staging_buf *staging_idle;
    uint64_t staging_idle_bytes;

    /* Open one-shot recordings started by the public flux_oneshot_begin
     * (oneshot.c). Maps the handed-out VkCommandBuffer back to its
     * transient pool so submit_and_end can recycle it. Guarded by
     * staging_lock (a leaf lock, same as the pool cache below). */
    struct {
        VkCommandPool pool;
        VkCommandBuffer cmd;
    } *oneshot_slots;
    uint32_t oneshot_slot_count;

    /* Transient command pool cache (oneshot.c). One-shot submissions
     * (uploads, layout transitions, readback) used to create + destroy
     * a VkCommandPool each time; the text atlas flush hits that path
     * every frame a new glyph appears. Pools are parked here — reset,
     * not destroyed — once the GPU provably retired every batch
     * recorded from them (the pending-upload fence), and handed out by
     * queue family. Shares staging_lock (a leaf lock, same recycle
     * paths); drained at device teardown by flux_vk_staging_pool_destroy
     * once the device is idle. */
    flux_transient_pool *transient_pool_idle;
    uint32_t transient_pool_idle_count;

    /* Protects publication of lazily-created per-module state slots. The lock
     * is held only while reading or publishing pointers and hooks; allocation,
     * Vulkan calls, and module locks must remain outside it. */
    flux_platform_mutex module_state_lock;
    bool module_state_lock_initialized;

    /* Vulkan requires external synchronisation for every operation that reads
     * or mutates the shared VkPipelineCache, including pipeline creation and
     * vkGetPipelineCacheData. Raw-cache users lock it through vulkan.h. */
    flux_platform_mutex pipeline_cache_lock;
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
     * accessible via flux_device_default_sampler_handle.
     * `default_sampler_handle` is read on every glyph-run / image draw
     * (the hot text path), so it is stored as an atomic: readers load
     * acquire and skip the bindless lock entirely once populated; the
     * single writer stores release inside the lock. */
    VkSampler default_sampler;
    _Atomic flux_bindless_handle default_sampler_handle;

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

/* Staging cache (bodies in oneshot.c). acquire returns a mapped
 * host-visible buffer of at least `size` bytes with `usage`, either
 * recycled from the idle list or freshly created; release returns it to
 * the cache (or destroys it once the cache cap is exceeded). Callers
 * must only release a buffer once the GPU work consuming it has provably
 * retired — the deferred upload submissions provide that by parking the
 * buffer on the pending list until its fence signals. */
flux_result flux_vk_staging_acquire(flux_device *d, VkDeviceSize size, VkBufferUsageFlags usage,
                                    flux_staging_buf **out);
void flux_vk_staging_release(flux_device *d, flux_staging_buf *sb);
/* Destroy every idle staging entry and every parked transient command
 * pool. Device must be idle (teardown only). */
void flux_vk_staging_pool_destroy(flux_device *d);

/* Deferred upload retirement (bodies in oneshot.c). drain waits on every
 * parked upload fence and recycles all entries — used by
 * flux_device_memory_stats (deterministic numbers) and by device
 * teardown once the device is idle. The upload entry points run the
 * non-blocking sweep internally. */
void flux_vk_upload_pending_drain(flux_device *d);

/* Fence + queue submission shared by the deferred upload paths (bodies
 * in oneshot.c). submit_upload submits `cmd` on `queue` with optional
 * wait/signal semaphores; on success *out_fence (when non-NULL) receives
 * the fence that signals when the batch retires, on failure nothing is
 * pending. upload_pending_park takes ownership of the batch's resources
 * (fence, command pools, handoff semaphore, staging list) and recycles
 * them once the fence signals. Used cross-module by the dma-buf import
 * path, whose acquire-fence transition must not block the caller. */
VkResult flux_vk_submit_upload(flux_device *d, VkQueue queue, VkCommandBuffer cmd,
                               VkSemaphore wait_sem, VkPipelineStageFlags2 wait_stage,
                               VkSemaphore signal_sem, VkPipelineStageFlags2 signal_stage,
                               VkFence *out_fence, uint64_t *out_serial);
void flux_vk_upload_pending_park(flux_device *d, VkFence fence, VkCommandPool pool,
                                 VkCommandPool pool2, VkSemaphore sem, flux_staging_buf *stagings,
                                 uint64_t serial);
/* Same as flux_vk_upload_pending_park but tags each pool with its queue
 * family so the recycle path can return it to the transient pool cache
 * instead of destroying it. Pass UINT32_MAX for an unknown family.
 * pool_cmd/pool2_cmd are the command buffers allocated from the pools
 * (VK_NULL_HANDLE when none); the recycle path frees them before the
 * pools return to the cache, because a reset pool keeps its allocated
 * command buffers alive and re-allocation then grows driver memory
 * without bound on some drivers (see flux_transient_cmdbufs). */
void flux_vk_upload_pending_park_families(flux_device *d, VkFence fence, VkCommandPool pool,
                                          uint32_t pool_family, VkCommandBuffer pool_cmd,
                                          VkCommandPool pool2, uint32_t pool2_family,
                                          VkCommandBuffer pool2_cmd, VkSemaphore sem,
                                          flux_staging_buf *stagings, uint64_t serial);

/* Upload batch internals (public entry points are flux_uploads_begin /
 * flux_uploads_flush). Upload helpers record into the batch while
 * d->upload_batch_open (guarded by upload_lock). */
void flux_vk_upload_batch_attach_staging(flux_device *d, flux_staging_buf *sb);

/* One-shot host -> device upload via a graphics-queue command buffer
 * (uses the graphics queue rather than transfer to avoid queue-family
 * ownership transfer plumbing in this simple path). Copies through a
 * cached host-visible staging buffer (flux_vk_staging_acquire), records
 * vkCmdCopyBuffer to dst, and submits deferred — the caller returns
 * immediately and the staging buffer is recycled once the copy's parked
 * fence signals. */
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

/* Graphics submission ordering for the retire queue (bodies in device.c).
 * note_submission returns the serial assigned to a batch just submitted
 * on the graphics queue; note_completed raises the completed watermark
 * (all graphics batches complete in FIFO order) and sweeps zombies whose
 * tag is covered. note_completed(0) is a harmless no-op for slots that
 * have never submitted. */
uint64_t flux_vk_note_graphics_submission(flux_device *d);
void flux_vk_note_graphics_completed(flux_device *d, uint64_t serial);

/* Park a released image's Vulkan pieces for deferred destruction. Takes
 * ownership of view/image/alloc/imported_memory; the bindless slots are
 * freed when the zombie is destroyed. imported_size is the byte count
 * previously noted via flux_vk_allocator_note_external for
 * imported_memory (0 when VK_NULL_HANDLE). Thread-safe. */
void flux_device_retire_image(flux_device *d, VkImageView view, VkImage image,
                              const flux_vk_alloc *alloc, VkDeviceMemory imported_memory,
                              VkDeviceSize imported_size, uint32_t bindless,
                              uint32_t bindless_storage);

/* Park a VkPipeline for deferred destruction (flux_pipeline_release_deferred
 * and flux_graphics_pipeline_release_deferred). Destroys it once every
 * submitted graphics batch has completed. Thread-safe. */
void flux_vk_retire_pipeline(flux_device *d, VkPipeline pipeline);
/* Park a released buffer's Vulkan pieces for deferred destruction. Takes
 * ownership of buffer/alloc. Thread-safe. */
void flux_device_retire_buffer(flux_device *d, VkBuffer buffer, const flux_vk_alloc *alloc);

/* Park a released sampler for deferred destruction. The bindless slot and
 * the VkSampler are freed only once the queue provably passed every batch
 * whose push constants could still carry the slot number — an inline
 * release would let the slot be recycled mid-flight (silent mis-sampling)
 * and violates VUID-vkDestroySampler-sampler-01070. Thread-safe. */
void flux_device_retire_sampler(flux_device *d, VkSampler sampler, uint32_t bindless);

/* vkDeviceWaitIdle serialised against concurrent queue submissions via
 * queue_lock. Every wait-idle in the library must go through this helper
 * (see the queue_lock comment in flux_device). */
void flux_vk_wait_idle(flux_device *d);

/* Best-effort debug name for a Vulkan object (VK_EXT_debug_utils).
 * No-op when the extension is unavailable. Names show up in RenderDoc
 * captures and validation messages. */
void flux_vk_set_name(flux_device *d, VkObjectType type, uint64_t handle, const char *name);
/* Destroy every zombie whose tag is covered by completed_serial. */
void flux_device_sweep_retire(flux_device *d);
/* Unconditionally destroy all parked zombies (device teardown, after the
 * device is known idle). */
void flux_device_drain_retire(flux_device *d);

/* Allocator helpers — routed through device->allocator if set, else
 * libc. flux_internal_alloc always returns zeroed memory regardless
 * of allocator origin. */
void *flux_internal_alloc(flux_device *d, size_t bytes);
void flux_internal_free(flux_device *d, void *ptr);

#include <string.h> /* memset for flux_internal_alloc zeroing */

/* ------------------------------------------------------------------ */
/*  Surface + per-frame state                                         */
/* ------------------------------------------------------------------ */

#define FLUX_MAX_TIMESTAMPS_PER_FRAME 64

typedef struct flux_timestamp_scope {
    const char *label; /* caller-owned string; assumed stable */
    uint32_t begin_query;
    uint32_t end_query; /* UINT32_MAX while open */
} flux_timestamp_scope;

typedef void *(*flux_frame_resource_retain_fn)(void *resource);
typedef void (*flux_frame_resource_release_fn)(void *resource);

typedef struct flux_frame_foreign_image {
    VkImage image;
    void *resource;
    flux_frame_resource_release_fn release;
    bool *foreign_owned;
    bool acquired;
    /* Optional one-shot sync_file import for this use of a reusable
     * dma-buf image. Recycled only after the frame-slot fence retires and a
     * successful queue submit proves the temporary payload was consumed. */
    VkSemaphore acquire_semaphore;
    bool acquire_wait_submitted;
} flux_frame_foreign_image;

typedef struct flux_per_frame {
    VkCommandPool pool;
    VkCommandBuffer cmd;
    VkCommandBuffer foreign_acquire_cmd;
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

    /* Graphics-queue submission serial of the batch guarded by
     * in_flight; a successful wait on in_flight retires everything up
     * to this serial (see the device retire queue). */
    uint64_t submitted_serial;

    /* Images sampled by this batch. Every image stays retained through the
     * slot fence, so atlas replacement and other mid-frame owner releases
     * cannot invalidate a recorded bindless handle. Imported dma-bufs also
     * carry the optional FOREIGN ownership bookkeeping below. */
    flux_frame_foreign_image *foreign_images;
    /* Submit-time storage. One extra entry accommodates the window-system
     * image-acquired semaphore before the foreign-image waits. */
    VkSemaphoreSubmitInfo *foreign_waits;
    uint32_t foreign_image_count;
    uint32_t foreign_image_capacity;
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

struct flux_frame {
    flux_surface *surface; /* not retained — surface owns the frame slot */
    uint32_t slot;         /* 0..frames_in_flight-1; matches per_frame[] */
    flux_frame_state state;
    bool pass_active; /* true between begin_pass and end_pass */
    bool readback_requested;
    /* Exact physical-pixel source region copied when readback_requested is
     * true. Full-frame compatibility requests populate the surface extent. */
    flux_readback_region readback_region;

    /* Scene-draw caches (scene.c). begin_frame re-initialises this
     * whole struct, so they are per-frame by construction.
     *
     * scene_bound_* mirror the command buffer's current pipeline /
     * descriptor bindings so scene_draw can skip redundant rebinds
     * (the canvas backend does the same per pass). Other modules'
     * passes bind their own pipelines on this command buffer (the
     * canvas end_pass output blit always does), and bindings persist
     * across passes — so the mirror is only valid within one pass and
     * flux_frame_begin_pass resets it, forcing the first scene draw of
     * each pass to rebind.
     *
     * scene_view_* cache inverse(camera.view): one memcmp per draw
     * instead of a 4x4 inverse for the common static-camera pass. */
    VkPipeline scene_bound_pipeline;
    VkDescriptorSet scene_bound_set;
    VkPipelineLayout scene_bound_layout;
    flux_mat4 scene_view_src;
    flux_mat4 scene_view_inv;
    bool scene_view_inv_valid;
};

/* Recorders call this before sampling an image. The first use in a frame
 * retains resource until the frame slot's fence retires. `foreign_owned` is
 * NULL for ordinary images; for imported dma-bufs, flux_frame_submit prepends
 * a FOREIGN -> graphics acquire when necessary and records the matching
 * graphics -> FOREIGN release after all sampling. */
bool flux_frame_track_foreign_image(flux_frame *frame, VkImage image, void *resource,
                                    flux_frame_resource_retain_fn retain,
                                    flux_frame_resource_release_fn release, bool *foreign_owned);
/* Attach a one-shot semaphore to an image already tracked by this frame.
 * Takes ownership of `semaphore` only on success. */
bool flux_frame_set_foreign_image_acquire(flux_frame *frame, void *resource, VkSemaphore semaphore);

/* External SYNC_FD semaphore pool used by reusable dma-buf per-frame waits. */
VkSemaphore flux_dmabuf_acquire_semaphore_take(flux_device *device);
void flux_dmabuf_acquire_semaphore_recycle(flux_device *device, VkSemaphore semaphore);
void flux_dmabuf_acquire_semaphore_pool_destroy(flux_device *device);
void flux_frame_foreign_images_destroy(flux_surface *surface, flux_per_frame *per_frame);

/* Cross-TU bodies shared between the canvas-level
 * flux_canvas_wait_dmabuf_acquire (src/canvas/dmabuf_acquire.c) and the
 * platform pair src/core/dmabuf.c / dmabuf_stub.c. import_acquire_semaphore
 * imports `fd` as a temporary SYNC_FD payload on a pooled semaphore
 * (FLUX_ERROR_UNSUPPORTED from the stub); close_fd releases the caller's
 * sync_file fd after a successful import (no-op off Linux). Keeping the
 * platform fork here leaves the canvas call site free of #ifdef. */
flux_result flux_dmabuf_import_acquire_semaphore(flux_device *d, int fd, VkSemaphore *out);
void flux_dmabuf_close_fd(int fd);

/* ADR-0070: the profile's baked 65³ working-space LUT (R-fastest),
 * or NULL when the profile is parametric-only. */
const float *flux_icc_profile_lut(const flux_icc_profile *p, uint32_t *out_size);

struct flux_surface {
    atomic_uint ref_count;
    flux_device *device; /* retained */

    VkSurfaceKHR vk_surface; /* VK_NULL_HANDLE for offscreen surfaces */
    bool vsync;
    bool hdr_preferred;
    bool offscreen; /* ADR-0013: no swapchain, surface-owned images */
    bool readback_supported;

    VkSwapchainKHR swapchain;
    VkFormat format;
    VkColorSpaceKHR color_space;
    VkExtent2D extent;
    uint32_t image_count;
    uint32_t image_capacity;
    VkImage *images;
    VkImageView *image_views;
    VkImageLayout *image_layouts;
    /* Bindless sampled-image handle per surface image (ADR-0069 canvas
     * LOAD seed); FLUX_BINDLESS_INVALID until registered. */
    flux_bindless_handle *image_bindless;
    /* Exportable offscreen images are released to FOREIGN on export. The host
     * must not let a frame slot be reused until its external consumer has
     * released the dma-buf; begin_frame then records the matching acquire. */
    bool *image_foreign_owned;
    /* Exportable offscreen images signal one SYNC_FD-capable semaphore per
     * slot. This flag prevents exporting its temporary payload twice before
     * the slot is reacquired and submitted again. */
    bool *image_sync_exported;
    /* Present-wait semaphores are indexed by acquired swapchain image, not by
     * frame slot. Reacquiring an image proves its prior presentation has
     * finished consuming the corresponding semaphore. */
    VkSemaphore *render_finished;
    flux_vk_alloc *image_allocs; /* offscreen only */
    /* Offscreen dmabuf-export metadata (see ADR-0013 offscreen surface). When
     * offscreen images are created exportable (external-memory + DRM
     * modifier), these record the negotiated modifier so the host can
     * build a matching zwp_linux_buffer_params. exportable == false for
     * windowed surfaces and when the device lacks the needed extensions. */
    bool offscreen_exportable;
    /* Surface creation was pinned to CPU readback (flux_surface_readback_desc):
     * never make its offscreen images dma-buf exportable, so read_pixels
     * always works even on a dma-buf-capable device. */
    bool offscreen_require_readback;
    /* Persistent host-visible readback destination. require_readback surfaces
     * fill it every frame; other surfaces allocate and fill it only when the
     * recording frame requests a snapshot. The copy is part of the original
     * graphics command buffer, so readiness is the frame fence and CPU
     * readback needs no second queue submission. */
    flux_staging_buf *readback_staging;
    /* Region most recently copied into readback_staging. Valid exactly when
     * last_readback_slot != UINT32_MAX. */
    flux_readback_region last_readback_region;
    uint64_t offscreen_modifier; /* DRM_FORMAT_MOD_* (0 = invalid) */
    uint32_t offscreen_stride;   /* bytes per row of plane 0 */
    /* Optional consumer modifier constraint copied from the surface extension
     * at creation and retained so resize preserves the same contract. */
    uint64_t *offscreen_allowed_modifiers;
    uint32_t offscreen_allowed_modifier_count;
    /* Optional offscreen container constraint copied from
     * flux_surface_offscreen_format_desc; NULL/0 = transfer-derived
     * defaults. Retained so resize re-negotiates the same contract. */
    flux_format *offscreen_formats;
    uint32_t offscreen_format_count;
    bool hdr_actual;

    /* ADR-0069: the color space the surface presents in, and the
     * caller's preference list (copied from flux_surface_color_space_desc;
     * NULL/0 = legacy hdr_preferred mapping). Retained so resize
     * re-negotiates the same contract. */
    flux_color_space output_color_space;
    flux_color_space *requested_spaces;
    uint32_t requested_space_count;

    /* What pixels are WRITTEN in (flux_surface_output_color_desc): the
     * display's actual space on legacy platforms; equals
     * output_color_space when no override was given. The canvas output
     * transform targets this space and the LOAD seed decodes from it. */
    flux_color_space content_space;
    flux_color_space output_override; /* valid when has_output_override */
    bool has_output_override;

    /* ADR-0069 HDR presentation (from flux_surface_hdr_desc).
     * sdr_white_nits == 0 selects the 203 cd/m² default at use sites.
     * hdr_metadata is applied to every (re)created swapchain when
     * hdr_metadata_present and the device has VK_EXT_hdr_metadata. */
    float sdr_white_nits;
    bool hdr_metadata_present;
    VkHdrMetadataEXT hdr_metadata;

    /* Offscreen: slot of the most recently submitted frame, or
     * UINT32_MAX before the first submit. flux_surface_read_pixels
     * waits on this slot's fence. */
    uint32_t last_submitted_slot;
    /* Slot whose submission most recently copied into readback_staging, or
     * UINT32_MAX when no immutable snapshot is available. */
    uint32_t last_readback_slot;

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
