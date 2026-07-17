/*
 * image_internal.h — internal flux_image layout + the storage-writable
 * image constructor. Shared by canvas/ (which owns the canonical image
 * lifecycle) and effect/ (which creates transient compute-writable
 * targets and reads back image state at dispatch time).
 *
 * Narrower than canvas/internal.h: pulling this out lets effect.c
 * depend on the image struct without dragging in the canvas pipeline
 * enum, push-constant layout, vertex struct, geometry helpers, or
 * path/paint internals.
 *
 * Never installed.
 */
#ifndef FLUX_IMAGE_INTERNAL_H
#define FLUX_IMAGE_INTERNAL_H

#include "../core/internal.h"
#include <flux/canvas.h>
#include <flux/vulkan.h>

struct flux_image {
    atomic_uint ref_count;
    flux_device *device; /* retained unless device_weak */
    bool device_weak;    /* true for effect-pool transients: owned
                          * by per-device module state, so a strong
                          * device ref would cycle and the device
                          * refcount could never reach zero. */
    uint32_t width;
    uint32_t height;
    flux_format format;
    VkImage image;
    flux_vk_alloc alloc;
    VkDeviceMemory imported_memory; /* dma-buf import: owned directly, freed
                                     * with vkFreeMemory (bypasses the slab).
                                     * VK_NULL_HANDLE for normal images. */
    VkDeviceSize imported_size;     /* bytes noted into allocator stats for
                                     * imported_memory; 0 when unset */
    VkImageView view;
    flux_bindless_handle bindless; /* SAMPLED_IMAGE slot, FLUX_BINDLESS_INVALID if unbound */
    flux_bindless_handle
        bindless_storage; /* STORAGE_IMAGE slot, FLUX_BINDLESS_INVALID for sampled-only images */
    VkImageLayout current_layout; /* tracked so cross-module recorders can emit correct barriers */
    bool render_target;           /* created with COLOR_ATTACHMENT usage */
};

/* Internal: create an image with STORAGE | SAMPLED | TRANSFER_SRC |
 * TRANSFER_DST usage, transitioned to GENERAL layout, registered into
 * both the SAMPLED and STORAGE bindless slots. The effect module uses
 * this for transient blur targets; not exposed publicly because the
 * usage / layout policy is implementation-defined. */
flux_result flux_image_create_compute_writable(flux_device *d, uint32_t width, uint32_t height,
                                               flux_format fmt, flux_image **out);

#endif /* FLUX_IMAGE_INTERNAL_H */
