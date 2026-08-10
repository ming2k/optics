/*
 * flux/dmabuf_acquire.c — flux_canvas_wait_dmabuf_acquire, the one dma-buf
 * entry point that needs canvas internals (struct flux_canvas,
 * canvas_track_foreign_image). Everything else dma-buf is core-level and
 * lives in src/core/dmabuf.c / dmabuf_stub.c (ADR-0052); this file is
 * compiled with the canvas module on every host and stays free of platform
 * #ifdef by delegating the sync_file import to
 * flux_dmabuf_import_acquire_semaphore, whose stub twin answers
 * FLUX_ERROR_UNSUPPORTED off Linux.
 */

#include "internal.h"
#include <flux/dmabuf.h>

flux_result flux_canvas_wait_dmabuf_acquire(flux_canvas *canvas, flux_image *image,
                                            int acquire_sync_fd) {
    if (!canvas || !image || acquire_sync_fd < 0)
        return FLUX_ERROR_INVALID_ARGUMENT;
    if (!canvas->recording || !canvas->frame) {
        FLUX_FAIL(FLUX_ERROR_INVALID_STATE, "dma-buf acquire wait requires an active canvas frame");
        return FLUX_ERROR_INVALID_STATE;
    }
    if (!image->imported_memory || image->device != canvas->device) {
        FLUX_FAIL(FLUX_ERROR_INVALID_ARGUMENT,
                  "dma-buf acquire wait requires an imported image on this canvas device");
        return FLUX_ERROR_INVALID_ARGUMENT;
    }
    if (!canvas_track_foreign_image(canvas, image)) {
        FLUX_FAIL(FLUX_ERROR_OUT_OF_MEMORY, "failed to track reusable dma-buf image");
        return FLUX_ERROR_OUT_OF_MEMORY;
    }

    VkSemaphore semaphore = VK_NULL_HANDLE;
    flux_result r =
        flux_dmabuf_import_acquire_semaphore(canvas->device, acquire_sync_fd, &semaphore);
    if (r != FLUX_OK)
        return r;
    if (!flux_frame_set_foreign_image_acquire(canvas->frame, image, semaphore)) {
        vkDestroySemaphore(canvas->device->device, semaphore, nullptr);
        FLUX_FAIL(FLUX_ERROR_INVALID_STATE,
                  "dma-buf image already has an acquire wait in this frame");
        return FLUX_ERROR_INVALID_STATE;
    }

    flux_dmabuf_close_fd(acquire_sync_fd);
    return FLUX_OK;
}
