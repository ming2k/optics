/*
 * Internal flux_image layout. Images are core GPU resources shared by
 * canvas, scene, and effect. Never installed.
 */
#ifndef OPTICS_GFX_IMAGE_INTERNAL_H
#define OPTICS_GFX_IMAGE_INTERNAL_H

#include "internal.h"
#include <optics/gfx/vulkan.h>

/* GPU resources shared by an image handle and its immutable snapshots. The
 * handle owns one reference; an update that would mutate a pinned version
 * copies the pixels into a fresh image and detaches the handle. */
/* Immutable color interpretation shared by an image and every pixel version
 * derived from it. ICC profiles, the params buffer, and the baked LUT are
 * content-independent, so copy-on-write never duplicates them. */
typedef struct optics_gfx_image_color {
    atomic_uint refs;
    optics_gfx_icc_profile *icc;
    optics_gfx_buffer *color_params;
    uint64_t color_params_address;
    VkImage lut_image;
    optics_gfx_vk_alloc lut_alloc;
    VkImageView lut_view;
    optics_gfx_bindless_handle lut_bindless;
} optics_gfx_image_color;

typedef struct optics_gfx_image_shared {
    atomic_uint refs;
    VkImage image;
    optics_gfx_vk_alloc alloc;
    VkDeviceMemory imported_memory;
    VkDeviceSize imported_size;
    VkImageView view;
    optics_gfx_bindless_handle bindless;
    optics_gfx_bindless_handle bindless_storage;
} optics_gfx_image_shared;

struct optics_gfx_image {
    atomic_uint ref_count;
    optics_gfx_device *device;       /* retained unless device_weak */
    bool device_weak;                /* true for effect-pool transients owned by device state */
    optics_gfx_image_shared *shared; /* non-NULL once a snapshot exists */
    bool mutable;                    /* false for immutable snapshot handles */
    bool imported;                   /* external dma-buf memory, never written */
    bool snapshot_allowed;           /* false for borrowed imports */
    uint32_t width;
    uint32_t height;
    optics_gfx_format format;
    VkImage image;
    optics_gfx_vk_alloc alloc;
    VkDeviceMemory imported_memory;
    VkDeviceSize imported_size;
    bool foreign_owned;
    VkImageView view;
    optics_gfx_bindless_handle bindless;
    optics_gfx_bindless_handle bindless_storage;
    VkImageLayout current_layout;
    bool render_target;

    /* True while this image is the output of a registered pass that has not
     * executed (ADR-0147). The plan is the only writer: registration sets it,
     * execution and pruning clear it. It lives on the image rather than only in
     * the plan because a raw recording path (`flux_effect_*`, a custom compute
     * pass driven by a bare VkCommandBuffer) has no frame to ask. */
    bool deferred_pending;

    /* ADR-0069/0070 content color space (flux_image_color_space_desc).
     * NULL selects the format-derived fast path (sRGB for 8-bit, linear for
     * 16F). Shared with every pixel version derived from this image. */
    optics_gfx_image_color *color;
};

/* Resolve an image's content color space from a creation desc's `next`
 * chain (ADR-0069/0070 flux_image_color_space_desc) and build the
 * GPU-side params block / baked 3D LUT when tagged. Shared by the
 * upload path (flux_image_create) and the dma-buf import path. */
optics_gfx_result optics_gfx_image_init_color(optics_gfx_device *d, optics_gfx_image *im,
                                              const void *next_chain);
void optics_gfx_image_release_color(optics_gfx_device *d, optics_gfx_image_color *color);

#endif /* FLUX_IMAGE_INTERNAL_H */
