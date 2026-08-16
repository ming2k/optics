/*
 * Internal flux_image layout. Images are core GPU resources shared by
 * canvas, scene, and effect. Never installed.
 */
#ifndef FLUX_IMAGE_INTERNAL_H
#define FLUX_IMAGE_INTERNAL_H

#include "internal.h"
#include <flux/vulkan.h>

struct flux_image {
    atomic_uint ref_count;
    flux_device *device; /* retained unless device_weak */
    bool device_weak;    /* true for effect-pool transients owned by device state */
    uint32_t width;
    uint32_t height;
    flux_format format;
    VkImage image;
    flux_vk_alloc alloc;
    VkDeviceMemory imported_memory;
    VkDeviceSize imported_size;
    bool foreign_owned;
    VkImageView view;
    flux_bindless_handle bindless;
    flux_bindless_handle bindless_storage;
    VkImageLayout current_layout;
    bool render_target;

    /* ADR-0069/0070 content color space (flux_image_color_space_desc).
     * color_params_address == 0 selects the format-derived fast path
     * (sRGB for 8-bit, linear for 16F); otherwise it is the device
     * address of a flux_image_color_params block. LUT-tagged content
     * additionally owns the baked 3D LUT (2D-laid-out 16F image). */
    flux_icc_profile *icc;
    flux_buffer *color_params;
    uint64_t color_params_address;
    VkImage lut_image;
    flux_vk_alloc lut_alloc;
    VkImageView lut_view;
    flux_bindless_handle lut_bindless;
};

/* Resolve an image's content color space from a creation desc's `next`
 * chain (ADR-0069/0070 flux_image_color_space_desc) and build the
 * GPU-side params block / baked 3D LUT when tagged. Shared by the
 * upload path (flux_image_create) and the dma-buf import path. */
flux_result flux_image_init_color(flux_device *d, flux_image *im, const void *next_chain);

#endif /* FLUX_IMAGE_INTERNAL_H */
