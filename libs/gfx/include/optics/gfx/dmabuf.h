/*
 * gfx/dmabuf.h - import a Linux dma-buf as a sampled optics_gfx_image.
 *
 * A compositor receives client buffers as dma-buf file descriptors
 * (linux-dmabuf). This module wraps such a buffer as a optics_gfx_image that
 * the canvas can sample without a CPU pixel copy, using
 * VK_EXT_external_memory_dma_buf and VK_EXT_image_drm_format_modifier.
 *
 * Request OPTICS_GFX_DEVICE_FEATURE_DMABUF through optics_gfx_device_features_desc at
 * optics_gfx_device_create. Request OPTICS_GFX_DEVICE_FEATURE_DMABUF_SYNC_FILE when using
 * acquire_sync_fd. gfx owns the Vulkan extension bundle behind those
 * semantic capabilities.
 *   VK_KHR_sampler_ycbcr_conversion (+ its deps) for future planar formats
 *
 * Design contract:
 *   - The imported optics_gfx_image is refcounted like an uploaded one and
 *     released with optics_gfx_image_release.
 *   - gfx takes ownership of the passed dma-buf and acquire-sync file
 *     descriptors only when this call returns OPTICS_GFX_OK. On failure the caller
 *     still owns them.
 *   - Single-plane formats only in this revision; plane_count != 1 returns
 *     OPTICS_GFX_ERROR_UNSUPPORTED.
 *   - Set has_acquire_sync_fd + acquire_sync_fd to wait a producer sync_file
 *     fence before gfx samples the image. If no acquire fence is supplied,
 *     the caller must ensure the producer is finished.
 */

#ifndef OPTICS_GFX_DMABUF_H
#define OPTICS_GFX_DMABUF_H

#include <optics/gfx/core.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* optics_gfx_image lives in <optics/gfx/canvas.h>; forward-declare so this header
 * compiles without dragging the full canvas surface in. */
typedef struct optics_gfx_image optics_gfx_image;
typedef struct optics_gfx_canvas optics_gfx_canvas;

#define OPTICS_GFX_DMABUF_MAX_PLANES 4

/* What the producer transferred with the dma-buf.
 *
 * BORROWED is the safe default: the buffer may be sampled for the current
 * frame/render pass, but the producer may reuse it after the acquire fence
 * retires. Such an image cannot be snapshotted into a retained DisplayList;
 * bind it through a dynamic display-list input slot instead.
 *
 * EXCLUSIVE means the producer has transferred ownership for the image's
 * lifetime. The image may be snapshotted and retained across frames, but it is
 * still immutable: gfx never writes to imported memory. */
typedef enum optics_gfx_dmabuf_ownership : uint32_t {
    OPTICS_GFX_DMABUF_OWNERSHIP_BORROWED = 0,
    OPTICS_GFX_DMABUF_OWNERSHIP_EXCLUSIVE = 1,
} optics_gfx_dmabuf_ownership;

/* One dma-buf plane. `fd` is a dma-buf file descriptor; gfx duplicates
 * what Vulkan imports so the ownership rule above is upheld on failures. */
typedef struct optics_gfx_dmabuf_plane {
    int fd;
    uint32_t offset;
    uint32_t stride;
} optics_gfx_dmabuf_plane;

typedef struct optics_gfx_dmabuf_image_desc {
    optics_gfx_struct_type type; /* OPTICS_GFX_TYPE_DMABUF_IMAGE_DESC */
    const void *next;            /* optional optics_gfx_image_color_space_desc (ADR-0069/0070):
                                  * tags the imported content's color space so texels
                                  * decode into the working space when sampled */
    uint32_t width;
    uint32_t height;
    optics_gfx_format format; /* a optics_gfx_format matching the fourcc */
    uint64_t modifier;        /* DRM format modifier */
    uint32_t plane_count;
    optics_gfx_dmabuf_plane planes[OPTICS_GFX_DMABUF_MAX_PLANES];
    bool has_acquire_sync_fd;
    int acquire_sync_fd;                   /* Linux sync_file fd */
    optics_gfx_dmabuf_ownership ownership; /* BORROWED (default) or EXCLUSIVE */
} optics_gfx_dmabuf_image_desc;

#define OPTICS_GFX_DMABUF_IMAGE_DESC_INIT {.type = OPTICS_GFX_TYPE_DMABUF_IMAGE_DESC}

/* Import a dma-buf as a sampled optics_gfx_image. On OPTICS_GFX_OK, gfx owns and closes
 * the plane file descriptors and acquire_sync_fd when provided; on error the
 * caller keeps them. */
OPTICS_GFX_NODISCARD OPTICS_GFX_API optics_gfx_result optics_gfx_image_import_dmabuf(
    optics_gfx_device *d, const optics_gfx_dmabuf_image_desc *desc, optics_gfx_image **out);

/* Wait a new producer sync_file before the current canvas frame samples an
 * already-imported dma-buf image. This is the reusable-buffer counterpart to
 * optics_gfx_dmabuf_image_desc.acquire_sync_fd: it avoids rebuilding the VkImage on
 * every client commit.
 *
 * Call after optics_gfx_canvas_begin and before drawing `image`. On OPTICS_GFX_OK gfx
 * owns and closes acquire_sync_fd; on error the caller keeps it. */
typedef struct optics_gfx_canvas_wait_dmabuf_acquire_opts {
    optics_gfx_image *image;
    int acquire_sync_fd;
} optics_gfx_canvas_wait_dmabuf_acquire_opts;

OPTICS_GFX_NODISCARD OPTICS_GFX_API optics_gfx_result optics_gfx_canvas_wait_dmabuf_acquire(
    optics_gfx_canvas *canvas, const optics_gfx_canvas_wait_dmabuf_acquire_opts *opts);

/* Whether the logical device has the complete dma-buf import capability.
 * Use optics_gfx_dmabuf_format_modifiers or optics_gfx_image_import_dmabuf to validate a
 * specific format/modifier pair. */
OPTICS_GFX_API bool optics_gfx_dmabuf_supported(const optics_gfx_device *d);

/* Whether acquire_sync_fd can be imported through VK_KHR_external_semaphore_fd.
 * This is only required when optics_gfx_dmabuf_image_desc.has_acquire_sync_fd is true. */
OPTICS_GFX_API bool optics_gfx_dmabuf_sync_supported(const optics_gfx_device *d);

/* Enumerate the single-plane DRM format modifiers a buffer of `format` may use
 * to be both sampleable by this device and importable as external memory, i.e.
 * the set a compositor may advertise alongside the matching fourcc through
 * zwp_linux_dmabuf_v1 so clients allocate GPU-optimal (tiled/compressed)
 * layouts rather than falling back to DRM_FORMAT_MOD_LINEAR.
 *
 * `out_modifiers` points to a caller-owned buffer of `*inout_count` slots.
 * On OPTICS_GFX_OK `*inout_count` receives the number of modifiers written. If the
 * buffer is too small the call returns OPTICS_GFX_ERROR_INVALID_ARGUMENT and sets
 * `*inout_count` to the required length (without writing anything), so a
 * two-pass caller can probe then allocate. Pass `format` as a optics_gfx_format
 * matching the fourcc the host will advertise. */
typedef struct optics_gfx_dmabuf_format_modifiers_opts {
    optics_gfx_format format;
    uint64_t *out_modifiers;
    uint32_t *inout_count;
} optics_gfx_dmabuf_format_modifiers_opts;

OPTICS_GFX_NODISCARD OPTICS_GFX_API optics_gfx_result optics_gfx_dmabuf_format_modifiers(
    optics_gfx_device *d, const optics_gfx_dmabuf_format_modifiers_opts *opts);

#ifdef __cplusplus
}
#endif

#endif /* OPTICS_GFX_DMABUF_H */
