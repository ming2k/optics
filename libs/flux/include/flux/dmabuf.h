/*
 * flux/dmabuf.h - import a Linux dma-buf as a sampled flux_image.
 *
 * A compositor receives client buffers as dma-buf file descriptors
 * (linux-dmabuf). This module wraps such a buffer as a flux_image that
 * the canvas can sample without a CPU pixel copy, using
 * VK_EXT_external_memory_dma_buf and VK_EXT_image_drm_format_modifier.
 *
 * Enable the required device extensions at flux_device_create by listing
 * them in required_device_extensions:
 *   VK_KHR_external_memory_fd
 *   VK_EXT_external_memory_dma_buf
 *   VK_EXT_image_drm_format_modifier
 *   VK_EXT_queue_family_foreign
 *   VK_KHR_external_semaphore_fd when using acquire_sync_fd
 *   VK_KHR_sampler_ycbcr_conversion (+ its deps) for future planar formats
 *
 * Design contract:
 *   - The imported flux_image is refcounted like an uploaded one and
 *     released with flux_image_release.
 *   - flux takes ownership of the passed dma-buf and acquire-sync file
 *     descriptors only when this call returns FLUX_OK. On failure the caller
 *     still owns them.
 *   - Single-plane formats only in this revision; plane_count != 1 returns
 *     FLUX_ERROR_UNSUPPORTED.
 *   - Set has_acquire_sync_fd + acquire_sync_fd to wait a producer sync_file
 *     fence before flux samples the image. If no acquire fence is supplied,
 *     the caller must ensure the producer is finished.
 */

#ifndef FLUX_DMABUF_H
#define FLUX_DMABUF_H

#include <flux/core.h>

#ifdef __cplusplus
extern "C" {
#endif

/* flux_image lives in <flux/canvas.h>; forward-declare so this header
 * compiles without dragging the full canvas surface in. */
typedef struct flux_image flux_image;

#define FLUX_DMABUF_MAX_PLANES 4

/* One dma-buf plane. `fd` is a dma-buf file descriptor; flux duplicates
 * what Vulkan imports so the ownership rule above is upheld on failures. */
typedef struct flux_dmabuf_plane {
    int fd;
    uint32_t offset;
    uint32_t stride;
} flux_dmabuf_plane;

typedef struct flux_dmabuf_image_desc {
    flux_struct_type type; /* FLUX_TYPE_DMABUF_IMAGE_DESC */
    const void *next;
    uint32_t width;
    uint32_t height;
    flux_format format; /* a flux_format matching the fourcc */
    uint64_t modifier;  /* DRM format modifier */
    uint32_t plane_count;
    flux_dmabuf_plane planes[FLUX_DMABUF_MAX_PLANES];
    bool has_acquire_sync_fd;
    int acquire_sync_fd; /* Linux sync_file fd */
} flux_dmabuf_image_desc;

#define FLUX_DMABUF_IMAGE_DESC_INIT {.type = FLUX_TYPE_DMABUF_IMAGE_DESC}

/* Import a dma-buf as a sampled flux_image. On FLUX_OK, flux owns and closes
 * the plane file descriptors and acquire_sync_fd when provided; on error the
 * caller keeps them. */
FLUX_NODISCARD FLUX_API flux_result flux_image_import_dmabuf(flux_device *d,
                                                             const flux_dmabuf_image_desc *desc,
                                                             flux_image **out);

/* Whether the device was created with the extensions dma-buf import needs.
 * This is an extension-level check; use flux_image_import_dmabuf to validate
 * a specific format/modifier pair. */
FLUX_API bool flux_dmabuf_supported(const flux_device *d);

/* Whether acquire_sync_fd can be imported through VK_KHR_external_semaphore_fd.
 * This is only required when flux_dmabuf_image_desc.has_acquire_sync_fd is true. */
FLUX_API bool flux_dmabuf_sync_supported(const flux_device *d);

#ifdef __cplusplus
}
#endif

#endif /* FLUX_DMABUF_H */
