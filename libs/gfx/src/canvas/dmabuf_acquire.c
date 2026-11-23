/*
 * gfx/dmabuf_acquire.c — optics_gfx_canvas_wait_dmabuf_acquire, the one dma-buf
 * entry point that needs canvas internals (struct optics_gfx_canvas,
 * canvas_track_foreign_image). Everything else dma-buf is core-level and
 * lives in src/core/dmabuf.c / dmabuf_stub.c (ADR-0052); this file is
 * compiled with the canvas module on every host and stays free of platform
 * #ifdef by delegating the sync_file import to
 * optics_gfx_dmabuf_import_acquire_semaphore, whose stub twin answers
 * OPTICS_GFX_ERROR_UNSUPPORTED off Linux.
 */

#include "internal.h"
#include <optics/gfx/dmabuf.h>

optics_gfx_result
optics_gfx_canvas_wait_dmabuf_acquire(optics_gfx_canvas *canvas,
                                      const optics_gfx_canvas_wait_dmabuf_acquire_opts *opts) {
    if (!canvas || !opts || !opts->image || opts->acquire_sync_fd < 0)
        return OPTICS_GFX_ERROR_INVALID_ARGUMENT;
    optics_gfx_image *image = opts->image;
    int acquire_sync_fd = opts->acquire_sync_fd;
    if (!canvas->recording || !canvas->frame) {
        OPTICS_GFX_FAIL(OPTICS_GFX_ERROR_INVALID_STATE,
                        "dma-buf acquire wait requires an active canvas frame");
        return OPTICS_GFX_ERROR_INVALID_STATE;
    }
    if (!image->imported_memory || image->device != canvas->device) {
        OPTICS_GFX_FAIL(OPTICS_GFX_ERROR_INVALID_ARGUMENT,
                        "dma-buf acquire wait requires an imported image on this canvas device");
        return OPTICS_GFX_ERROR_INVALID_ARGUMENT;
    }
    if (!canvas_track_foreign_image(canvas, image)) {
        OPTICS_GFX_FAIL(OPTICS_GFX_ERROR_OUT_OF_MEMORY, "failed to track reusable dma-buf image");
        return OPTICS_GFX_ERROR_OUT_OF_MEMORY;
    }

    VkSemaphore semaphore = VK_NULL_HANDLE;
    optics_gfx_result r =
        optics_gfx_dmabuf_import_acquire_semaphore(canvas->device, acquire_sync_fd, &semaphore);
    if (r != OPTICS_GFX_OK)
        return r;
    if (!optics_gfx_frame_set_foreign_image_acquire(canvas->frame, image, semaphore)) {
        vkDestroySemaphore(canvas->device->device, semaphore, nullptr);
        OPTICS_GFX_FAIL(OPTICS_GFX_ERROR_INVALID_STATE,
                        "dma-buf image already has an acquire wait in this frame");
        return OPTICS_GFX_ERROR_INVALID_STATE;
    }

    optics_gfx_dmabuf_close_fd(acquire_sync_fd);
    return OPTICS_GFX_OK;
}
