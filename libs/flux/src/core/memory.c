/*
 * GPU memory helpers: memory-type lookup, VkBuffer/VkImage create+bind
 * wrappers around the slab allocator (vk_allocator.c), and the
 * transient per-frame ring.
 *
 * These are the device-level primitives every other module uses to
 * back a VkDeviceMemory. They have no surface / frame affinity; the
 * one-shot upload helpers that consume them live in oneshot.c.
 */
#include "internal.h"

#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/*  Memory type helper                                                */
/* ------------------------------------------------------------------ */

uint32_t flux_vk_find_memory_type(flux_device *d, uint32_t type_filter,
                                  VkMemoryPropertyFlags wanted) {
    for (uint32_t i = 0; i < d->mem_props.memoryTypeCount; ++i) {
        if (!(type_filter & (1u << i)))
            continue;
        if ((d->mem_props.memoryTypes[i].propertyFlags & wanted) == wanted)
            return i;
    }
    return UINT32_MAX;
}

flux_result flux_vk_alloc_buffer(flux_device *d, VkDeviceSize size, VkBufferUsageFlags usage,
                                 VkMemoryPropertyFlags props, bool wants_device_address,
                                 VkBuffer *out_buffer, flux_vk_alloc *out_alloc) {
    *out_buffer = VK_NULL_HANDLE;
    *out_alloc = (flux_vk_alloc){0};

    VkBufferCreateInfo bci = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = size,
        .usage = usage,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    VkResult vr = vkCreateBuffer(d->device, &bci, nullptr, out_buffer);
    if (vr != VK_SUCCESS) {
        FLUX_FAIL_VK(FLUX_ERROR_BACKEND_FAILURE, "vkCreateBuffer failed", vr);
        return FLUX_ERROR_BACKEND_FAILURE;
    }

    VkMemoryRequirements mr;
    vkGetBufferMemoryRequirements(d->device, *out_buffer, &mr);

    flux_result r =
        flux_vk_allocate(d, mr, props, /*is_image=*/false, wants_device_address, out_alloc);
    if (r != FLUX_OK) {
        vkDestroyBuffer(d->device, *out_buffer, nullptr);
        *out_buffer = VK_NULL_HANDLE;
        return r;
    }
    vr = vkBindBufferMemory(d->device, *out_buffer, out_alloc->memory, out_alloc->offset);
    if (vr != VK_SUCCESS) {
        flux_vk_deallocate(d, out_alloc);
        vkDestroyBuffer(d->device, *out_buffer, nullptr);
        *out_buffer = VK_NULL_HANDLE;
        FLUX_FAIL_VK(FLUX_ERROR_BACKEND_FAILURE, "vkBindBufferMemory failed", vr);
        return FLUX_ERROR_BACKEND_FAILURE;
    }
    return FLUX_OK;
}

flux_result flux_vk_alloc_image(flux_device *d, const VkImageCreateInfo *ici,
                                VkMemoryPropertyFlags props, VkImage *out_image,
                                flux_vk_alloc *out_alloc) {
    *out_image = VK_NULL_HANDLE;
    *out_alloc = (flux_vk_alloc){0};

    VkResult vr = vkCreateImage(d->device, ici, nullptr, out_image);
    if (vr != VK_SUCCESS) {
        FLUX_FAIL_VK(FLUX_ERROR_BACKEND_FAILURE, "vkCreateImage failed", vr);
        return FLUX_ERROR_BACKEND_FAILURE;
    }

    VkMemoryRequirements mr;
    vkGetImageMemoryRequirements(d->device, *out_image, &mr);

    flux_result r = flux_vk_allocate(d, mr, props, /*is_image=*/true,
                                     /*wants_device_address=*/false, out_alloc);
    if (r != FLUX_OK) {
        vkDestroyImage(d->device, *out_image, nullptr);
        *out_image = VK_NULL_HANDLE;
        return r;
    }
    vr = vkBindImageMemory(d->device, *out_image, out_alloc->memory, out_alloc->offset);
    if (vr != VK_SUCCESS) {
        flux_vk_deallocate(d, out_alloc);
        vkDestroyImage(d->device, *out_image, nullptr);
        *out_image = VK_NULL_HANDLE;
        FLUX_FAIL_VK(FLUX_ERROR_BACKEND_FAILURE, "vkBindImageMemory failed", vr);
        return FLUX_ERROR_BACKEND_FAILURE;
    }
    return FLUX_OK;
}

/* Dedicated allocation for an exportable image. The caller's ici pNext
 * chain carries VkExternalMemoryImageCreateInfo (+ DRM-modifier struct);
 * `export_info` (VkMemoryDedicatedAllocateInfo etc.) is appended after our
 * flags-info so the memory is dedicated to this image and exportable. */
flux_result flux_vk_alloc_image_dedicated(flux_device *d, const VkImageCreateInfo *ici,
                                          VkMemoryPropertyFlags props, const void *export_info,
                                          VkImage *out_image, flux_vk_alloc *out_alloc) {
    *out_image = VK_NULL_HANDLE;
    *out_alloc = (flux_vk_alloc){0};

    VkResult vr = vkCreateImage(d->device, ici, nullptr, out_image);
    if (vr != VK_SUCCESS) {
        FLUX_FAIL_VK(FLUX_ERROR_BACKEND_FAILURE, "dedicated vkCreateImage failed", vr);
        return FLUX_ERROR_BACKEND_FAILURE;
    }

    VkMemoryRequirements2 mreq = {.sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2};
    VkImageMemoryRequirementsInfo2 mri = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2,
        .image = *out_image,
    };
    vkGetImageMemoryRequirements2(d->device, &mri, &mreq);

    /* External/exportable images only support a dedicated allocation in
     * practice; intersect the memory-type bits with the requested props. */
    uint32_t mt = flux_vk_find_memory_type(d, mreq.memoryRequirements.memoryTypeBits, props);
    if (mt == UINT32_MAX) {
        /* DEVICE_LOCAL may be unsatisfiable for some external formats; try
         * any type the image accepts. */
        mt = flux_vk_find_memory_type(d, mreq.memoryRequirements.memoryTypeBits, 0);
    }
    if (mt == UINT32_MAX) {
        vkDestroyImage(d->device, *out_image, nullptr);
        *out_image = VK_NULL_HANDLE;
        FLUX_FAIL(FLUX_ERROR_UNSUPPORTED, "no memory type for exportable image");
        return FLUX_ERROR_UNSUPPORTED;
    }

    /* pNext chain: export_info (dedicated alloc) -> MemoryAllocateInfo.
     * We never need device-address or host-visible mapping for an exported
     * offscreen colour attachment. */
    VkMemoryAllocateInfo mai = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .pNext = export_info,
        .allocationSize = mreq.memoryRequirements.size,
        .memoryTypeIndex = mt,
    };

    VkDeviceMemory mem = VK_NULL_HANDLE;
    vr = vkAllocateMemory(d->device, &mai, nullptr, &mem);
    if (vr != VK_SUCCESS) {
        vkDestroyImage(d->device, *out_image, nullptr);
        *out_image = VK_NULL_HANDLE;
        FLUX_FAIL_VK(FLUX_ERROR_BACKEND_FAILURE, "dedicated vkAllocateMemory failed", vr);
        return FLUX_ERROR_BACKEND_FAILURE;
    }

    vr = vkBindImageMemory(d->device, *out_image, mem, 0);
    if (vr != VK_SUCCESS) {
        vkFreeMemory(d->device, mem, nullptr);
        vkDestroyImage(d->device, *out_image, nullptr);
        *out_image = VK_NULL_HANDLE;
        FLUX_FAIL_VK(FLUX_ERROR_BACKEND_FAILURE, "dedicated vkBindImageMemory failed", vr);
        return FLUX_ERROR_BACKEND_FAILURE;
    }

    flux_vk_allocator *a = &d->mem_allocator;
    pthread_mutex_lock(&a->lock);
    a->bytes_in_use += mreq.memoryRequirements.size;
    a->bytes_reserved += mreq.memoryRequirements.size;
    a->live_allocations++;
    pthread_mutex_unlock(&a->lock);

    out_alloc->memory = mem;
    out_alloc->offset = 0;
    out_alloc->size = mreq.memoryRequirements.size;
    out_alloc->mapped = NULL;
    out_alloc->block = NULL;
    return FLUX_OK;
}

/* ------------------------------------------------------------------ */
/*  Transient memory ring                                             */
/* ------------------------------------------------------------------ */

flux_result flux_transient_ring_init(flux_transient_ring *r, flux_device *d,
                                     VkDeviceSize per_frame) {
    memset(r, 0, sizeof(*r));
    if (per_frame == 0)
        per_frame = 16ull * 1024 * 1024; /* 16 MiB default */
    if (d->frames_in_flight == 0 || per_frame > UINT64_MAX / d->frames_in_flight) {
        FLUX_FAIL(FLUX_ERROR_OUT_OF_RANGE, "transient ring size overflow");
        return FLUX_ERROR_OUT_OF_RANGE;
    }
    r->per_frame_size = per_frame;
    r->total_size = per_frame * d->frames_in_flight;

    /* The transient ring's whole-buffer mapping requirement and BDA
     * usage make it a natural dedicated allocation. The allocator's
     * oversize threshold pushes a 32+ MiB request straight to a
     * dedicated VkDeviceMemory, so we don't carve it out of a pool. */
    flux_result rr = flux_vk_alloc_buffer(
        d, r->total_size,
        VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT |
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
            VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
        /*wants_device_address=*/true, &r->buffer, &r->alloc);
    if (rr != FLUX_OK)
        return rr;

    /* Allocator already mapped the dedicated block (HOST_VISIBLE path).
     * For pooled allocations the per-block map handles it too. */
    if (r->alloc.mapped) {
        r->mapped = (uint8_t *)r->alloc.mapped;
    } else {
        void *m = nullptr;
        VkResult vr =
            vkMapMemory(d->device, r->alloc.memory, r->alloc.offset, r->alloc.size, 0, &m);
        if (vr != VK_SUCCESS) {
            FLUX_FAIL_VK(FLUX_ERROR_BACKEND_FAILURE, "transient ring vkMapMemory", vr);
            flux_transient_ring_destroy(r, d);
            return FLUX_ERROR_BACKEND_FAILURE;
        }
        r->mapped = (uint8_t *)m;
    }

    VkBufferDeviceAddressInfo bdai = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO,
        .buffer = r->buffer,
    };
    r->device_address = vkGetBufferDeviceAddress(d->device, &bdai);
    return FLUX_OK;
}

void flux_transient_ring_destroy(flux_transient_ring *r, flux_device *d) {
    if (!r || !d)
        return;
    if (r->buffer)
        vkDestroyBuffer(d->device, r->buffer, nullptr);
    if (r->alloc.memory)
        flux_vk_deallocate(d, &r->alloc);
    memset(r, 0, sizeof(*r));
}
