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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/*  Memory type helper                                                */
/* ------------------------------------------------------------------ */

uint32_t optics_gfx_vk_find_memory_type(optics_gfx_device *d, uint32_t type_filter,
                                        VkMemoryPropertyFlags wanted) {
    for (uint32_t i = 0; i < d->mem_props.memoryTypeCount; ++i) {
        if (!(type_filter & (1u << i)))
            continue;
        if ((d->mem_props.memoryTypes[i].propertyFlags & wanted) == wanted)
            return i;
    }
    return UINT32_MAX;
}

optics_gfx_result optics_gfx_vk_alloc_buffer(optics_gfx_device *d, VkDeviceSize size,
                                             VkBufferUsageFlags usage, VkMemoryPropertyFlags props,
                                             bool wants_device_address, VkBuffer *out_buffer,
                                             optics_gfx_vk_alloc *out_alloc) {
    *out_buffer = VK_NULL_HANDLE;
    *out_alloc = (optics_gfx_vk_alloc){};

    VkBufferCreateInfo bci = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = size,
        .usage = usage,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    VkResult vr = vkCreateBuffer(d->device, &bci, nullptr, out_buffer);
    if (vr != VK_SUCCESS) {
        OPTICS_GFX_FAIL_VK(OPTICS_GFX_ERROR_BACKEND_FAILURE, "vkCreateBuffer failed", vr);
        return OPTICS_GFX_ERROR_BACKEND_FAILURE;
    }

    /* Chain VkMemoryDedicatedRequirements so the allocator can honour a
     * driver-mandated (or preferred) dedicated allocation. */
    VkMemoryDedicatedRequirements ded_req = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS,
    };
    VkMemoryRequirements2 mreq = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2,
        .pNext = &ded_req,
    };
    VkBufferMemoryRequirementsInfo2 mri = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_REQUIREMENTS_INFO_2,
        .buffer = *out_buffer,
    };
    vkGetBufferMemoryRequirements2(d->device, &mri, &mreq);

    optics_gfx_vk_dedication ded = {
        .buffer = *out_buffer,
        .required = ded_req.requiresDedicatedAllocation == VK_TRUE,
        .preferred = ded_req.prefersDedicatedAllocation == VK_TRUE,
    };
    optics_gfx_result r =
        optics_gfx_vk_allocate(d, mreq.memoryRequirements, props, /*is_image=*/false,
                               wants_device_address, &ded, out_alloc);
    if (r != OPTICS_GFX_OK) {
        vkDestroyBuffer(d->device, *out_buffer, nullptr);
        *out_buffer = VK_NULL_HANDLE;
        return r;
    }
    vr = vkBindBufferMemory(d->device, *out_buffer, out_alloc->memory, out_alloc->offset);
    if (vr != VK_SUCCESS) {
        optics_gfx_vk_deallocate(d, out_alloc);
        vkDestroyBuffer(d->device, *out_buffer, nullptr);
        *out_buffer = VK_NULL_HANDLE;
        OPTICS_GFX_FAIL_VK(OPTICS_GFX_ERROR_BACKEND_FAILURE, "vkBindBufferMemory failed", vr);
        return OPTICS_GFX_ERROR_BACKEND_FAILURE;
    }
    return OPTICS_GFX_OK;
}

optics_gfx_result optics_gfx_vk_alloc_image(optics_gfx_device *d, const VkImageCreateInfo *ici,
                                            VkMemoryPropertyFlags props, VkImage *out_image,
                                            optics_gfx_vk_alloc *out_alloc) {
    *out_image = VK_NULL_HANDLE;
    *out_alloc = (optics_gfx_vk_alloc){};

    VkResult vr = vkCreateImage(d->device, ici, nullptr, out_image);
    if (vr != VK_SUCCESS) {
        OPTICS_GFX_FAIL_VK(OPTICS_GFX_ERROR_BACKEND_FAILURE, "vkCreateImage failed", vr);
        return OPTICS_GFX_ERROR_BACKEND_FAILURE;
    }

    VkMemoryDedicatedRequirements ded_req = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS,
    };
    VkMemoryRequirements2 mreq = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2,
        .pNext = &ded_req,
    };
    VkImageMemoryRequirementsInfo2 mri = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2,
        .image = *out_image,
    };
    vkGetImageMemoryRequirements2(d->device, &mri, &mreq);

    optics_gfx_vk_dedication ded = {
        .image = *out_image,
        .required = ded_req.requiresDedicatedAllocation == VK_TRUE,
        .preferred = ded_req.prefersDedicatedAllocation == VK_TRUE,
    };
    optics_gfx_result r =
        optics_gfx_vk_allocate(d, mreq.memoryRequirements, props, /*is_image=*/true,
                               /*wants_device_address=*/false, &ded, out_alloc);
    if (r != OPTICS_GFX_OK) {
        vkDestroyImage(d->device, *out_image, nullptr);
        *out_image = VK_NULL_HANDLE;
        return r;
    }
    vr = vkBindImageMemory(d->device, *out_image, out_alloc->memory, out_alloc->offset);
    if (vr != VK_SUCCESS) {
        optics_gfx_vk_deallocate(d, out_alloc);
        vkDestroyImage(d->device, *out_image, nullptr);
        *out_image = VK_NULL_HANDLE;
        OPTICS_GFX_FAIL_VK(OPTICS_GFX_ERROR_BACKEND_FAILURE, "vkBindImageMemory failed", vr);
        return OPTICS_GFX_ERROR_BACKEND_FAILURE;
    }
    return OPTICS_GFX_OK;
}

/* ------------------------------------------------------------------ */
/*  Transient memory ring                                             */
/* ------------------------------------------------------------------ */

optics_gfx_result optics_gfx_transient_ring_init(optics_gfx_transient_ring *r, optics_gfx_device *d,
                                                 VkDeviceSize per_frame) {
    memset(r, 0, sizeof(*r));
    if (per_frame == 0)
        per_frame = 16ull * 1024 * 1024; /* 16 MiB default */
    if (d->frames_in_flight == 0 || per_frame > UINT64_MAX / d->frames_in_flight) {
        OPTICS_GFX_FAIL(OPTICS_GFX_ERROR_OUT_OF_RANGE, "transient ring size overflow");
        return OPTICS_GFX_ERROR_OUT_OF_RANGE;
    }
    r->per_frame_size = per_frame;
    r->total_size = per_frame * d->frames_in_flight;

    /* The transient ring's whole-buffer mapping requirement and BDA
     * usage make it a natural dedicated allocation. The allocator's
     * oversize threshold pushes a 32+ MiB request straight to a
     * dedicated VkDeviceMemory, so we don't carve it out of a pool. */
    optics_gfx_result rr = optics_gfx_vk_alloc_buffer(
        d, r->total_size,
        VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT |
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
            VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
        /*wants_device_address=*/true, &r->buffer, &r->alloc);
    if (rr != OPTICS_GFX_OK)
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
            OPTICS_GFX_FAIL_VK(OPTICS_GFX_ERROR_BACKEND_FAILURE, "transient ring vkMapMemory", vr);
            optics_gfx_transient_ring_destroy(r, d);
            return OPTICS_GFX_ERROR_BACKEND_FAILURE;
        }
        r->mapped = (uint8_t *)m;
    }

    VkBufferDeviceAddressInfo bdai = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO,
        .buffer = r->buffer,
    };
    r->device_address = vkGetBufferDeviceAddress(d->device, &bdai);
    optics_gfx_vk_set_name(d, VK_OBJECT_TYPE_BUFFER, (uint64_t)r->buffer, "gfx transient ring");
    return OPTICS_GFX_OK;
}

void optics_gfx_transient_ring_destroy(optics_gfx_transient_ring *r, optics_gfx_device *d) {
    if (!r || !d)
        return;
    if (r->buffer)
        vkDestroyBuffer(d->device, r->buffer, nullptr);
    if (r->alloc.memory)
        optics_gfx_vk_deallocate(d, &r->alloc);
    memset(r, 0, sizeof(*r));
}
