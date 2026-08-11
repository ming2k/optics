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
};

#endif /* FLUX_IMAGE_INTERNAL_H */
