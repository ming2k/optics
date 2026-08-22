/*
 * One-shot GPU submission helpers.
 *
 * Used by:
 *   - flux_vk_upload_to_buffer / flux_vk_upload_to_image (initial mesh /
 *     image uploads and image sub-region updates)
 *   - flux_vk_transition_image_layout (no-data layout transitions)
 *   - flux_surface_read_pixels (offscreen readback in surface.c)
 *
 * The submit-and-wait + transient command-buffer allocators are
 * exported through internal.h because readback (in surface.c) and the
 * upload paths share them.
 */
#include "internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/*  Submit infrastructure                                             */
/*                                                                    */
/*  Shared with surface.c (readback). Bodies file-local.              */
/* ------------------------------------------------------------------ */

/* Submit `cmd` on `queue` with optional wait/signal semaphores, then wait on a
 * fresh fence. A finite timeout is still reported to the caller, but before
 * returning we wait the queue idle so every object referenced by the submit is
 * safe to destroy. This trades a potentially longer stall on a wedged driver
 * for correct Vulkan object lifetimes; a future asynchronous retire queue can
 * restore strict timeout latency without reintroducing use-after-submit. */
VkResult flux_vk_submit_and_wait(flux_device *d, VkQueue queue, VkCommandBuffer cmd,
                                 VkSemaphore wait_sem, VkPipelineStageFlags2 wait_stage,
                                 VkSemaphore signal_sem, VkPipelineStageFlags2 signal_stage) {
    VkFence fence = VK_NULL_HANDLE;
    VkFenceCreateInfo fci = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkResult fr = vkCreateFence(d->device, &fci, nullptr, &fence);
    if (fr != VK_SUCCESS)
        return fr;

    VkSemaphoreSubmitInfo wait_info = {
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
        .semaphore = wait_sem,
        .stageMask = wait_stage,
    };
    VkSemaphoreSubmitInfo signal_info = {
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
        .semaphore = signal_sem,
        .stageMask = signal_stage,
    };
    VkCommandBufferSubmitInfo cbsi = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
        .commandBuffer = cmd,
    };
    VkSubmitInfo2 si = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
        .waitSemaphoreInfoCount = wait_sem ? 1 : 0,
        .pWaitSemaphoreInfos = wait_sem ? &wait_info : nullptr,
        .signalSemaphoreInfoCount = signal_sem ? 1 : 0,
        .pSignalSemaphoreInfos = signal_sem ? &signal_info : nullptr,
        .commandBufferInfoCount = 1,
        .pCommandBufferInfos = &cbsi,
    };
    flux_platform_mutex_lock(&d->queue_lock);
    VkResult vr = vkQueueSubmit2(queue, 1, &si, fence);
    /* Graphics-queue batches retire every earlier batch in FIFO order;
     * remember this one's serial so the fence wait below can raise the
     * retire watermark. Transfer-queue submissions are unordered against
     * graphics work and must not move that watermark. */
    uint64_t graphics_serial = 0;
    if (vr == VK_SUCCESS && queue == d->graphics_queue)
        graphics_serial = flux_vk_note_graphics_submission(d);
    flux_platform_mutex_unlock(&d->queue_lock);
    if (vr == VK_SUCCESS) {
        vr = vkWaitForFences(d->device, 1, &fence, VK_TRUE, FLUX_DEFAULT_FRAME_TIMEOUT_NS);
        if (vr == VK_SUCCESS && graphics_serial)
            flux_vk_note_graphics_completed(d, graphics_serial);
        if (vr != VK_SUCCESS && vr != VK_ERROR_DEVICE_LOST) {
            VkResult wait_result = vr;
            flux_platform_mutex_lock(&d->queue_lock);
            VkResult idle = vkQueueWaitIdle(queue);
            flux_platform_mutex_unlock(&d->queue_lock);
            if (idle == VK_SUCCESS && graphics_serial)
                flux_vk_note_graphics_completed(d, graphics_serial);
            /* Preserve timeout/error semantics once completion is proven. If
             * idle itself fails, surface the stronger backend/device error. */
            vr = idle == VK_SUCCESS ? wait_result : idle;
        }
    }
    vkDestroyFence(d->device, fence, nullptr);
    return vr;
}

/* Convenience for the common single-queue case. */
VkResult flux_vk_submit_one_shot_and_wait(flux_device *d, VkCommandBuffer cmd) {
    return flux_vk_submit_and_wait(d, d->graphics_queue, cmd, VK_NULL_HANDLE, 0, VK_NULL_HANDLE, 0);
}

static flux_result submit_result(VkResult vr) {
    if (vr == VK_SUCCESS)
        return FLUX_OK;
    if (vr == VK_TIMEOUT)
        return FLUX_ERROR_TIMEOUT;
    if (vr == VK_ERROR_DEVICE_LOST)
        return FLUX_ERROR_DEVICE_LOST;
    return FLUX_ERROR_BACKEND_FAILURE;
}

/* Pick the queue family + queue for one-shot uploads. When a
 * dedicated transfer queue exists, prefer it; the destination
 * resource needs a queue-family ownership transfer barrier to the
 * graphics family before the next graphics-side use. */
bool flux_vk_prefer_transfer_queue(const flux_device *d) {
    return d->transfer_dedicated && d->transfer_family != d->graphics_family;
}

/* ------------------------------------------------------------------ */
/*  Transient command pool cache                                      */
/*                                                                    */
/*  One-shot submissions used to pay a vkCreateCommandPool +           */
/*  vkDestroyCommandPool pair apiece; the text atlas flush hits this   */
/*  path every frame a new glyph appears. Idle pools are parked on the */
/*  device — with their command buffers freed, then reset — and       */
/*  handed out by queue family. Mirrors the staging-buffer cache        */
/*  below: a checked-out pool is owned by the caller and comes back    */
/*  only once the GPU provably retired every batch recorded from it    */
/*  (the pending-upload fence, or never-submitted failure paths), so   */
/*  the reset on return can never clobber in-flight commands. The      */
/*  idle list shares staging_lock (a leaf lock taken by the same       */
/*  recycle paths) and is drained at device teardown by               */
/*  flux_vk_staging_pool_destroy. Command buffers allocated from a     */
/*  cached pool are freed before the pool is parked: a reset pool      */
/*  keeps its allocated buffers alive, and re-allocating on re-acquire */
/*  then grows per-buffer driver state without bound on Intel ANV.     */
/* ------------------------------------------------------------------ */

/* Most pools parked idle before returns destroy instead of caching.
 * One per queue family covers steady state; the cap only bounds
 * bursty concurrent one-shot uploads. */
#define FLUX_VK_TRANSIENT_POOL_CACHE_CAP 8u

/* Return a pool to the cache (family known) or destroy it (family
 * unknown — pools parked by callers of the plain park API, e.g. the
 * dma-buf import path). The caller must guarantee no batch recorded
 * from the pool is still in flight. cmd/cmd2 are command buffers still
 * allocated from the pool; they are freed (not reset) so the pool
 * returns to the cache with zero allocated command buffers. Keeping
 * them allocated across a reset+re-acquire cycle leaks driver-side
 * per-command-buffer state cumulatively on Intel ANV (~72 KiB per
 * cycle), which is why the cache cannot rely on vkResetCommandPool
 * alone. */
static void transient_pool_release(flux_device *d, VkCommandPool pool, uint32_t family,
                                   VkCommandBuffer cmd, VkCommandBuffer cmd2) {
    if (!pool)
        return;
    if (cmd || cmd2) {
        VkCommandBuffer bufs[2];
        uint32_t n = 0;
        if (cmd)
            bufs[n++] = cmd;
        if (cmd2)
            bufs[n++] = cmd2;
        vkFreeCommandBuffers(d->device, pool, n, bufs);
    }
    if (family == UINT32_MAX) {
        vkDestroyCommandPool(d->device, pool, nullptr);
        return;
    }
    vkResetCommandPool(d->device, pool, 0);
    flux_transient_pool *e = flux_internal_alloc(d, sizeof(*e));
    if (!e) {
        vkDestroyCommandPool(d->device, pool, nullptr);
        return;
    }
    e->pool = pool;
    e->family = family;
    flux_platform_mutex_lock(&d->staging_lock);
    if (d->transient_pool_idle_count >= FLUX_VK_TRANSIENT_POOL_CACHE_CAP) {
        flux_platform_mutex_unlock(&d->staging_lock);
        flux_internal_free(d, e);
        vkDestroyCommandPool(d->device, pool, nullptr);
        return;
    }
    e->next = d->transient_pool_idle;
    d->transient_pool_idle = e;
    d->transient_pool_idle_count++;
    flux_platform_mutex_unlock(&d->staging_lock);
}

/* Pop an idle pool created for `family`, VK_NULL_HANDLE when none is
 * parked. Returned pools are already reset. */
static VkCommandPool transient_pool_acquire(flux_device *d, uint32_t family) {
    flux_platform_mutex_lock(&d->staging_lock);
    for (flux_transient_pool **pp = &d->transient_pool_idle; *pp; pp = &(*pp)->next) {
        if ((*pp)->family != family)
            continue;
        flux_transient_pool *e = *pp;
        *pp = e->next;
        d->transient_pool_idle_count--;
        flux_platform_mutex_unlock(&d->staging_lock);
        VkCommandPool pool = e->pool;
        flux_internal_free(d, e);
        return pool;
    }
    flux_platform_mutex_unlock(&d->staging_lock);
    return VK_NULL_HANDLE;
}

/* Teardown only (device idle): destroy every parked pool. Called from
 * flux_vk_staging_pool_destroy, which device teardown already runs. */
static void transient_pool_cache_destroy(flux_device *d) {
    flux_transient_pool *idle = d->transient_pool_idle;
    d->transient_pool_idle = NULL;
    d->transient_pool_idle_count = 0;
    while (idle) {
        flux_transient_pool *next = idle->next;
        vkDestroyCommandPool(d->device, idle->pool, nullptr);
        flux_internal_free(d, idle);
        idle = next;
    }
}

/* Allocate a transient pool + one primary command buffer on the
 * given queue family; the pool is reused from the device cache when
 * one is parked for the family. Out params zeroed on failure. The
 * returned pool is caller-owned exactly like a fresh one: the caller
 * either destroys it or returns it through the pending-upload
 * recycle. */
VkResult flux_vk_new_transient_cmd(flux_device *d, uint32_t family, VkCommandPool *out_pool,
                                   VkCommandBuffer *out_cmd) {
    *out_pool = transient_pool_acquire(d, family);
    *out_cmd = VK_NULL_HANDLE;
    if (*out_pool == VK_NULL_HANDLE) {
        VkCommandPoolCreateInfo pci = {
            .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
            .queueFamilyIndex = family,
            .flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT,
        };
        VkResult vr = vkCreateCommandPool(d->device, &pci, nullptr, out_pool);
        if (vr != VK_SUCCESS)
            return vr;
    }
    VkCommandBufferAllocateInfo cbai = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = *out_pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };
    VkResult vr = vkAllocateCommandBuffers(d->device, &cbai, out_cmd);
    if (vr != VK_SUCCESS) {
        vkDestroyCommandPool(d->device, *out_pool, nullptr);
        *out_pool = VK_NULL_HANDLE;
        return vr;
    }
    VkCommandBufferBeginInfo cbbi = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    vr = vkBeginCommandBuffer(*out_cmd, &cbbi);
    if (vr != VK_SUCCESS) {
        vkDestroyCommandPool(d->device, *out_pool, nullptr);
        *out_pool = VK_NULL_HANDLE;
        *out_cmd = VK_NULL_HANDLE;
        return vr;
    }
    return VK_SUCCESS;
}

/* ------------------------------------------------------------------ */
/*  Staging buffer cache                                              */
/* ------------------------------------------------------------------ */

/* Total bytes the idle staging cache may hold before releases destroy
 * instead of caching. 64 MiB bounds worst-case host memory pinned by
 * idle staging while comfortably holding a few large texture uploads. */
#define FLUX_VK_STAGING_CACHE_CAP (64ull * 1024 * 1024)

static void staging_destroy(flux_device *d, flux_staging_buf *sb) {
    if (!sb)
        return;
    if (sb->buffer)
        vkDestroyBuffer(d->device, sb->buffer, nullptr);
    if (sb->alloc.memory)
        flux_vk_deallocate(d, &sb->alloc);
    flux_internal_free(d, sb);
}

flux_result flux_vk_staging_acquire(flux_device *d, VkDeviceSize size, VkBufferUsageFlags usage,
                                    flux_staging_buf **out) {
    *out = NULL;

    /* Smallest-fit idle entry with matching usage. */
    flux_platform_mutex_lock(&d->staging_lock);
    flux_staging_buf **best = NULL;
    for (flux_staging_buf **pp = &d->staging_idle; *pp; pp = &(*pp)->next) {
        if ((*pp)->usage != usage || (*pp)->capacity < size)
            continue;
        if (!best || (*pp)->capacity < (*best)->capacity)
            best = pp;
    }
    if (best) {
        flux_staging_buf *sb = *best;
        *best = sb->next;
        d->staging_idle_bytes -= sb->capacity;
        flux_platform_mutex_unlock(&d->staging_lock);
        sb->next = NULL;
        *out = sb;
        return FLUX_OK;
    }
    flux_platform_mutex_unlock(&d->staging_lock);

    flux_staging_buf *sb = flux_internal_alloc(d, sizeof(*sb));
    if (!sb)
        return FLUX_ERROR_OUT_OF_MEMORY;
    flux_result r = flux_vk_alloc_buffer(
        d, size, usage, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
        /*wants_device_address=*/false, &sb->buffer, &sb->alloc);
    if (r != FLUX_OK) {
        flux_internal_free(d, sb);
        return r;
    }
    sb->capacity = size;
    sb->usage = usage;
    sb->next = NULL;
    char name[64];
    snprintf(name, sizeof(name), "flux staging %llu KiB", (unsigned long long)(size >> 10));
    flux_vk_set_name(d, VK_OBJECT_TYPE_BUFFER, (uint64_t)sb->buffer, name);
    *out = sb;
    return FLUX_OK;
}

void flux_vk_staging_release(flux_device *d, flux_staging_buf *sb) {
    if (!sb)
        return;
    flux_platform_mutex_lock(&d->staging_lock);
    if (d->staging_idle_bytes + sb->capacity > FLUX_VK_STAGING_CACHE_CAP) {
        flux_platform_mutex_unlock(&d->staging_lock);
        staging_destroy(d, sb);
        return;
    }
    sb->next = d->staging_idle;
    d->staging_idle = sb;
    d->staging_idle_bytes += sb->capacity;
    flux_platform_mutex_unlock(&d->staging_lock);
}

void flux_vk_staging_pool_destroy(flux_device *d) {
    flux_staging_buf *idle = d->staging_idle;
    d->staging_idle = NULL;
    d->staging_idle_bytes = 0;
    while (idle) {
        flux_staging_buf *next = idle->next;
        staging_destroy(d, idle);
        idle = next;
    }
    /* Same lifetime as the idle staging entries: device is idle, so
     * parked transient pools are safe to destroy too. */
    transient_pool_cache_destroy(d);
}

/* ------------------------------------------------------------------ */
/*  Deferred upload retirement                                        */
/*                                                                    */
/*  One-shot uploads and flushed batches never block the caller on a   */
/*  fence: the copy is submitted with a fence and everything the batch */
/*  references (staging buffers, command pools, QFOT semaphores) is    */
/*  parked on d->upload_pending_head. Queue submission order alone     */
/*  makes the copies visible to later same-queue work (frames,         */
/*  readback), so no wait is needed for correctness — only for buffer  */
/*  reuse, which the fence + lazy sweep below provide.                 */
/* ------------------------------------------------------------------ */

/* Recycle the resources of one upload batch whose fence has signaled
 * (or that was never submitted). fence may be VK_NULL_HANDLE. Pools
 * with a known queue family return to the transient pool cache (the
 * fence proves their batches retired); the rest are destroyed. The
 * command buffers are freed first — see transient_pool_release. */
static void upload_pending_recycle(flux_device *d, VkFence fence, VkCommandPool pool,
                                   uint32_t pool_family, VkCommandBuffer pool_cmd,
                                   VkCommandPool pool2, uint32_t pool2_family,
                                   VkCommandBuffer pool2_cmd, VkSemaphore sem,
                                   flux_staging_buf *stagings) {
    if (fence)
        vkDestroyFence(d->device, fence, nullptr);
    transient_pool_release(d, pool, pool_family, pool_cmd, VK_NULL_HANDLE);
    transient_pool_release(d, pool2, pool2_family, pool2_cmd, VK_NULL_HANDLE);
    if (sem)
        vkDestroySemaphore(d->device, sem, nullptr);
    while (stagings) {
        flux_staging_buf *next = stagings->next;
        stagings->next = NULL;
        flux_vk_staging_release(d, stagings);
        stagings = next;
    }
}

/* Non-blocking sweep: recycle every parked upload whose fence already
 * signaled. A signaled graphics-queue fence also proves every earlier
 * graphics batch complete (the queue is FIFO), so the retire watermark
 * advances with it — this is what sweeps retire-queue zombies outside
 * the frame path. The upload entry points call this so steady-state
 * operation bounds the pending list to the uploads genuinely in
 * flight. */
static void upload_pending_sweep(flux_device *d) {
    flux_platform_mutex_lock(&d->upload_pending_lock);
    flux_upload_pending **pp = &d->upload_pending_head;
    while (*pp) {
        flux_upload_pending *p = *pp;
        if (vkGetFenceStatus(d->device, p->fence) == VK_SUCCESS) {
            *pp = p->next;
            p->next = NULL;
            if (p->serial)
                flux_vk_note_graphics_completed(d, p->serial);
            upload_pending_recycle(d, p->fence, p->pool, p->pool_family, p->pool_cmd, p->pool2,
                                   p->pool2_family, p->pool2_cmd, p->sem, p->stagings);
            flux_internal_free(d, p);
        } else {
            pp = &p->next;
        }
    }
    flux_platform_mutex_unlock(&d->upload_pending_lock);
}

void flux_vk_upload_pending_drain(flux_device *d) {
    for (;;) {
        flux_platform_mutex_lock(&d->upload_pending_lock);
        flux_upload_pending *p = d->upload_pending_head;
        if (p)
            d->upload_pending_head = p->next;
        flux_platform_mutex_unlock(&d->upload_pending_lock);
        if (!p)
            break;
        p->next = NULL;
        vkWaitForFences(d->device, 1, &p->fence, VK_TRUE, UINT64_MAX);
        if (p->serial)
            flux_vk_note_graphics_completed(d, p->serial);
        upload_pending_recycle(d, p->fence, p->pool, p->pool_family, p->pool_cmd, p->pool2,
                               p->pool2_family, p->pool2_cmd, p->sem, p->stagings);
        flux_internal_free(d, p);
    }
}

/* Park a just-submitted upload batch for deferred retirement; the fence
 * must be the one the batch was submitted with, `serial` its graphics
 * submission serial (0 for transfer-queue batches). The pool family
 * tags let the recycle path return the pools to the transient pool
 * cache. On host OOM, wait the fence inline and recycle immediately —
 * the slow path the old code had everywhere, taken only when a small
 * allocation fails. */
void flux_vk_upload_pending_park_families(flux_device *d, VkFence fence, VkCommandPool pool,
                                          uint32_t pool_family, VkCommandBuffer pool_cmd,
                                          VkCommandPool pool2, uint32_t pool2_family,
                                          VkCommandBuffer pool2_cmd, VkSemaphore sem,
                                          flux_staging_buf *stagings, uint64_t serial) {
    flux_upload_pending *p = flux_internal_alloc(d, sizeof(*p));
    if (!p) {
        vkWaitForFences(d->device, 1, &fence, VK_TRUE, UINT64_MAX);
        if (serial)
            flux_vk_note_graphics_completed(d, serial);
        upload_pending_recycle(d, fence, pool, pool_family, pool_cmd, pool2, pool2_family,
                               pool2_cmd, sem, stagings);
        return;
    }
    *p = (flux_upload_pending){
        .fence = fence,
        .pool = pool,
        .pool2 = pool2,
        .pool_family = pool_family,
        .pool2_family = pool2_family,
        .pool_cmd = pool_cmd,
        .pool2_cmd = pool2_cmd,
        .sem = sem,
        .stagings = stagings,
        .serial = serial,
    };
    flux_platform_mutex_lock(&d->upload_pending_lock);
    p->next = d->upload_pending_head;
    d->upload_pending_head = p;
    flux_platform_mutex_unlock(&d->upload_pending_lock);
}

void flux_vk_upload_pending_park(flux_device *d, VkFence fence, VkCommandPool pool,
                                 VkCommandPool pool2, VkSemaphore sem, flux_staging_buf *stagings,
                                 uint64_t serial) {
    /* Callers of the plain API (dma-buf import) don't tag pool
     * families; their pools are destroyed on recycle, not cached, so
     * the command buffers die with the pool and need no explicit free
     * here. */
    flux_vk_upload_pending_park_families(d, fence, pool, UINT32_MAX, VK_NULL_HANDLE, pool2,
                                         UINT32_MAX, VK_NULL_HANDLE, sem, stagings, serial);
}

/* Fence + queue submission shared by every deferred upload path. On
 * success *out_fence (when non-NULL) receives a fence that signals when
 * the batch retires; on failure nothing is pending, the fence is
 * destroyed, and the caller recycles the batch's resources inline.
 * Graphics-queue submissions are assigned a retire-watermark serial
 * (*out_serial, 0 when NULL or when submitting elsewhere) in true queue
 * order; parking it lets the sweep advance the completed watermark when
 * the fence signals. */
VkResult flux_vk_submit_upload(flux_device *d, VkQueue queue, VkCommandBuffer cmd,
                               VkSemaphore wait_sem, VkPipelineStageFlags2 wait_stage,
                               VkSemaphore signal_sem, VkPipelineStageFlags2 signal_stage,
                               VkFence *out_fence, uint64_t *out_serial) {
    if (out_fence)
        *out_fence = VK_NULL_HANDLE;
    if (out_serial)
        *out_serial = 0;
    VkFence fence = VK_NULL_HANDLE;
    if (out_fence) {
        VkFenceCreateInfo fci = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        VkResult fr = vkCreateFence(d->device, &fci, nullptr, &fence);
        if (fr != VK_SUCCESS)
            return fr;
    }
    VkSemaphoreSubmitInfo wait_info = {
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
        .semaphore = wait_sem,
        .stageMask = wait_stage,
    };
    VkSemaphoreSubmitInfo signal_info = {
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
        .semaphore = signal_sem,
        .stageMask = signal_stage,
    };
    VkCommandBufferSubmitInfo cbsi = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
        .commandBuffer = cmd,
    };
    VkSubmitInfo2 si = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
        .waitSemaphoreInfoCount = wait_sem ? 1 : 0,
        .pWaitSemaphoreInfos = wait_sem ? &wait_info : nullptr,
        .signalSemaphoreInfoCount = signal_sem ? 1 : 0,
        .pSignalSemaphoreInfos = signal_sem ? &signal_info : nullptr,
        .commandBufferInfoCount = 1,
        .pCommandBufferInfos = &cbsi,
    };
    flux_platform_mutex_lock(&d->queue_lock);
    VkResult vr = vkQueueSubmit2(queue, 1, &si, fence);
    uint64_t serial = 0;
    if (vr == VK_SUCCESS && queue == d->graphics_queue && out_serial)
        serial = flux_vk_note_graphics_submission(d);
    flux_platform_mutex_unlock(&d->queue_lock);
    if (vr != VK_SUCCESS) {
        if (fence)
            vkDestroyFence(d->device, fence, nullptr);
        return vr;
    }
    if (out_fence)
        *out_fence = fence;
    if (out_serial)
        *out_serial = serial;
    return VK_SUCCESS;
}

/* ------------------------------------------------------------------ */
/*  Upload batch (public flux_uploads_begin / flux_uploads_flush)     */
/* ------------------------------------------------------------------ */

void flux_vk_upload_batch_attach_staging(flux_device *d, flux_staging_buf *sb) {
    sb->next = d->upload_batch_stagings;
    d->upload_batch_stagings = sb;
}

flux_result flux_uploads_begin(flux_device *d) {
    if (!d)
        return FLUX_ERROR_INVALID_ARGUMENT;
    upload_pending_sweep(d);
    flux_platform_mutex_lock(&d->upload_lock);
    if (d->upload_batch_open) {
        /* Nested scope: the outer batch already records every upload;
         * just bump the depth so its flush waits for this scope too. */
        d->upload_batch_depth++;
        flux_platform_mutex_unlock(&d->upload_lock);
        return FLUX_OK;
    }
    /* Batches always record on the graphics queue: same-queue implicit
     * ordering against in-flight frames is what makes live-image
     * updates safe, and it needs no QFOT dance. */
    VkResult vr = flux_vk_new_transient_cmd(d, d->graphics_family, &d->upload_batch_pool,
                                            &d->upload_batch_cmd);
    if (vr != VK_SUCCESS) {
        flux_platform_mutex_unlock(&d->upload_lock);
        FLUX_FAIL_VK(FLUX_ERROR_BACKEND_FAILURE, "upload batch cmd alloc failed", vr);
        return FLUX_ERROR_BACKEND_FAILURE;
    }
    d->upload_batch_open = true;
    d->upload_batch_depth = 1;
    flux_platform_mutex_unlock(&d->upload_lock);
    return FLUX_OK;
}

flux_result flux_uploads_flush(flux_device *d) {
    if (!d)
        return FLUX_ERROR_INVALID_ARGUMENT;
    upload_pending_sweep(d);
    flux_platform_mutex_lock(&d->upload_lock);
    if (!d->upload_batch_open) {
        flux_platform_mutex_unlock(&d->upload_lock);
        return FLUX_OK;
    }
    if (d->upload_batch_depth > 1) {
        /* Inner scope close: the outermost flush submits everything. */
        d->upload_batch_depth--;
        flux_platform_mutex_unlock(&d->upload_lock);
        return FLUX_OK;
    }
    /* Detach the batch state first: the submit below is device-global
     * queue work, not batch state, and a concurrent uploads_begin must
     * not wait on it. */
    VkCommandBuffer cmd = d->upload_batch_cmd;
    VkCommandPool pool = d->upload_batch_pool;
    flux_staging_buf *stagings = d->upload_batch_stagings;
    d->upload_batch_pool = VK_NULL_HANDLE;
    d->upload_batch_cmd = VK_NULL_HANDLE;
    d->upload_batch_stagings = NULL;
    d->upload_batch_open = false;
    d->upload_batch_depth = 0;
    flux_platform_mutex_unlock(&d->upload_lock);

    /* Deferred submit, no fence wait. Later same-queue work (the frame
     * that follows in begin_frame, readback) is ordered after the copies
     * by queue submission order alone; the parked fence only gates
     * staging-buffer reuse, which the lazy sweep handles. */
    VkResult vr = vkEndCommandBuffer(cmd);
    VkFence fence = VK_NULL_HANDLE;
    uint64_t serial = 0;
    if (vr == VK_SUCCESS)
        vr = flux_vk_submit_upload(d, d->graphics_queue, cmd, VK_NULL_HANDLE, 0, VK_NULL_HANDLE, 0,
                                   &fence, &serial);
    if (vr == VK_SUCCESS) {
        flux_vk_upload_pending_park_families(d, fence, pool, d->graphics_family, cmd,
                                             VK_NULL_HANDLE, 0, VK_NULL_HANDLE, VK_NULL_HANDLE,
                                             stagings, serial);
        return FLUX_OK;
    }
    /* End/submit failure: nothing reached the GPU, so the batch's
     * resources are safe to recycle inline. */
    upload_pending_recycle(d, VK_NULL_HANDLE, pool, d->graphics_family, cmd, VK_NULL_HANDLE, 0,
                           VK_NULL_HANDLE, VK_NULL_HANDLE, stagings);
    flux_result r = submit_result(vr);
    FLUX_FAIL_VK(r, "upload batch submit failed", vr);
    return r;
}

/* ------------------------------------------------------------------ */
/*  One-shot buffer upload                                            */
/* ------------------------------------------------------------------ */

/* Make a vkCmdCopyBuffer's transfer write visible to every later read
 * consumer of a flux buffer (vertex/index fetches, uniform/storage reads,
 * buffer-reference loads). Same-queue submission order alone orders
 * execution but carries no memory visibility, so the copy needs an explicit
 * transfer-write -> shader-read dependency — the buffer counterpart of
 * record_image_upload's post barrier. Shared by the direct graphics-queue
 * path and by upload batches (which record graphics-side too). */
static void record_buffer_copy_barrier(VkCommandBuffer cmd, VkBuffer dst, VkDeviceSize offset,
                                       VkDeviceSize size) {
    VkBufferMemoryBarrier2 post = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2,
        .srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT,
        .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
        .dstStageMask =
            VK_PIPELINE_STAGE_2_VERTEX_INPUT_BIT | VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT |
            VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        .dstAccessMask = VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_2_INDEX_READ_BIT |
                         VK_ACCESS_2_UNIFORM_READ_BIT | VK_ACCESS_2_SHADER_READ_BIT,
        .buffer = dst,
        .offset = offset,
        .size = size,
    };
    VkDependencyInfo di = {
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .bufferMemoryBarrierCount = 1,
        .pBufferMemoryBarriers = &post,
    };
    vkCmdPipelineBarrier2(cmd, &di);
}

flux_result flux_vk_upload_to_buffer(flux_device *d, VkBuffer dst, VkDeviceSize offset,
                                     const void *data, VkDeviceSize size) {
    if (size == 0)
        return FLUX_OK;

    upload_pending_sweep(d);
    flux_staging_buf *staging = NULL;
    VkCommandPool xfer_pool = VK_NULL_HANDLE;
    VkCommandBuffer xfer_cmd = VK_NULL_HANDLE;
    VkCommandPool gfx_pool = VK_NULL_HANDLE;
    VkCommandBuffer gfx_cmd = VK_NULL_HANDLE;
    VkSemaphore handoff = VK_NULL_HANDLE;
    VkResult vr = VK_SUCCESS;
    /* Hoisted above every goto fail: the fail path releases pools by
     * family, so the declaration must precede any jump to it. */
    bool use_xfer = flux_vk_prefer_transfer_queue(d);
    flux_result r = flux_vk_staging_acquire(d, size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, &staging);
    if (r != FLUX_OK)
        return r;

    /* HOST_VISIBLE staging buffers are always pre-mapped by the
     * allocator (one mapping per VkDeviceMemory, set at block-create
     * time). If this is NULL the allocator invariant is broken. */
    if (!staging->alloc.mapped) {
        FLUX_FAIL(FLUX_ERROR_BACKEND_FAILURE,
                  "staging allocation came back un-mapped (allocator invariant violated)");
        vr = VK_ERROR_UNKNOWN;
        goto fail;
    }
    memcpy(staging->alloc.mapped, data, size);

    /* Batch mode: record into the open batch and return. The staging
     * buffer stays checked out (chained on the batch) until the flush's
     * parked fence proves the GPU is done reading it. */
    flux_platform_mutex_lock(&d->upload_lock);
    if (d->upload_batch_open) {
        VkBufferCopy region = {.srcOffset = 0, .dstOffset = offset, .size = size};
        vkCmdCopyBuffer(d->upload_batch_cmd, staging->buffer, dst, 1, &region);
        record_buffer_copy_barrier(d->upload_batch_cmd, dst, offset, size);
        flux_vk_upload_batch_attach_staging(d, staging);
        flux_platform_mutex_unlock(&d->upload_lock);
        return FLUX_OK;
    }
    flux_platform_mutex_unlock(&d->upload_lock);

    vr = flux_vk_new_transient_cmd(d, use_xfer ? d->transfer_family : d->graphics_family,
                                   &xfer_pool, &xfer_cmd);
    if (vr != VK_SUCCESS) {
        FLUX_FAIL_VK(FLUX_ERROR_BACKEND_FAILURE, "upload cmd alloc failed", vr);
        goto fail;
    }

    VkBufferCopy region = {.srcOffset = 0, .dstOffset = offset, .size = size};
    vkCmdCopyBuffer(xfer_cmd, staging->buffer, dst, 1, &region);

    if (use_xfer) {
        /* Release ownership transfer-family -> graphics-family. */
        VkBufferMemoryBarrier2 rel = {
            .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2,
            .srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT,
            .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
            .dstStageMask = 0, /* required for release half of QFOT */
            .dstAccessMask = 0,
            .srcQueueFamilyIndex = d->transfer_family,
            .dstQueueFamilyIndex = d->graphics_family,
            .buffer = dst,
            .offset = offset,
            .size = size,
        };
        VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                               .bufferMemoryBarrierCount = 1,
                               .pBufferMemoryBarriers = &rel};
        vkCmdPipelineBarrier2(xfer_cmd, &di);
    } else {
        /* Same-queue direct upload: execution order with later batches is
         * implicit, visibility is not — make the transfer write readable by
         * the frame work that follows on the graphics queue. */
        record_buffer_copy_barrier(xfer_cmd, dst, offset, size);
    }

    vr = vkEndCommandBuffer(xfer_cmd);
    if (vr != VK_SUCCESS) {
        FLUX_FAIL_VK(FLUX_ERROR_BACKEND_FAILURE, "vkEndCommandBuffer (upload) failed", vr);
        goto fail;
    }

    if (use_xfer) {
        VkSemaphoreCreateInfo sci = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        vr = vkCreateSemaphore(d->device, &sci, nullptr, &handoff);
        if (vr != VK_SUCCESS) {
            FLUX_FAIL_VK(FLUX_ERROR_BACKEND_FAILURE, "vkCreateSemaphore (upload handoff) failed",
                         vr);
            goto fail;
        }
        /* Acquire on graphics. */
        vr = flux_vk_new_transient_cmd(d, d->graphics_family, &gfx_pool, &gfx_cmd);
        if (vr != VK_SUCCESS) {
            FLUX_FAIL_VK(FLUX_ERROR_BACKEND_FAILURE, "upload acquire cmd alloc failed", vr);
            goto fail;
        }
        VkBufferMemoryBarrier2 acq = {
            .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2,
            .srcStageMask = 0, /* required for acquire half of QFOT */
            .srcAccessMask = 0,
            .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
            .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT,
            .srcQueueFamilyIndex = d->transfer_family,
            .dstQueueFamilyIndex = d->graphics_family,
            .buffer = dst,
            .offset = offset,
            .size = size,
        };
        VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                               .bufferMemoryBarrierCount = 1,
                               .pBufferMemoryBarriers = &acq};
        vkCmdPipelineBarrier2(gfx_cmd, &di);
        vr = vkEndCommandBuffer(gfx_cmd);
        if (vr != VK_SUCCESS) {
            FLUX_FAIL_VK(FLUX_ERROR_BACKEND_FAILURE, "vkEndCommandBuffer (acquire) failed", vr);
            goto fail;
        }
        /* Submit transfer-then-graphics with the handoff semaphore, all
         * deferred. The single fence parked on the graphics submit covers
         * both batches: the graphics batch only starts after the handoff
         * semaphore (signalled by the transfer batch) fires. */
        vr = flux_vk_submit_upload(d, d->transfer_queue, xfer_cmd, VK_NULL_HANDLE, 0, handoff,
                                   VK_PIPELINE_STAGE_2_COPY_BIT, nullptr, nullptr);
        if (vr != VK_SUCCESS) {
            FLUX_FAIL_VK(FLUX_ERROR_BACKEND_FAILURE, "transfer-queue submit failed", vr);
            goto fail;
        }
        VkFence gfence = VK_NULL_HANDLE;
        uint64_t gserial = 0;
        vr = flux_vk_submit_upload(d, d->graphics_queue, gfx_cmd, handoff,
                                   VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_NULL_HANDLE, 0, &gfence,
                                   &gserial);
        if (vr == VK_SUCCESS) {
            flux_vk_upload_pending_park_families(d, gfence, xfer_pool, d->transfer_family, xfer_cmd,
                                                 gfx_pool, d->graphics_family, gfx_cmd, handoff,
                                                 staging, gserial);
            return FLUX_OK;
        }
        /* The transfer batch is in flight and references the staging
         * buffer and its command pool; drain the transfer queue before
         * the fail path recycles them. */
        flux_platform_mutex_lock(&d->queue_lock);
        vkQueueWaitIdle(d->transfer_queue);
        flux_platform_mutex_unlock(&d->queue_lock);
        FLUX_FAIL_VK(FLUX_ERROR_BACKEND_FAILURE, "graphics-queue acquire submit failed", vr);
        goto fail;
    } else {
        VkFence fence = VK_NULL_HANDLE;
        uint64_t serial = 0;
        vr = flux_vk_submit_upload(d, d->graphics_queue, xfer_cmd, VK_NULL_HANDLE, 0,
                                   VK_NULL_HANDLE, 0, &fence, &serial);
        if (vr == VK_SUCCESS) {
            flux_vk_upload_pending_park_families(d, fence, xfer_pool, d->graphics_family, xfer_cmd,
                                                 VK_NULL_HANDLE, 0, VK_NULL_HANDLE, VK_NULL_HANDLE,
                                                 staging, serial);
            return FLUX_OK;
        }
        FLUX_FAIL_VK(FLUX_ERROR_BACKEND_FAILURE, "upload submit failed", vr);
        goto fail;
    }

fail:
    if (handoff)
        vkDestroySemaphore(d->device, handoff, nullptr);
    /* Failure paths only (the success paths parked everything and
     * returned above): no submission is pending, so pools and the
     * staging buffer go straight back to their caches. */
    transient_pool_release(d, gfx_pool, d->graphics_family, gfx_cmd, VK_NULL_HANDLE);
    transient_pool_release(d, xfer_pool, use_xfer ? d->transfer_family : d->graphics_family,
                           xfer_cmd, VK_NULL_HANDLE);
    flux_vk_staging_release(d, staging);
    return submit_result(vr);
}

/* ------------------------------------------------------------------ */
/*  One-shot image upload                                             */
/* ------------------------------------------------------------------ */

/* Record the complete single-queue upload for one image into `cmd`:
 * old_layout -> TRANSFER_DST, buffer->image copy, TRANSFER_DST ->
 * SHADER_READ_ONLY. srcAccess covers prior shader reads when updating
 * an already-sampled image. Shared by the synchronous graphics-queue
 * path and by upload batches (which always record graphics-side). */
static void record_image_upload(VkCommandBuffer cmd, VkBuffer staging, VkImage dst, int32_t ox,
                                int32_t oy, uint32_t w, uint32_t h, VkImageLayout old_layout) {
    bool from_shader = old_layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkImageMemoryBarrier2 pre = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .srcStageMask = from_shader ? VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT
                                    : VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT,
        .srcAccessMask = from_shader ? VK_ACCESS_2_SHADER_SAMPLED_READ_BIT : 0,
        .dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT,
        .dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
        .oldLayout = old_layout,
        .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .image = dst,
        .subresourceRange =
            {
                .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                .levelCount = 1,
                .layerCount = 1,
            },
    };
    VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                           .imageMemoryBarrierCount = 1,
                           .pImageMemoryBarriers = &pre};
    vkCmdPipelineBarrier2(cmd, &di);

    VkBufferImageCopy region = {
        .bufferOffset = 0,
        .imageSubresource =
            {
                .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                .mipLevel = 0,
                .baseArrayLayer = 0,
                .layerCount = 1,
            },
        .imageOffset = {ox, oy, 0},
        .imageExtent = {w, h, 1},
    };
    vkCmdCopyBufferToImage(cmd, staging, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    VkImageMemoryBarrier2 post = pre;
    post.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
    post.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    post.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    post.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
    post.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    post.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    di.pImageMemoryBarriers = &post;
    vkCmdPipelineBarrier2(cmd, &di);
}

flux_result flux_vk_upload_to_image(flux_device *d, VkImage dst, int32_t offset_x, int32_t offset_y,
                                    uint32_t width, uint32_t height, VkImageLayout old_layout,
                                    const void *data, size_t bytes) {
    if (bytes == 0)
        return FLUX_OK;

    upload_pending_sweep(d);
    flux_staging_buf *staging = NULL;
    VkCommandPool xfer_pool = VK_NULL_HANDLE;
    VkCommandBuffer xfer_cmd = VK_NULL_HANDLE;
    VkCommandPool gfx_pool = VK_NULL_HANDLE;
    VkCommandBuffer gfx_cmd = VK_NULL_HANDLE;
    VkSemaphore handoff = VK_NULL_HANDLE;
    VkResult vr = VK_SUCCESS;
    /* Hoisted above every goto fail: the fail path releases pools by
     * family, so the declaration must precede any jump to it. See the
     * comment below for the transfer-queue policy itself. */
    bool use_xfer = flux_vk_prefer_transfer_queue(d) && old_layout == VK_IMAGE_LAYOUT_UNDEFINED;
    flux_result r = flux_vk_staging_acquire(d, bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, &staging);
    if (r != FLUX_OK)
        return r;

    if (!staging->alloc.mapped) {
        FLUX_FAIL(FLUX_ERROR_BACKEND_FAILURE,
                  "image staging allocation came back un-mapped (allocator invariant violated)");
        vr = VK_ERROR_UNKNOWN;
        goto fail;
    }
    memcpy(staging->alloc.mapped, data, bytes);

    /* Batch mode: record the full single-queue sequence into the open
     * batch; the staging buffer stays checked out until flush. */
    flux_platform_mutex_lock(&d->upload_lock);
    if (d->upload_batch_open) {
        record_image_upload(d->upload_batch_cmd, staging->buffer, dst, offset_x, offset_y, width,
                            height, old_layout);
        flux_vk_upload_batch_attach_staging(d, staging);
        flux_platform_mutex_unlock(&d->upload_lock);
        return FLUX_OK;
    }
    flux_platform_mutex_unlock(&d->upload_lock);

    /* When the image is already SHADER_READ_ONLY_OPTIMAL we're updating
     * a resource that may still be sampled by an in-flight frame on the
     * graphics queue. Same-queue submissions are implicitly ordered, so
     * a graphics-queue submission correctly waits on the prior frame's
     * fragment-shader reads before the layout transition. A dedicated
     * transfer queue has *no* implicit ordering against the graphics
     * queue, so the SHADER_READ → TRANSFER_DST transition would race
     * with in-flight reads. The proper fix on a transfer queue is a
     * graphics-release → transfer-acquire QFOT dance, but that costs
     * two extra submissions + two semaphores + four barriers — strictly
     * worse than just recording the update on the graphics queue, which
     * is what every production renderer does for live-image updates.
     * Initial-upload path (old_layout==UNDEFINED) keeps the transfer
     * queue since the resource is not yet in flight. */
    vr = flux_vk_new_transient_cmd(d, use_xfer ? d->transfer_family : d->graphics_family,
                                   &xfer_pool, &xfer_cmd);
    if (vr != VK_SUCCESS) {
        FLUX_FAIL_VK(FLUX_ERROR_BACKEND_FAILURE, "image upload cmd alloc failed", vr);
        goto fail;
    }

    /* Record the copy sequence: dual-queue (transfer + QFOT) for initial
     * uploads when a dedicated transfer queue exists, otherwise the
     * single-queue sequence shared with the batch path. */
    if (use_xfer) {
        /* old_layout -> TRANSFER_DST, copy, then the release-ownership
         * barrier; the matching acquire (+ final transition to
         * SHADER_READ_ONLY) is recorded on the graphics queue. This
         * path only runs for old_layout == UNDEFINED. */
        VkImageMemoryBarrier2 pre = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
            .srcStageMask = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT,
            .srcAccessMask = 0,
            .dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT,
            .dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
            .oldLayout = old_layout,
            .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            .image = dst,
            .subresourceRange =
                {
                    .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                    .levelCount = 1,
                    .layerCount = 1,
                },
        };
        VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                               .imageMemoryBarrierCount = 1,
                               .pImageMemoryBarriers = &pre};
        vkCmdPipelineBarrier2(xfer_cmd, &di);

        VkBufferImageCopy region = {
            .bufferOffset = 0,
            .imageSubresource =
                {
                    .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                    .mipLevel = 0,
                    .baseArrayLayer = 0,
                    .layerCount = 1,
                },
            .imageOffset = {offset_x, offset_y, 0},
            .imageExtent = {width, height, 1},
        };
        vkCmdCopyBufferToImage(xfer_cmd, staging->buffer, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                               1, &region);

        VkImageMemoryBarrier2 rel = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
            .srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT,
            .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
            .dstStageMask = 0,
            .dstAccessMask = 0,
            .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            .srcQueueFamilyIndex = d->transfer_family,
            .dstQueueFamilyIndex = d->graphics_family,
            .image = dst,
            .subresourceRange =
                {
                    .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                    .levelCount = 1,
                    .layerCount = 1,
                },
        };
        di.pImageMemoryBarriers = &rel;
        vkCmdPipelineBarrier2(xfer_cmd, &di);
    } else {
        record_image_upload(xfer_cmd, staging->buffer, dst, offset_x, offset_y, width, height,
                            old_layout);
    }

    vr = vkEndCommandBuffer(xfer_cmd);
    if (vr != VK_SUCCESS) {
        FLUX_FAIL_VK(FLUX_ERROR_BACKEND_FAILURE, "vkEndCommandBuffer (image upload) failed", vr);
        goto fail;
    }

    if (use_xfer) {
        VkSemaphoreCreateInfo sci = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        vr = vkCreateSemaphore(d->device, &sci, nullptr, &handoff);
        if (vr != VK_SUCCESS) {
            FLUX_FAIL_VK(FLUX_ERROR_BACKEND_FAILURE, "vkCreateSemaphore (image handoff) failed",
                         vr);
            goto fail;
        }
        /* Graphics-queue: acquire + final layout transition. */
        vr = flux_vk_new_transient_cmd(d, d->graphics_family, &gfx_pool, &gfx_cmd);
        if (vr != VK_SUCCESS) {
            FLUX_FAIL_VK(FLUX_ERROR_BACKEND_FAILURE, "image acquire cmd alloc failed", vr);
            goto fail;
        }
        VkImageMemoryBarrier2 acq = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
            .srcStageMask = 0,
            .srcAccessMask = 0,
            .dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
            .dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            .srcQueueFamilyIndex = d->transfer_family,
            .dstQueueFamilyIndex = d->graphics_family,
            .image = dst,
            .subresourceRange =
                {
                    .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                    .levelCount = 1,
                    .layerCount = 1,
                },
        };
        VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                               .imageMemoryBarrierCount = 1,
                               .pImageMemoryBarriers = &acq};
        vkCmdPipelineBarrier2(gfx_cmd, &di);
        vr = vkEndCommandBuffer(gfx_cmd);
        if (vr != VK_SUCCESS) {
            FLUX_FAIL_VK(FLUX_ERROR_BACKEND_FAILURE, "vkEndCommandBuffer (acquire) failed", vr);
            goto fail;
        }
        /* Deferred transfer-then-graphics with the handoff semaphore; the
         * single fence parked on the graphics submit covers both batches
         * (the graphics batch waits on the transfer-signalled handoff). */
        vr = flux_vk_submit_upload(d, d->transfer_queue, xfer_cmd, VK_NULL_HANDLE, 0, handoff,
                                   VK_PIPELINE_STAGE_2_COPY_BIT, nullptr, nullptr);
        if (vr != VK_SUCCESS) {
            FLUX_FAIL_VK(FLUX_ERROR_BACKEND_FAILURE, "transfer-queue image submit failed", vr);
            goto fail;
        }
        VkFence gfence = VK_NULL_HANDLE;
        uint64_t gserial = 0;
        vr = flux_vk_submit_upload(d, d->graphics_queue, gfx_cmd, handoff,
                                   VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_NULL_HANDLE, 0, &gfence,
                                   &gserial);
        if (vr == VK_SUCCESS) {
            flux_vk_upload_pending_park_families(d, gfence, xfer_pool, d->transfer_family, xfer_cmd,
                                                 gfx_pool, d->graphics_family, gfx_cmd, handoff,
                                                 staging, gserial);
            return FLUX_OK;
        }
        /* The transfer batch is in flight and still reads the staging
         * buffer and its command pool; drain it before recycling. */
        flux_platform_mutex_lock(&d->queue_lock);
        vkQueueWaitIdle(d->transfer_queue);
        flux_platform_mutex_unlock(&d->queue_lock);
        FLUX_FAIL_VK(FLUX_ERROR_BACKEND_FAILURE, "graphics-queue image acquire failed", vr);
        goto fail;
    } else {
        VkFence fence = VK_NULL_HANDLE;
        uint64_t serial = 0;
        vr = flux_vk_submit_upload(d, d->graphics_queue, xfer_cmd, VK_NULL_HANDLE, 0,
                                   VK_NULL_HANDLE, 0, &fence, &serial);
        if (vr == VK_SUCCESS) {
            flux_vk_upload_pending_park_families(d, fence, xfer_pool, d->graphics_family, xfer_cmd,
                                                 VK_NULL_HANDLE, 0, VK_NULL_HANDLE, VK_NULL_HANDLE,
                                                 staging, serial);
            return FLUX_OK;
        }
        FLUX_FAIL_VK(FLUX_ERROR_BACKEND_FAILURE, "image upload submit failed", vr);
        goto fail;
    }

fail:
    if (handoff)
        vkDestroySemaphore(d->device, handoff, nullptr);
    /* Failure paths only: no submission is pending (any successful
     * transfer submit was drained above), so pools and the staging
     * buffer go straight back to their caches. */
    transient_pool_release(d, gfx_pool, d->graphics_family, gfx_cmd, VK_NULL_HANDLE);
    transient_pool_release(d, xfer_pool, use_xfer ? d->transfer_family : d->graphics_family,
                           xfer_cmd, VK_NULL_HANDLE);
    flux_vk_staging_release(d, staging);
    return submit_result(vr);
}

/* ------------------------------------------------------------------ */
/*  One-shot layout transition                                        */
/* ------------------------------------------------------------------ */

/* Record one layout-transition barrier into `cmd`. Shared by the
 * one-shot path and by upload batches. */
static void record_layout_transition(VkCommandBuffer cmd, VkImage img, VkImageLayout old_layout,
                                     VkImageLayout new_layout) {
    VkImageMemoryBarrier2 b = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .srcStageMask = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT,
        .srcAccessMask = 0,
        .dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
        .dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
        .oldLayout = old_layout,
        .newLayout = new_layout,
        .image = img,
        .subresourceRange =
            {
                .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                .levelCount = 1,
                .layerCount = 1,
            },
    };
    VkDependencyInfo di = {
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .imageMemoryBarrierCount = 1,
        .pImageMemoryBarriers = &b,
    };
    vkCmdPipelineBarrier2(cmd, &di);
}

flux_result flux_vk_transition_image_layout(flux_device *d, VkImage img, VkImageLayout old_layout,
                                            VkImageLayout new_layout) {
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkResult vr;

    upload_pending_sweep(d);
    /* Batch mode: record the barrier and return; flush submits it. */
    flux_platform_mutex_lock(&d->upload_lock);
    if (d->upload_batch_open) {
        record_layout_transition(d->upload_batch_cmd, img, old_layout, new_layout);
        flux_platform_mutex_unlock(&d->upload_lock);
        return FLUX_OK;
    }
    flux_platform_mutex_unlock(&d->upload_lock);

    vr = flux_vk_new_transient_cmd(d, d->graphics_family, &pool, &cmd);
    if (vr != VK_SUCCESS) {
        FLUX_FAIL_VK(FLUX_ERROR_BACKEND_FAILURE, "transition cmd alloc failed", vr);
        goto fail;
    }

    record_layout_transition(cmd, img, old_layout, new_layout);

    vr = vkEndCommandBuffer(cmd);
    if (vr != VK_SUCCESS) {
        FLUX_FAIL_VK(FLUX_ERROR_BACKEND_FAILURE, "transition vkEndCommandBuffer failed", vr);
        goto fail;
    }

    {
        VkFence fence = VK_NULL_HANDLE;
        uint64_t serial = 0;
        vr = flux_vk_submit_upload(d, d->graphics_queue, cmd, VK_NULL_HANDLE, 0, VK_NULL_HANDLE, 0,
                                   &fence, &serial);
        if (vr == VK_SUCCESS) {
            flux_vk_upload_pending_park_families(d, fence, pool, d->graphics_family, cmd,
                                                 VK_NULL_HANDLE, 0, VK_NULL_HANDLE, VK_NULL_HANDLE,
                                                 NULL, serial);
            return FLUX_OK;
        }
    }

fail:
    /* Nothing from the pool was submitted (or the submit failed), so
     * it goes straight back to the transient pool cache. */
    transient_pool_release(d, pool, d->graphics_family, cmd, VK_NULL_HANDLE);
    if (vr != VK_SUCCESS) {
        flux_result r = submit_result(vr);
        FLUX_FAIL_VK(r, "image layout transition failed", vr);
        return r;
    }
    return FLUX_OK;
}

/* ------------------------------------------------------------------ */
/*  Public one-shot submission (flux_oneshot_*, vulkan.h)             */
/* ------------------------------------------------------------------ */
/* Thin, safe publication of the sequence this file already implements:
 * transient pool -> ONE_TIME_SUBMIT begin -> graphics-queue submit with
 * a finite fence wait -> recycle. Headless effect users previously had
 * no flux path for this at all (effect.h demanded a caller-supplied
 * one-shot command buffer and a prior vkQueueWaitIdle while owning the
 * only queues). */

FLUX_API flux_result flux_oneshot_begin(flux_device *d, VkCommandBuffer *out_cmd) {
    if (!d || !out_cmd)
        return FLUX_ERROR_INVALID_ARGUMENT;
    *out_cmd = VK_NULL_HANDLE;

    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkResult vr = flux_vk_new_transient_cmd(d, d->graphics_family, &pool, &cmd);
    if (vr != VK_SUCCESS) {
        flux_result r = submit_result(vr);
        FLUX_FAIL_VK(r, "flux_oneshot_begin: transient command buffer failed", vr);
        return r;
    }

    /* The pool must outlive the caller's recording and come back to the
     * cache only after submit; park it on the handle the caller passes
     * back. VkCommandBuffer is an opaque pointer, so we cannot attach
     * state to it — instead remember the (cmd -> pool) pair in a small
     * side table guarded by staging_lock, mirroring how upload batches
     * track theirs. */
    flux_platform_mutex_lock(&d->staging_lock);
    /* Grow-on-demand open-addressed table; slots freed at recycle. */
    if (!d->oneshot_slots) {
        d->oneshot_slots = flux_internal_alloc(d, sizeof(*d->oneshot_slots) * 8);
        if (!d->oneshot_slots) {
            flux_platform_mutex_unlock(&d->staging_lock);
            transient_pool_release(d, pool, d->graphics_family, cmd, VK_NULL_HANDLE);
            return FLUX_ERROR_OUT_OF_MEMORY;
        }
        d->oneshot_slot_count = 8;
        memset(d->oneshot_slots, 0, sizeof(*d->oneshot_slots) * d->oneshot_slot_count);
    }
    uint32_t slot = UINT32_MAX;
    for (uint32_t i = 0; i < d->oneshot_slot_count; ++i) {
        if (!d->oneshot_slots[i].cmd) {
            slot = i;
            break;
        }
    }
    if (slot == UINT32_MAX) {
        uint32_t new_count = d->oneshot_slot_count * 2;
        __typeof__(d->oneshot_slots) grown =
            flux_internal_alloc(d, sizeof(*grown) * new_count);
        if (!grown) {
            flux_platform_mutex_unlock(&d->staging_lock);
            transient_pool_release(d, pool, d->graphics_family, cmd, VK_NULL_HANDLE);
            return FLUX_ERROR_OUT_OF_MEMORY;
        }
        memset(grown, 0, sizeof(*grown) * new_count);
        memcpy(grown, d->oneshot_slots, sizeof(*grown) * d->oneshot_slot_count);
        flux_internal_free(d, d->oneshot_slots);
        d->oneshot_slots = grown;
        slot = d->oneshot_slot_count;
        d->oneshot_slot_count = new_count;
    }
    d->oneshot_slots[slot].pool = pool;
    d->oneshot_slots[slot].cmd = cmd;
    flux_platform_mutex_unlock(&d->staging_lock);

    *out_cmd = cmd;
    return FLUX_OK;
}

static flux_result fluxi_oneshot_take(flux_device *d, VkCommandBuffer cmd,
                                      VkCommandPool *out_pool) {
    *out_pool = VK_NULL_HANDLE;
    if (!d || cmd == VK_NULL_HANDLE)
        return FLUX_ERROR_INVALID_ARGUMENT;
    flux_platform_mutex_lock(&d->staging_lock);
    for (uint32_t i = 0; i < d->oneshot_slot_count; ++i) {
        if (d->oneshot_slots && d->oneshot_slots[i].cmd == cmd) {
            *out_pool = d->oneshot_slots[i].pool;
            d->oneshot_slots[i].cmd = VK_NULL_HANDLE;
            d->oneshot_slots[i].pool = VK_NULL_HANDLE;
            flux_platform_mutex_unlock(&d->staging_lock);
            return FLUX_OK;
        }
    }
    flux_platform_mutex_unlock(&d->staging_lock);
    return FLUX_ERROR_INVALID_ARGUMENT; /* not from flux_oneshot_begin */
}

FLUX_API flux_result flux_oneshot_submit_and_end(flux_device *d, VkCommandBuffer cmd) {
    VkCommandPool pool = VK_NULL_HANDLE;
    flux_result take = fluxi_oneshot_take(d, cmd, &pool);
    if (take != FLUX_OK) {
        FLUX_FAIL(take, "flux_oneshot_submit_and_end: cmd not from flux_oneshot_begin");
        return take;
    }

    VkResult vr = vkEndCommandBuffer(cmd);
    if (vr != VK_SUCCESS) {
        transient_pool_release(d, pool, d->graphics_family, cmd, VK_NULL_HANDLE);
        flux_result r = submit_result(vr);
        FLUX_FAIL_VK(r, "flux_oneshot_submit_and_end: vkEndCommandBuffer failed", vr);
        return r;
    }

    vr = flux_vk_submit_and_wait(d, d->graphics_queue, cmd, VK_NULL_HANDLE, 0, VK_NULL_HANDLE, 0);
    /* submit_and_wait guarantees the batch completed (or timed out with a
     * queue-idle fallback), so the pool is safe to recycle either way. */
    transient_pool_release(d, pool, d->graphics_family, cmd, VK_NULL_HANDLE);
    if (vr != VK_SUCCESS) {
        flux_result r = submit_result(vr);
        FLUX_FAIL_VK(r, "flux_oneshot_submit_and_end: submit/wait failed", vr);
        return r;
    }
    return FLUX_OK;
}

FLUX_API flux_result flux_oneshot_run(flux_device *d, flux_oneshot_record_fn record,
                                      void *userdata) {
    if (!record)
        return FLUX_ERROR_INVALID_ARGUMENT;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    flux_result r = flux_oneshot_begin(d, &cmd);
    if (r != FLUX_OK)
        return r;
    record(cmd, userdata);
    return flux_oneshot_submit_and_end(d, cmd);
}
