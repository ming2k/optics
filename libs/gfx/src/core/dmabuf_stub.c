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
#include <optics/gfx/dmabuf.h>

VkSemaphore optics_gfx_dmabuf_acquire_semaphore_take(optics_gfx_device *d) {
    (void)d;
    return VK_NULL_HANDLE;
}

void optics_gfx_dmabuf_acquire_semaphore_recycle(optics_gfx_device *d, VkSemaphore semaphore) {
    /* Nothing pooled on this platform; destroy like the unpooled path. */
    if (d && d->device && semaphore)
        vkDestroySemaphore(d->device, semaphore, nullptr);
}

void optics_gfx_dmabuf_acquire_semaphore_pool_destroy(optics_gfx_device *d) {
    (void)d;
}

bool optics_gfx_dmabuf_supported(const optics_gfx_device *d) {
    (void)d;
    return false;
}

bool optics_gfx_dmabuf_sync_supported(const optics_gfx_device *d) {
    (void)d;
    return false;
}

optics_gfx_result
optics_gfx_dmabuf_format_modifiers(optics_gfx_device *d,
                                   const optics_gfx_dmabuf_format_modifiers_opts *opts) {
    if (!d || !opts || !opts->inout_count)
        return OPTICS_GFX_ERROR_INVALID_ARGUMENT;
    OPTICS_GFX_FAIL(OPTICS_GFX_ERROR_UNSUPPORTED, "dma-buf is only available on Linux");
    return OPTICS_GFX_ERROR_UNSUPPORTED;
}

optics_gfx_result optics_gfx_dmabuf_import_acquire_semaphore(optics_gfx_device *d,
                                                             int acquire_sync_fd,
                                                             VkSemaphore *out) {
    (void)d;
    (void)acquire_sync_fd;
    *out = VK_NULL_HANDLE;
    OPTICS_GFX_FAIL(OPTICS_GFX_ERROR_UNSUPPORTED, "dma-buf is only available on Linux");
    return OPTICS_GFX_ERROR_UNSUPPORTED;
}

void optics_gfx_dmabuf_close_fd(int fd) {
    (void)fd;
}

optics_gfx_result optics_gfx_image_import_dmabuf(optics_gfx_device *d,
                                                 const optics_gfx_dmabuf_image_desc *desc,
                                                 optics_gfx_image **out) {
    /* Mirror the Linux implementation's validation order so callers and
     * tests observe the same argument errors on every platform; the
     * capability gap is reported only for a fully-formed desc. */
    if (!d || !desc || !out)
        return OPTICS_GFX_ERROR_INVALID_ARGUMENT;
    *out = nullptr;
    if (desc->type != OPTICS_GFX_TYPE_DMABUF_IMAGE_DESC) {
        OPTICS_GFX_FAIL(OPTICS_GFX_ERROR_INVALID_ARGUMENT,
                        "desc->type != FLUX_TYPE_DMABUF_IMAGE_DESC");
        return OPTICS_GFX_ERROR_INVALID_ARGUMENT;
    }
    if (desc->width == 0 || desc->height == 0) {
        OPTICS_GFX_FAIL(OPTICS_GFX_ERROR_INVALID_ARGUMENT, "dmabuf image zero-extent");
        return OPTICS_GFX_ERROR_INVALID_ARGUMENT;
    }
    if (desc->plane_count != 1) {
        OPTICS_GFX_FAIL(OPTICS_GFX_ERROR_UNSUPPORTED, "only single-plane dma-buf import supported");
        return OPTICS_GFX_ERROR_UNSUPPORTED;
    }
    if (desc->has_acquire_sync_fd && desc->acquire_sync_fd < 0) {
        OPTICS_GFX_FAIL(OPTICS_GFX_ERROR_INVALID_ARGUMENT, "dma-buf acquire_sync_fd is invalid");
        return OPTICS_GFX_ERROR_INVALID_ARGUMENT;
    }
    const optics_gfx_dmabuf_plane *plane = &desc->planes[0];
    if (plane->fd < 0) {
        OPTICS_GFX_FAIL(OPTICS_GFX_ERROR_INVALID_ARGUMENT, "dma-buf plane fd is invalid");
        return OPTICS_GFX_ERROR_INVALID_ARGUMENT;
    }
    if (plane->stride == 0) {
        OPTICS_GFX_FAIL(OPTICS_GFX_ERROR_INVALID_ARGUMENT, "dma-buf plane stride is zero");
        return OPTICS_GFX_ERROR_INVALID_ARGUMENT;
    }
    OPTICS_GFX_FAIL(OPTICS_GFX_ERROR_UNSUPPORTED, "dma-buf import is only available on Linux");
    return OPTICS_GFX_ERROR_UNSUPPORTED;
}
