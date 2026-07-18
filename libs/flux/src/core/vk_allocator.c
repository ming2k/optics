/*
 * GPU memory allocator.
 *
 * Block-based sub-allocation. Vulkan implementations cap the number
 * of distinct vkAllocateMemory calls a process may make (commonly
 * 4096); a real application with hundreds of textures + meshes hits
 * that cap fast and fragments per-process address space.
 *
 * Design:
 *   - One allocator on the device, lock-protected.
 *   - A flat list of blocks. Each block is a single VkDeviceMemory
 *     and a sorted free-list of (offset, size) ranges.
 *   - Pools are keyed by the triple (memory_type, is_image_pool,
 *     has_dev_addr). Images and buffers never share blocks, and
 *     BDA-capable buffers never share a block with non-BDA buffers,
 *     side-stepping the bufferImageGranularity rule with zero per-
 *     alloc cost.
 *   - Oversize requests (>= FLUX_VK_DEDICATED_THRESH) go to a
 *     standalone VkDeviceMemory and are tagged via block==NULL.
 *   - Pooled blocks set MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT iff the
 *     caller asked for a device address; dedicated allocations set
 *     the flag only when the caller asks.
 *   - Free-list coalescing: on dealloc, the new range is inserted in
 *     sorted order and merged with adjacent ranges in O(n) where n
 *     is the per-block free-list length (typically small).
 *
 * Safety:
 *   - Every pooled dealloc is validated against the owning block's
 *     `allocated_bytes` mirror. A double-free (or a stale copy of
 *     the caller's flux_vk_alloc struct) is logged and rejected
 *     instead of corrupting the free-list.
 *   - `lost_bytes` per block tracks ranges whose free-list reinsert
 *     failed due to host OOM. Such blocks are still reclaimable
 *     once their live allocations all return.
 */
#include "internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/*  Helpers                                                           */
/* ------------------------------------------------------------------ */

static VkDeviceSize align_up(VkDeviceSize v, VkDeviceSize a) {
    return (v + a - 1) & ~(a - 1);
}

static flux_vk_range *range_new(VkDeviceSize offset, VkDeviceSize size) {
    flux_vk_range *r = calloc(1, sizeof(*r));
    if (!r)
        return NULL;
    r->offset = offset;
    r->size = size;
    return r;
}

/* Insert `(offset, size)` into `*head_ptr`, merging with adjacent
 * ranges. Returns true on success, false on OOM. */
static bool range_insert_coalesce(flux_vk_range **head_ptr, VkDeviceSize offset,
                                  VkDeviceSize size) {
    flux_vk_range *prev = NULL;
    flux_vk_range *cur = *head_ptr;
    while (cur && cur->offset < offset) {
        prev = cur;
        cur = cur->next;
    }

    /* Coalesce with predecessor (prev) if it touches the new range. */
    if (prev && prev->offset + prev->size == offset) {
        prev->size += size;
        /* Then coalesce prev with cur if they now touch. */
        if (cur && prev->offset + prev->size == cur->offset) {
            prev->size += cur->size;
            prev->next = cur->next;
            free(cur);
        }
        return true;
    }

    /* Coalesce with successor (cur) if it touches the new range. */
    if (cur && offset + size == cur->offset) {
        cur->offset = offset;
        cur->size += size;
        return true;
    }

    /* No coalesce; allocate a fresh node. */
    flux_vk_range *node = range_new(offset, size);
    if (!node)
        return false;
    node->next = cur;
    if (prev)
        prev->next = node;
    else
        *head_ptr = node;
    return true;
}

/* Try to carve `size` bytes (aligned to `align`) from the block's
 * free-list. Returns true and writes the offset; false if no fit. */
static bool block_try_alloc(flux_vk_block *b, VkDeviceSize size, VkDeviceSize align,
                            VkDeviceSize *out_offset) {
    flux_vk_range *prev = NULL;
    for (flux_vk_range *cur = b->free_list; cur; prev = cur, cur = cur->next) {
        VkDeviceSize aligned = align_up(cur->offset, align);
        VkDeviceSize waste = aligned - cur->offset;
        if (waste > cur->size)
            continue;
        if (cur->size - waste < size)
            continue;

        *out_offset = aligned;

        /* The carved region is [aligned, aligned+size). Split. */
        VkDeviceSize tail_offset = aligned + size;
        VkDeviceSize tail_size = cur->size - waste - size;

        if (waste == 0 && tail_size == 0) {
            /* Whole range consumed. */
            if (prev)
                prev->next = cur->next;
            else
                b->free_list = cur->next;
            free(cur);
        } else if (waste == 0) {
            cur->offset = tail_offset;
            cur->size = tail_size;
        } else if (tail_size == 0) {
            cur->size = waste;
        } else {
            /* Split into [orig_offset, aligned) and [tail_offset, end). */
            flux_vk_range *tail = range_new(tail_offset, tail_size);
            if (!tail) {
                /* OOM: undo by leaving the range as-is. The carve fails. */
                return false;
            }
            cur->size = waste;
            tail->next = cur->next;
            cur->next = tail;
        }
        return true;
    }
    return false;
}

/* ------------------------------------------------------------------ */
/*  Block lifecycle                                                   */
/* ------------------------------------------------------------------ */

/* Map a vkAllocateMemory / vkMapMemory failure to a flux_result. GPU
 * and host OOM both surface as FLUX_ERROR_OUT_OF_MEMORY so callers can
 * react (reclaim, retry, evict) instead of seeing a generic backend
 * fault; anything else stays BACKEND_FAILURE. */
static flux_result result_from_vk_memory_failure(VkResult vr) {
    if (vr == VK_ERROR_OUT_OF_DEVICE_MEMORY || vr == VK_ERROR_OUT_OF_HOST_MEMORY)
        return FLUX_ERROR_OUT_OF_MEMORY;
    return FLUX_ERROR_BACKEND_FAILURE;
}

static flux_vk_block *block_create(flux_device *d, uint32_t memory_type, VkDeviceSize size,
                                   bool is_image_pool, bool has_dev_addr, bool host_visible,
                                   flux_result *err) {
    flux_vk_block *b = calloc(1, sizeof(*b));
    if (!b) {
        *err = FLUX_ERROR_OUT_OF_MEMORY;
        return NULL;
    }
    b->size = size;
    b->memory_type = memory_type;
    b->is_image_pool = is_image_pool;
    b->has_dev_addr = has_dev_addr;

    VkMemoryAllocateFlagsInfo afi = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO,
        .flags = has_dev_addr ? VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT : 0,
    };
    VkMemoryAllocateInfo mai = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .pNext = has_dev_addr ? &afi : NULL,
        .allocationSize = size,
        .memoryTypeIndex = memory_type,
    };
    VkResult vr = vkAllocateMemory(d->device, &mai, NULL, &b->memory);
    if (vr != VK_SUCCESS) {
        *err = result_from_vk_memory_failure(vr);
        FLUX_FAIL_VK(*err, "vkAllocateMemory (pool block) failed", vr);
        free(b);
        return NULL;
    }

    if (host_visible) {
        vr = vkMapMemory(d->device, b->memory, 0, VK_WHOLE_SIZE, 0, &b->mapped);
        if (vr != VK_SUCCESS) {
            *err = result_from_vk_memory_failure(vr);
            FLUX_FAIL_VK(*err, "vkMapMemory (pool block) failed", vr);
            vkFreeMemory(d->device, b->memory, NULL);
            free(b);
            return NULL;
        }
    }

    /* One free range covering the whole block. */
    b->free_list = range_new(0, size);
    if (!b->free_list) {
        *err = FLUX_ERROR_OUT_OF_MEMORY;
        if (b->mapped)
            vkUnmapMemory(d->device, b->memory);
        vkFreeMemory(d->device, b->memory, NULL);
        free(b);
        return NULL;
    }
    char name[80];
    snprintf(name, sizeof(name), "flux pool block #%u (%s mt=%u %llu MiB)",
             d->mem_allocator.block_seq++, is_image_pool ? "image" : "buffer", memory_type,
             (unsigned long long)(size >> 20));
    flux_vk_set_name(d, VK_OBJECT_TYPE_DEVICE_MEMORY, (uint64_t)b->memory, name);
    return b;
}

static void block_destroy(flux_device *d, flux_vk_block *b) {
    if (!b)
        return;
    for (flux_vk_range *r = b->free_list; r;) {
        flux_vk_range *next = r->next;
        free(r);
        r = next;
    }
    if (b->memory) {
        if (b->mapped)
            vkUnmapMemory(d->device, b->memory);
        vkFreeMemory(d->device, b->memory, NULL);
    }
    free(b);
}

/* ------------------------------------------------------------------ */
/*  Allocator lifecycle                                               */
/* ------------------------------------------------------------------ */

flux_result flux_vk_allocator_init(flux_device *d) {
    flux_vk_allocator *a = &d->mem_allocator;
    memset(a, 0, sizeof(*a));
    if (pthread_mutex_init(&a->lock, NULL) != 0) {
        FLUX_FAIL(FLUX_ERROR_BACKEND_FAILURE, "allocator mutex init failed");
        return FLUX_ERROR_BACKEND_FAILURE;
    }
    a->lock_initialized = true;

    if (d->physical_device) {
        uint32_t ext_count = 0;
        VkResult vr =
            vkEnumerateDeviceExtensionProperties(d->physical_device, NULL, &ext_count, NULL);
        if (vr == VK_SUCCESS && ext_count > 0) {
            VkExtensionProperties *exts = calloc(ext_count, sizeof(*exts));
            if (exts) {
                vr = vkEnumerateDeviceExtensionProperties(d->physical_device, NULL, &ext_count,
                                                          exts);
                if (vr == VK_SUCCESS) {
                    for (uint32_t i = 0; i < ext_count; ++i) {
                        if (strcmp(exts[i].extensionName, VK_EXT_MEMORY_BUDGET_EXTENSION_NAME) ==
                            0) {
                            a->has_memory_budget = true;
                            break;
                        }
                    }
                }
                free(exts);
            }
        }
    }
    return FLUX_OK;
}

void flux_vk_allocator_destroy(flux_device *d) {
    flux_vk_allocator *a = &d->mem_allocator;
    if (!a->lock_initialized)
        return;

    /* Diagnostic: if there are live allocations at device teardown,
     * we're about to leak GPU memory or free still-bound resources.
     * Log and proceed; the per-block free will recover the memory. */
    if (a->live_allocations > 0 && d->log) {
        char buf[256];
        snprintf(buf, sizeof(buf),
                 "flux_vk_allocator: %u live allocations at teardown "
                 "(possible resource leak — bytes_in_use=%llu)",
                 a->live_allocations, (unsigned long long)a->bytes_in_use);
        d->log(FLUX_LOG_WARN, "flux", 0, "%s", buf, d->log_user);
    }

    for (flux_vk_block *b = a->blocks; b;) {
        flux_vk_block *next = b->next;
        block_destroy(d, b);
        b = next;
    }
    a->blocks = NULL;
    pthread_mutex_destroy(&a->lock);
    a->lock_initialized = false;
}

/* ------------------------------------------------------------------ */
/*  Public alloc / dealloc                                            */
/* ------------------------------------------------------------------ */

/* Pick a memory type satisfying mr.memoryTypeBits and the requested
 * property flags. When multiple types match, prefer the one whose
 * property flags are *exactly* the requested set (no extra bits like
 * HOST_VISIBLE on a DEVICE_LOCAL request, which on UMA devices would
 * silently back device-local resources with system memory). Falls
 * back to the first superset match. UINT32_MAX on failure. */
static uint32_t pick_memory_type(flux_device *d, uint32_t type_filter,
                                 VkMemoryPropertyFlags wanted) {
    uint32_t fallback = UINT32_MAX;
    for (uint32_t i = 0; i < d->mem_props.memoryTypeCount; ++i) {
        if (!(type_filter & (1u << i)))
            continue;
        VkMemoryPropertyFlags have = d->mem_props.memoryTypes[i].propertyFlags;
        if ((have & wanted) != wanted)
            continue;
        if (have == wanted)
            return i; /* exact match preferred */
        if (fallback == UINT32_MAX)
            fallback = i;
    }
    return fallback;
}

static flux_result do_dedicated(flux_device *d, VkDeviceSize size, uint32_t memory_type,
                                bool wants_device_address, bool host_visible,
                                const flux_vk_dedication *ded, flux_vk_alloc *out) {
    /* Chain order: [VkMemoryDedicatedAllocateInfo] -> [FlagsInfo] ->
     * MemoryAllocateInfo. The dedicated-info struct is only chained when
     * it names a real resource — both handles null is a VUID violation. */
    VkMemoryDedicatedAllocateInfo dai = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
        .buffer = ded ? ded->buffer : VK_NULL_HANDLE,
        .image = ded ? ded->image : VK_NULL_HANDLE,
    };
    VkMemoryAllocateFlagsInfo afi = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO,
        .flags = wants_device_address ? VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT : 0,
    };
    const void *chain = NULL;
    if (dai.buffer != VK_NULL_HANDLE || dai.image != VK_NULL_HANDLE)
        chain = &dai;
    if (wants_device_address) {
        afi.pNext = chain;
        chain = &afi;
    }
    VkMemoryAllocateInfo mai = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .pNext = chain,
        .allocationSize = size,
        .memoryTypeIndex = memory_type,
    };
    VkDeviceMemory mem = VK_NULL_HANDLE;
    VkResult vr = vkAllocateMemory(d->device, &mai, NULL, &mem);
    if (vr != VK_SUCCESS) {
        flux_result err = result_from_vk_memory_failure(vr);
        FLUX_FAIL_VK(err, "vkAllocateMemory (dedicated) failed", vr);
        return err;
    }
    void *mapped = NULL;
    if (host_visible) {
        vr = vkMapMemory(d->device, mem, 0, VK_WHOLE_SIZE, 0, &mapped);
        if (vr != VK_SUCCESS) {
            flux_result err = result_from_vk_memory_failure(vr);
            FLUX_FAIL_VK(err, "vkMapMemory (dedicated) failed", vr);
            vkFreeMemory(d->device, mem, NULL);
            return err;
        }
    }
    out->memory = mem;
    out->offset = 0;
    out->size = size;
    out->mapped = mapped;
    out->block = NULL;
    char name[80];
    snprintf(name, sizeof(name), "flux dedicated %llu KiB (mt=%u)",
             (unsigned long long)(size >> 10), memory_type);
    flux_vk_set_name(d, VK_OBJECT_TYPE_DEVICE_MEMORY, (uint64_t)mem, name);
    return FLUX_OK;
}

/* Best-effort heap usage/budget query for `heap_index`, via
 * VK_EXT_memory_budget. Returns false when the extension is absent so
 * callers can skip budget-aware behaviour entirely. */
static bool query_heap_budget(const flux_device *d, uint32_t heap_index, VkDeviceSize *usage,
                              VkDeviceSize *budget) {
    if (!d->mem_allocator.has_memory_budget || heap_index >= VK_MAX_MEMORY_HEAPS)
        return false;
    VkPhysicalDeviceMemoryBudgetPropertiesEXT bp = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT,
    };
    VkPhysicalDeviceMemoryProperties2 mp2 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2,
        .pNext = &bp,
    };
    vkGetPhysicalDeviceMemoryProperties2(d->physical_device, &mp2);
    *usage = bp.heapUsage[heap_index];
    *budget = bp.heapBudget[heap_index];
    return true;
}

/* Destroy every empty block, returning its memory to the driver.
 * a->lock must be held. Defined after block_is_reclaimable below. */
static uint32_t reclaim_locked(flux_device *d);

flux_result flux_vk_allocate(flux_device *d, VkMemoryRequirements mr,
                             VkMemoryPropertyFlags wanted_flags, bool is_image,
                             bool wants_device_address, const flux_vk_dedication *dedication,
                             flux_vk_alloc *out) {
    if (!d || !out)
        return FLUX_ERROR_INVALID_ARGUMENT;
    *out = (flux_vk_alloc){0};

    uint32_t mt = pick_memory_type(d, mr.memoryTypeBits, wanted_flags);
    if (mt == UINT32_MAX) {
        FLUX_FAIL(FLUX_ERROR_UNSUPPORTED, "no memory type satisfies requested properties");
        return FLUX_ERROR_UNSUPPORTED;
    }
    bool host_visible = (wanted_flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0;

    /* Alignment is at least mr.alignment; non-coherent host visible
     * memory must also respect nonCoherentAtomSize, but we use only
     * HOST_COHERENT for now. */
    VkDeviceSize align = mr.alignment > 0 ? mr.alignment : 1;
    VkDeviceSize size = align_up(mr.size, align);

    flux_vk_allocator *a = &d->mem_allocator;
    pthread_mutex_lock(&a->lock);

    /* Dedicated path: oversize requests; any resource the driver marks
     * requiresDedicated (mandatory — pooling would be a spec violation);
     * or prefersDedicated above the small-size floor (driver knows best
     * for big resources, but small ones stay pooled to protect the
     * vkAllocateMemory count). */
    bool force_dedicated =
        dedication && (dedication->required ||
                       (dedication->preferred && size >= FLUX_VK_PREFER_DEDICATED_MIN));
    if (force_dedicated || size >= FLUX_VK_DEDICATED_THRESH) {
        flux_result r =
            do_dedicated(d, size, mt, wants_device_address, host_visible, dedication, out);
        if (r == FLUX_ERROR_OUT_OF_MEMORY && reclaim_locked(d) > 0) {
            /* Empty pool blocks may be sitting on the memory this
             * request needs: hand them back to the driver and retry
             * once before declaring OOM. */
            r = do_dedicated(d, size, mt, wants_device_address, host_visible, dedication, out);
        }
        if (r == FLUX_OK) {
            a->bytes_in_use += size;
            a->bytes_reserved += size;
            a->live_allocations++;
        }
        pthread_mutex_unlock(&a->lock);
        return r;
    }

    /* Find or create a block matching (memory_type, is_image_pool, dev_addr). */
    flux_vk_block *target = NULL;
    VkDeviceSize chosen_offset = 0;
    for (flux_vk_block *b = a->blocks; b; b = b->next) {
        if (b->memory_type != mt)
            continue;
        if (b->is_image_pool != is_image)
            continue;
        /* A block with dev_addr can host non-dev-addr buffers and
         * vice-versa is allowed only when the buffer doesn't ask for
         * it. Simplify: keep them in the same pool only if both want
         * the same thing. (Pooled blocks default to has_dev_addr=true
         * for buffer pools, false for image pools.) */
        if (b->has_dev_addr != wants_device_address)
            continue;
        if (block_try_alloc(b, size, align, &chosen_offset)) {
            target = b;
            break;
        }
    }

    if (!target) {
        VkDeviceSize block_size = size > FLUX_VK_BLOCK_SIZE ? size : FLUX_VK_BLOCK_SIZE;
        /* If the memory type is host-visible at all, map the entire
         * block upfront — `vkMapMemory` can only be called once per
         * VkDeviceMemory, so we centralise the mapping here and avoid
         * a race where two upload threads both try to map the same
         * unmapped block. */
        bool block_is_host_visible =
            (d->mem_props.memoryTypes[mt].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0;
        /* Budget gate: when a fresh block would push the heap past its
         * driver-advertised budget, hand empty blocks back to the
         * driver before asking for more. The definitive failure still
         * comes from vkAllocateMemory if the driver cannot satisfy the
         * request — the budget is a hint, not a hard limit. */
        bool reclaimed = false;
        VkDeviceSize usage = 0, budget = 0;
        uint32_t heap = d->mem_props.memoryTypes[mt].heapIndex;
        if (query_heap_budget(d, heap, &usage, &budget) &&
            (usage >= budget || block_size > budget - usage)) {
            reclaim_locked(d);
            reclaimed = true;
        }
        flux_result err = FLUX_ERROR_OUT_OF_MEMORY;
        target = block_create(d, mt, block_size, is_image, wants_device_address,
                              block_is_host_visible, &err);
        if (!target && !reclaimed && reclaim_locked(d) > 0) {
            /* The failure may be nothing more than empty blocks of a
             * different key holding memory: reclaim and retry once. */
            target = block_create(d, mt, block_size, is_image, wants_device_address,
                                  block_is_host_visible, &err);
        }
        if (!target) {
            pthread_mutex_unlock(&a->lock);
            return err;
        }
        target->next = a->blocks;
        a->blocks = target;
        a->bytes_reserved += block_size;
        a->live_blocks++;
        if (!block_try_alloc(target, size, align, &chosen_offset)) {
            /* Shouldn't happen on a fresh block. */
            pthread_mutex_unlock(&a->lock);
            FLUX_FAIL(FLUX_ERROR_BACKEND_FAILURE,
                      "allocator: fresh block could not satisfy request");
            return FLUX_ERROR_BACKEND_FAILURE;
        }
    }

    out->memory = target->memory;
    out->offset = chosen_offset;
    out->size = size;
    out->mapped = target->mapped ? (uint8_t *)target->mapped + chosen_offset : NULL;
    out->block = target;
    target->allocated_bytes += size;
    target->live_allocations++;
    a->bytes_in_use += size;
    a->live_allocations++;
    pthread_mutex_unlock(&a->lock);
    return FLUX_OK;
}

/* A block is reclaimable when nothing is allocated out of it. Lost
 * ranges (free-list node OOM) are *freed* bytes, so they don't block
 * reclaim — they only mean the block can't serve new sub-allocations
 * until it's destroyed and re-created. */
static bool block_is_reclaimable(flux_vk_block *b) {
    return b->live_allocations == 0 && b->allocated_bytes == 0;
}

void flux_vk_deallocate(flux_device *d, flux_vk_alloc *alloc) {
    if (!d || !alloc || !alloc->memory)
        return;
    flux_vk_allocator *a = &d->mem_allocator;
    pthread_mutex_lock(&a->lock);

    flux_vk_block *freed_block = NULL;

    if (alloc->block == NULL) {
        /* Dedicated: free the standalone VkDeviceMemory entirely.
         * Double-free protection comes from the global live_allocations
         * counter — a second dealloc of the same handle would have
         * nothing left to subtract and is rejected. */
        if (a->live_allocations == 0 || alloc->size > a->bytes_in_use) {
            if (d->log) {
                d->log(FLUX_LOG_ERROR, "flux", 0, "%s",
                       "flux_vk_deallocate: rejected double-free of dedicated "
                       "allocation",
                       d->log_user);
            }
            pthread_mutex_unlock(&a->lock);
            return;
        }
        if (alloc->mapped)
            vkUnmapMemory(d->device, alloc->memory);
        vkFreeMemory(d->device, alloc->memory, NULL);
        a->bytes_in_use -= alloc->size;
        a->bytes_reserved -= alloc->size;
        a->live_allocations--;
    } else {
        flux_vk_block *b = alloc->block;

        /* Double-free / stale-copy detector. The free-list is the
         * source of truth for what's currently unallocated; the
         * allocated_bytes mirror is the source of truth for what's
         * currently live. If the caller hands us a range that the
         * block doesn't believe it holds, refuse and log instead of
         * inserting a duplicate range and corrupting future allocs. */
        if (alloc->size == 0 || alloc->size > b->allocated_bytes || b->live_allocations == 0) {
            if (d->log) {
                char buf[256];
                snprintf(buf, sizeof(buf),
                         "flux_vk_deallocate: rejected double-free or stale "
                         "alloc (offset=%llu size=%llu block_live=%u)",
                         (unsigned long long)alloc->offset, (unsigned long long)alloc->size,
                         b->live_allocations);
                d->log(FLUX_LOG_ERROR, "flux", 0, "%s", buf, d->log_user);
            }
            pthread_mutex_unlock(&a->lock);
            return;
        }

        bool ok = range_insert_coalesce(&b->free_list, alloc->offset, alloc->size);
        if (!ok) {
            /* Host OOM on the free-list node. The range can't be
             * reused, but the block is still reclaimable once its
             * other live allocations return. Track lost bytes per
             * block so reclaim sees the block as empty. */
            b->lost_bytes += alloc->size;
            a->lost_ranges_bytes += alloc->size;
            if (d->log) {
                d->log(FLUX_LOG_WARN, "flux", 0, "%s",
                       "flux_vk_deallocate: free-list node alloc failed; "
                       "range lost (will be recovered at block reclaim)",
                       d->log_user);
            }
        }
        b->allocated_bytes -= alloc->size;
        b->live_allocations--;
        a->bytes_in_use -= alloc->size;
        a->live_allocations--;
        if (block_is_reclaimable(b))
            freed_block = b;
    }

    *alloc = (flux_vk_alloc){0};

    /* Auto-reclaim: if the block that just freed an allocation is now
     * empty, and there are other blocks of the same key, reclaim it.
     * This prevents empty 64 MiB blocks from accumulating indefinitely. */
    if (freed_block) {
        bool has_peer = false;
        for (flux_vk_block *other = a->blocks; other; other = other->next) {
            if (other != freed_block && other->memory_type == freed_block->memory_type &&
                other->is_image_pool == freed_block->is_image_pool &&
                other->has_dev_addr == freed_block->has_dev_addr) {
                has_peer = true;
                break;
            }
        }
        if (has_peer) {
            flux_vk_block **pp = &a->blocks;
            while (*pp && *pp != freed_block)
                pp = &(*pp)->next;
            if (*pp == freed_block) {
                *pp = freed_block->next;
                a->bytes_reserved -= freed_block->size;
                a->lost_ranges_bytes -= freed_block->lost_bytes;
                a->live_blocks--;
                block_destroy(d, freed_block);
            }
        }
    }

    pthread_mutex_unlock(&a->lock);
}

static uint32_t reclaim_locked(flux_device *d) {
    flux_vk_allocator *a = &d->mem_allocator;
    uint32_t reclaimed = 0;
    flux_vk_block **pp = &a->blocks;
    while (*pp) {
        flux_vk_block *b = *pp;
        if (block_is_reclaimable(b)) {
            *pp = b->next;
            a->bytes_reserved -= b->size;
            a->lost_ranges_bytes -= b->lost_bytes;
            a->live_blocks--;
            block_destroy(d, b);
            reclaimed++;
        } else {
            pp = &b->next;
        }
    }
    return reclaimed;
}

uint32_t flux_vk_allocator_reclaim(flux_device *d) {
    if (!d)
        return 0;
    flux_vk_allocator *a = &d->mem_allocator;
    pthread_mutex_lock(&a->lock);
    uint32_t reclaimed = reclaim_locked(d);
    pthread_mutex_unlock(&a->lock);
    return reclaimed;
}

void flux_vk_allocator_note_external(flux_device *d, VkDeviceSize bytes) {
    flux_vk_allocator *a = &d->mem_allocator;
    pthread_mutex_lock(&a->lock);
    a->bytes_in_use += bytes;
    a->bytes_reserved += bytes;
    a->live_allocations++;
    pthread_mutex_unlock(&a->lock);
}

void flux_vk_allocator_unnote_external(flux_device *d, VkDeviceSize bytes) {
    flux_vk_allocator *a = &d->mem_allocator;
    pthread_mutex_lock(&a->lock);
    if (a->live_allocations == 0 || bytes > a->bytes_in_use) {
        if (d->log) {
            d->log(FLUX_LOG_ERROR, "flux", 0, "%s",
                   "flux_vk_allocator_unnote_external: rejected (would underflow)", d->log_user);
        }
        pthread_mutex_unlock(&a->lock);
        return;
    }
    a->bytes_in_use -= bytes;
    a->bytes_reserved -= bytes;
    a->live_allocations--;
    pthread_mutex_unlock(&a->lock);
}

void flux_device_memory_stats(flux_device *d, flux_memory_stats *out) {
    if (!out)
        return;
    memset(out, 0, sizeof(*out));
    if (!d)
        return;
    /* Settle deferred uploads before sampling: their staging buffers
     * stay checked out (and counted) until the copy's fence signals and
     * the pending entry is recycled, so without a drain the numbers
     * depend on GPU timing rather than on what the caller did. This is
     * a diagnostics path; the wait is off every hot loop. */
    flux_vk_upload_pending_drain(d);
    flux_vk_allocator *a = (flux_vk_allocator *)&d->mem_allocator;
    pthread_mutex_lock(&a->lock);
    out->bytes_in_use = a->bytes_in_use;
    out->bytes_reserved = a->bytes_reserved;
    out->lost_ranges_bytes = a->lost_ranges_bytes;
    out->live_allocations = a->live_allocations;
    out->live_blocks = a->live_blocks;
    pthread_mutex_unlock(&a->lock);
}
