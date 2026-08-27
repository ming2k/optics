/*
 * flux/dmabuf.h - import a Linux dma-buf as a sampled flux_image.
 *
 * A compositor receives client buffers as dma-buf file descriptors
 * (linux-dmabuf). This module wraps such a buffer as a flux_image that
 * the canvas can sample without a CPU pixel copy, using
 * VK_EXT_external_memory_dma_buf and VK_EXT_image_drm_format_modifier.
 *
 * Request FLUX_DEVICE_FEATURE_DMABUF through flux_device_features_desc at
 * flux_device_create. Request FLUX_DEVICE_FEATURE_DMABUF_SYNC_FILE when using
 * acquire_sync_fd. Flux owns the Vulkan extension bundle behind those
 * semantic capabilities.
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
typedef struct flux_canvas flux_canvas;

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
    const void *next;      /* optional flux_image_color_space_desc (ADR-0069/0070):
                            * tags the imported content's color space so texels
                            * decode into the working space when sampled */
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

/* Wait a new producer sync_file before the current canvas frame samples an
 * already-imported dma-buf image. This is the reusable-buffer counterpart to
 * flux_dmabuf_image_desc.acquire_sync_fd: it avoids rebuilding the VkImage on
 * every client commit.
 *
 * Call after flux_canvas_begin_frame and before drawing `image`. On FLUX_OK flux
 * owns and closes acquire_sync_fd; on error the caller keeps it. */
FLUX_NODISCARD FLUX_API flux_result flux_canvas_wait_dmabuf_acquire(flux_canvas *canvas,
                                                                    flux_image *image,
                                                                    int acquire_sync_fd);

/* Whether the logical device has the complete dma-buf import capability.
 * Use flux_dmabuf_format_modifiers or flux_image_import_dmabuf to validate a
 * specific format/modifier pair. */
FLUX_API bool flux_dmabuf_supported(const flux_device *d);

/* Whether acquire_sync_fd can be imported through VK_KHR_external_semaphore_fd.
 * This is only required when flux_dmabuf_image_desc.has_acquire_sync_fd is true. */
FLUX_API bool flux_dmabuf_sync_supported(const flux_device *d);

/* Enumerate the single-plane DRM format modifiers a buffer of `format` may use
 * to be both sampleable by this device and importable as external memory, i.e.
 * the set a compositor may advertise alongside the matching fourcc through
 * zwp_linux_dmabuf_v1 so clients allocate GPU-optimal (tiled/compressed)
 * layouts rather than falling back to DRM_FORMAT_MOD_LINEAR.
 *
 * `out_modifiers` points to a caller-owned buffer of `*inout_count` slots.
 * On FLUX_OK `*inout_count` receives the number of modifiers written. If the
 * buffer is too small the call returns FLUX_ERROR_INVALID_ARGUMENT and sets
 * `*inout_count` to the required length (without writing anything), so a
 * two-pass caller can probe then allocate. Pass `format` as a flux_format
 * matching the fourcc the host will advertise. */
FLUX_NODISCARD FLUX_API flux_result flux_dmabuf_format_modifiers(flux_device *d, flux_format format,
                                                                 uint64_t *out_modifiers,
                                                                 uint32_t *inout_count);

#ifdef __cplusplus
}
#endif

#endif /* FLUX_DMABUF_H */
