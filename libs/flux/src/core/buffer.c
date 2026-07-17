/*
 * Refcounted GPU buffer with explicit location choice.
 *
 * GPU_LOCAL: backed by DEVICE_LOCAL memory; not host-mappable. If
 * initial_data is set, the data is uploaded via a one-shot staging
 * buffer + transient command buffer (reuses flux_vk_upload_to_buffer).
 *
 * HOST_VISIBLE: backed by HOST_VISIBLE|HOST_COHERENT memory and
 * persistently mapped. The mapped pointer is returned by
 * flux_buffer_mapped(); writes are immediately visible to the GPU
 * on a coherent buffer.
 */
#include "internal.h"
#include <flux/vulkan.h>

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

struct flux_buffer {
    atomic_uint ref_count;
    flux_device *device; /* retained */
    VkBuffer buffer;
    flux_vk_alloc alloc;
    size_t size;
    void *mapped;            /* NULL for GPU_LOCAL */
    uint64_t device_address; /* 0 if not requested */
};

static VkBufferUsageFlags to_vk_usage(uint32_t mask, bool with_dst, bool with_address) {
    VkBufferUsageFlags u = 0;
    if (mask & FLUX_BUFFER_USAGE_VERTEX)
        u |= VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    if (mask & FLUX_BUFFER_USAGE_INDEX)
        u |= VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
    if (mask & FLUX_BUFFER_USAGE_UNIFORM)
        u |= VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
    if (mask & FLUX_BUFFER_USAGE_STORAGE)
        u |= VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    if (mask & FLUX_BUFFER_USAGE_TRANSFER_SRC)
        u |= VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    if (mask & FLUX_BUFFER_USAGE_TRANSFER_DST)
        u |= VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    if (with_dst)
        u |= VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    if (with_address)
        u |= VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    return u;
}

flux_result flux_buffer_create(flux_device *d, const flux_buffer_desc *desc, flux_buffer **out) {
    if (!d || !desc || !out)
        return FLUX_ERROR_INVALID_ARGUMENT;
    if (desc->type != FLUX_TYPE_BUFFER_DESC) {
        FLUX_FAIL(FLUX_ERROR_INVALID_ARGUMENT, "desc->type != FLUX_TYPE_BUFFER_DESC");
        return FLUX_ERROR_INVALID_ARGUMENT;
    }
    if (desc->size == 0) {
        FLUX_FAIL(FLUX_ERROR_INVALID_ARGUMENT, "buffer size is 0");
        return FLUX_ERROR_INVALID_ARGUMENT;
    }
    *out = nullptr;

    flux_buffer *b = flux_internal_alloc(d, sizeof(*b));
    if (!b)
        return FLUX_ERROR_OUT_OF_MEMORY;
    atomic_init(&b->ref_count, 1u);
    b->device = flux_device_retain(d);
    b->size = desc->size;

    bool need_staging = desc->location == FLUX_BUFFER_GPU_LOCAL && desc->initial_data != nullptr;
    VkBufferUsageFlags vk_usage = to_vk_usage(desc->usage, need_staging, desc->device_address);
    VkMemoryPropertyFlags vk_props =
        desc->location == FLUX_BUFFER_GPU_LOCAL
            ? VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT
            : (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

    flux_result r = flux_vk_alloc_buffer(d, desc->size, vk_usage, vk_props, desc->device_address,
                                         &b->buffer, &b->alloc);
    if (r != FLUX_OK)
        goto fail;

    {
        char name[80];
        snprintf(name, sizeof(name), "flux_buffer %llu KiB",
                 (unsigned long long)(desc->size >> 10));
        flux_vk_set_name(d, VK_OBJECT_TYPE_BUFFER, (uint64_t)b->buffer, name);
    }

    if (desc->location == FLUX_BUFFER_HOST_VISIBLE) {
        b->mapped = b->alloc.mapped;
        if (!b->mapped) {
            FLUX_FAIL(FLUX_ERROR_BACKEND_FAILURE,
                      "HOST_VISIBLE buffer came back un-mapped (allocator invariant violated)");
            r = FLUX_ERROR_BACKEND_FAILURE;
            goto fail;
        }
        if (desc->initial_data)
            memcpy(b->mapped, desc->initial_data, desc->size);
    } else if (desc->initial_data) {
        r = flux_vk_upload_to_buffer(d, b->buffer, 0, desc->initial_data, desc->size);
        if (r != FLUX_OK)
            goto fail;
    }

    if (desc->device_address) {
        VkBufferDeviceAddressInfo bdai = {
            .sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO,
            .buffer = b->buffer,
        };
        b->device_address = vkGetBufferDeviceAddress(d->device, &bdai);
    }

    *out = b;
    return FLUX_OK;

fail:
    if (b->buffer)
        vkDestroyBuffer(d->device, b->buffer, nullptr);
    if (b->alloc.memory)
        flux_vk_deallocate(d, &b->alloc);
    flux_device_release(d);
    flux_internal_free(d, b);
    return r;
}

flux_buffer *flux_buffer_retain(flux_buffer *b) {
    if (b)
        atomic_fetch_add_explicit(&b->ref_count, 1u, memory_order_relaxed);
    return b;
}

void flux_buffer_release(flux_buffer *b) {
    if (!b)
        return;
    if (atomic_fetch_sub_explicit(&b->ref_count, 1u, memory_order_acq_rel) != 1u)
        return;
    flux_device *d = b->device;
    /* The buffer may still be bound to batches in flight on the graphics
     * queue (vertex/index/uniform binds recorded before this release).
     * Destroying the VkBuffer or freeing its memory inline can fault the
     * engine mid-batch — the same i915 hazard that motivated the image
     * retire queue. Park the pieces on the device retire queue; they are
     * destroyed once the queue provably passed every batch that could
     * reference them. */
    flux_device_retire_buffer(d, b->buffer, &b->alloc);
    flux_internal_free(d, b);
    flux_device_release(d);
}

void *flux_buffer_mapped(const flux_buffer *b) {
    return b ? b->mapped : nullptr;
}
size_t flux_buffer_size(const flux_buffer *b) {
    return b ? b->size : 0;
}

VkBuffer flux_buffer_vk_buffer(const flux_buffer *b) {
    return b ? b->buffer : VK_NULL_HANDLE;
}

uint64_t flux_buffer_device_address(const flux_buffer *b) {
    return b ? b->device_address : 0u;
}
