/*
 * flux/dmabuf_stub.c - non-Linux stand-in for dmabuf.c.
 *
 * dma-buf import/export is a Linux-only facility (file-descriptor based
 * external memory + sync_file fences). meson swaps this file in for
 * dmabuf.c when the host is not Linux: every public entry point from
 * include/flux/dmabuf.h keeps its signature and fails with
 * FLUX_ERROR_UNSUPPORTED (capability queries report false), so portable
 * callers compile and degrade cleanly.
 *
 * The internal acquire-semaphore pool helpers are no-ops here: with no
 * import path there is no SYNC_FD semaphore to pool, and the frame /
 * device teardown call sites stay platform-neutral.
 */

#include "internal.h"
#include <flux/dmabuf.h>

VkSemaphore flux_dmabuf_acquire_semaphore_take(flux_device *d) {
    (void)d;
    return VK_NULL_HANDLE;
}

void flux_dmabuf_acquire_semaphore_recycle(flux_device *d, VkSemaphore semaphore) {
    /* Nothing pooled on this platform; destroy like the unpooled path. */
    if (d && d->device && semaphore)
        vkDestroySemaphore(d->device, semaphore, nullptr);
}

void flux_dmabuf_acquire_semaphore_pool_destroy(flux_device *d) {
    (void)d;
}

bool flux_dmabuf_supported(const flux_device *d) {
    (void)d;
    return false;
}

bool flux_dmabuf_sync_supported(const flux_device *d) {
    (void)d;
    return false;
}

flux_result flux_dmabuf_format_modifiers(flux_device *d, flux_format format,
                                         uint64_t *out_modifiers, uint32_t *inout_count) {
    (void)format;
    (void)out_modifiers;
    if (!d || !inout_count)
        return FLUX_ERROR_INVALID_ARGUMENT;
    FLUX_FAIL(FLUX_ERROR_UNSUPPORTED, "dma-buf is only available on Linux");
    return FLUX_ERROR_UNSUPPORTED;
}

flux_result flux_dmabuf_import_acquire_semaphore(flux_device *d, int acquire_sync_fd,
                                                 VkSemaphore *out) {
    (void)d;
    (void)acquire_sync_fd;
    *out = VK_NULL_HANDLE;
    FLUX_FAIL(FLUX_ERROR_UNSUPPORTED, "dma-buf is only available on Linux");
    return FLUX_ERROR_UNSUPPORTED;
}

void flux_dmabuf_close_fd(int fd) {
    (void)fd;
}

flux_result flux_image_import_dmabuf(flux_device *d, const flux_dmabuf_image_desc *desc,
                                     flux_image **out) {
    /* Mirror the Linux implementation's validation order so callers and
     * tests observe the same argument errors on every platform; the
     * capability gap is reported only for a fully-formed desc. */
    if (!d || !desc || !out)
        return FLUX_ERROR_INVALID_ARGUMENT;
    *out = nullptr;
    if (desc->type != FLUX_TYPE_DMABUF_IMAGE_DESC) {
        FLUX_FAIL(FLUX_ERROR_INVALID_ARGUMENT, "desc->type != FLUX_TYPE_DMABUF_IMAGE_DESC");
        return FLUX_ERROR_INVALID_ARGUMENT;
    }
    if (desc->width == 0 || desc->height == 0) {
        FLUX_FAIL(FLUX_ERROR_INVALID_ARGUMENT, "dmabuf image zero-extent");
        return FLUX_ERROR_INVALID_ARGUMENT;
    }
    if (desc->plane_count != 1) {
        FLUX_FAIL(FLUX_ERROR_UNSUPPORTED, "only single-plane dma-buf import supported");
        return FLUX_ERROR_UNSUPPORTED;
    }
    if (desc->has_acquire_sync_fd && desc->acquire_sync_fd < 0) {
        FLUX_FAIL(FLUX_ERROR_INVALID_ARGUMENT, "dma-buf acquire_sync_fd is invalid");
        return FLUX_ERROR_INVALID_ARGUMENT;
    }
    const flux_dmabuf_plane *plane = &desc->planes[0];
    if (plane->fd < 0) {
        FLUX_FAIL(FLUX_ERROR_INVALID_ARGUMENT, "dma-buf plane fd is invalid");
        return FLUX_ERROR_INVALID_ARGUMENT;
    }
    if (plane->stride == 0) {
        FLUX_FAIL(FLUX_ERROR_INVALID_ARGUMENT, "dma-buf plane stride is zero");
        return FLUX_ERROR_INVALID_ARGUMENT;
    }
    FLUX_FAIL(FLUX_ERROR_UNSUPPORTED, "dma-buf import is only available on Linux");
    return FLUX_ERROR_UNSUPPORTED;
}
