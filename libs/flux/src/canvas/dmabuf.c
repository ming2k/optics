/*
 * flux/dmabuf.c - import a Linux dma-buf as a sampled flux_image.
 *
 * Uses VK_EXT_image_drm_format_modifier to create a VkImage whose memory
 * layout matches the producer's DRM modifier, VK_KHR_external_memory_fd to
 * import the plane's file descriptor, and a dedicated allocation bound to it.
 * An optional Linux sync_file acquire fence can be imported as an external
 * semaphore and waited by the foreign-queue acquire. The image is then
 * registered in the bindless heap, so the canvas samples it like any other
 * flux_image.
 *
 * Scope: single-plane formats with an explicit DRM modifier. Synchronisation
 * with the producer remains a caller contract in this revision.
 */

#include "internal.h"
#include <flux/dmabuf.h>

#include <errno.h>
#include <stdatomic.h>
#include <unistd.h>

static uint32_t dmabuf_min_bytes_per_pixel(flux_format f) {
    switch (f) {
    case FLUX_FORMAT_R8_UNORM:
        return 1;
    case FLUX_FORMAT_RGBA8_UNORM:
    case FLUX_FORMAT_BGRA8_UNORM:
    case FLUX_FORMAT_RGBA8_SRGB:
    case FLUX_FORMAT_BGRA8_SRGB:
        return 4;
    case FLUX_FORMAT_RGBA16_SFLOAT:
        return 8;
    case FLUX_FORMAT_R32_SFLOAT:
        return 4;
    case FLUX_FORMAT_RG32_SFLOAT:
        return 8;
    case FLUX_FORMAT_RGB32_SFLOAT:
        return 12;
    case FLUX_FORMAT_RGBA32_SFLOAT:
        return 16;
    default:
        return 0;
    }
}

static bool format_modifier_supports_sampling(flux_device *d, VkFormat format, uint64_t modifier,
                                              uint32_t plane_count) {
    VkDrmFormatModifierPropertiesListEXT list = {
        .sType = VK_STRUCTURE_TYPE_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_EXT,
    };
    VkFormatProperties2 props = {
        .sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2,
        .pNext = &list,
    };
    vkGetPhysicalDeviceFormatProperties2(d->physical_device, format, &props);
    if (list.drmFormatModifierCount == 0)
        return false;

    VkDrmFormatModifierPropertiesEXT *mods =
        flux_internal_alloc(d, sizeof(*mods) * list.drmFormatModifierCount);
    if (!mods)
        return false;

    list.pDrmFormatModifierProperties = mods;
    vkGetPhysicalDeviceFormatProperties2(d->physical_device, format, &props);

    bool supported = false;
    for (uint32_t i = 0; i < list.drmFormatModifierCount; ++i) {
        const VkDrmFormatModifierPropertiesEXT *m = &mods[i];
        if (m->drmFormatModifier != modifier)
            continue;
        if (m->drmFormatModifierPlaneCount != plane_count)
            continue;
        if ((m->drmFormatModifierTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) == 0) {
            continue;
        }
        supported = true;
        break;
    }

    flux_internal_free(d, mods);
    return supported;
}

static bool external_image_importable(flux_device *d, VkFormat format, uint64_t modifier,
                                      uint32_t width, uint32_t height) {
    VkPhysicalDeviceExternalImageFormatInfo external_info = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO,
        .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
    };
    VkPhysicalDeviceImageDrmFormatModifierInfoEXT modifier_info = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_DRM_FORMAT_MODIFIER_INFO_EXT,
        .pNext = &external_info,
        .drmFormatModifier = modifier,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .queueFamilyIndexCount = 0,
        .pQueueFamilyIndices = nullptr,
    };
    VkPhysicalDeviceImageFormatInfo2 info = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2,
        .pNext = &modifier_info,
        .format = format,
        .type = VK_IMAGE_TYPE_2D,
        .tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT,
        .usage = VK_IMAGE_USAGE_SAMPLED_BIT,
        .flags = 0,
    };
    VkExternalImageFormatProperties external_props = {
        .sType = VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES,
    };
    VkImageFormatProperties2 props = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2,
        .pNext = &external_props,
    };
    VkResult vr = vkGetPhysicalDeviceImageFormatProperties2(d->physical_device, &info, &props);
    if (vr != VK_SUCCESS)
        return false;
    if ((external_props.externalMemoryProperties.externalMemoryFeatures &
         VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT) == 0) {
        return false;
    }
    return width <= props.imageFormatProperties.maxExtent.width &&
           height <= props.imageFormatProperties.maxExtent.height;
}

static flux_result import_sync_fd_semaphore(flux_device *d, int acquire_sync_fd, VkSemaphore *out) {
    *out = VK_NULL_HANDLE;
    if (!d->has_external_semaphore_fd) {
        FLUX_FAIL(FLUX_ERROR_UNSUPPORTED, "acquire_sync_fd requires VK_KHR_external_semaphore_fd");
        return FLUX_ERROR_UNSUPPORTED;
    }

    int import_fd = dup(acquire_sync_fd);
    if (import_fd < 0) {
        flux_result dr =
            (errno == EBADF) ? FLUX_ERROR_INVALID_ARGUMENT : FLUX_ERROR_BACKEND_FAILURE;
        FLUX_FAIL(dr, "failed to duplicate acquire_sync_fd for Vulkan import");
        return dr;
    }
    bool fd_transferred = false;

    PFN_vkImportSemaphoreFdKHR pImportSemaphoreFd =
        (PFN_vkImportSemaphoreFdKHR)vkGetDeviceProcAddr(d->device, "vkImportSemaphoreFdKHR");
    if (!pImportSemaphoreFd) {
        close(import_fd);
        FLUX_FAIL(FLUX_ERROR_UNSUPPORTED, "VK_KHR_external_semaphore_fd entry point missing");
        return FLUX_ERROR_UNSUPPORTED;
    }

    VkExportSemaphoreCreateInfo export_info = {
        .sType = VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO,
        .handleTypes = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT,
    };
    VkSemaphoreCreateInfo sci = {
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
        .pNext = &export_info,
    };
    VkResult vr = vkCreateSemaphore(d->device, &sci, nullptr, out);
    if (vr != VK_SUCCESS) {
        close(import_fd);
        FLUX_FAIL_VK(FLUX_ERROR_BACKEND_FAILURE, "dma-buf acquire semaphore create failed", vr);
        return FLUX_ERROR_BACKEND_FAILURE;
    }

    VkImportSemaphoreFdInfoKHR import_info = {
        .sType = VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_FD_INFO_KHR,
        .semaphore = *out,
        .flags = VK_SEMAPHORE_IMPORT_TEMPORARY_BIT,
        .handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT,
        .fd = import_fd,
    };
    vr = pImportSemaphoreFd(d->device, &import_info);
    if (vr == VK_SUCCESS)
        fd_transferred = true;
    if (vr != VK_SUCCESS) {
        if (!fd_transferred)
            close(import_fd);
        vkDestroySemaphore(d->device, *out, nullptr);
        *out = VK_NULL_HANDLE;
        FLUX_FAIL_VK(FLUX_ERROR_BACKEND_FAILURE, "dma-buf acquire semaphore import failed", vr);
        return FLUX_ERROR_BACKEND_FAILURE;
    }

    return FLUX_OK;
}

/* The FOREIGN -> graphics-family acquire transition is submitted
 * deferred: the batch waits on acquire_sem GPU-side, so the calling
 * thread never blocks on the producer's fence, and later same-queue
 * work (the frame that samples this image) is ordered after the
 * transition by queue submission order alone. On success the parked
 * pending entry owns both the command pool and acquire_sem, recycled
 * once the batch's fence signals. */
static flux_result dmabuf_transition_image_layout(flux_device *d, VkImage img,
                                                  VkSemaphore acquire_sem) {
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkResult vr = flux_vk_new_transient_cmd(d, d->graphics_family, &pool, &cmd);
    if (vr != VK_SUCCESS) {
        FLUX_FAIL_VK(FLUX_ERROR_BACKEND_FAILURE, "dma-buf transition begin failed", vr);
        goto fail;
    }

    VkImageMemoryBarrier2 b = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .srcStageMask = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT,
        .srcAccessMask = 0,
        .dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
        .dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
        .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_FOREIGN_EXT,
        .dstQueueFamilyIndex = d->graphics_family,
        .image = img,
        .subresourceRange =
            {
                .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                .levelCount = 1,
                .layerCount = 1,
            },
    };
    VkDependencyInfo di = {
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .imageMemoryBarrierCount = 1,
        .pImageMemoryBarriers = &b,
    };
    vkCmdPipelineBarrier2(cmd, &di);

    vr = vkEndCommandBuffer(cmd);
    if (vr != VK_SUCCESS) {
        FLUX_FAIL_VK(FLUX_ERROR_BACKEND_FAILURE, "dma-buf transition end failed", vr);
        goto fail;
    }

    {
        VkFence fence = VK_NULL_HANDLE;
        uint64_t serial = 0;
        vr = flux_vk_submit_upload(d, d->graphics_queue, cmd, acquire_sem,
                                   VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_NULL_HANDLE, 0, &fence,
                                   &serial);
        if (vr == VK_SUCCESS) {
            /* Ownership of the command pool and acquire_sem moves to the
             * pending entry; both are recycled when the batch retires. */
            flux_vk_upload_pending_park(d, fence, pool, VK_NULL_HANDLE, acquire_sem, NULL, serial);
            return FLUX_OK;
        }
    }

fail:
    if (pool)
        vkDestroyCommandPool(d->device, pool, nullptr);
    flux_result r = vr == VK_TIMEOUT             ? FLUX_ERROR_TIMEOUT
                    : vr == VK_ERROR_DEVICE_LOST ? FLUX_ERROR_DEVICE_LOST
                                                 : FLUX_ERROR_BACKEND_FAILURE;
    FLUX_FAIL_VK(r, "dma-buf transition submit failed", vr);
    return r;
}

bool flux_dmabuf_supported(const flux_device *d) {
    if (!d)
        return false;
    return d->has_external_memory_fd && d->has_external_memory_dma_buf &&
           d->has_image_drm_format_modifier && d->has_queue_family_foreign;
}

bool flux_dmabuf_sync_supported(const flux_device *d) {
    return d ? d->has_external_semaphore_fd : false;
}

flux_result flux_image_import_dmabuf(flux_device *d, const flux_dmabuf_image_desc *desc,
                                     flux_image **out) {
    if (!d || !desc || !out)
        return FLUX_ERROR_INVALID_ARGUMENT;
    *out = nullptr;
    if (desc->type != FLUX_TYPE_DMABUF_IMAGE_DESC) {
        FLUX_FAIL(FLUX_ERROR_INVALID_ARGUMENT, "desc->type != FLUX_TYPE_DMABUF_IMAGE_DESC");
        return FLUX_ERROR_INVALID_ARGUMENT;
    }
    if (desc->width == 0 || desc->height == 0) {
        FLUX_FAIL(FLUX_ERROR_INVALID_ARGUMENT, "dmabuf image zero-extent");
        return FLUX_ERROR_INVALID_ARGUMENT;
    }
    if (desc->plane_count != 1) {
        FLUX_FAIL(FLUX_ERROR_UNSUPPORTED, "only single-plane dma-buf import supported");
        return FLUX_ERROR_UNSUPPORTED;
    }
    if (desc->has_acquire_sync_fd && desc->acquire_sync_fd < 0) {
        FLUX_FAIL(FLUX_ERROR_INVALID_ARGUMENT, "dma-buf acquire_sync_fd is invalid");
        return FLUX_ERROR_INVALID_ARGUMENT;
    }
    const flux_dmabuf_plane *plane = &desc->planes[0];
    if (plane->fd < 0) {
        FLUX_FAIL(FLUX_ERROR_INVALID_ARGUMENT, "dma-buf plane fd is invalid");
        return FLUX_ERROR_INVALID_ARGUMENT;
    }
    if (plane->stride == 0) {
        FLUX_FAIL(FLUX_ERROR_INVALID_ARGUMENT, "dma-buf plane stride is zero");
        return FLUX_ERROR_INVALID_ARGUMENT;
    }
    if (!flux_dmabuf_supported(d)) {
        FLUX_FAIL(FLUX_ERROR_UNSUPPORTED,
                  "device missing dma-buf import extensions (create with "
                  "VK_KHR_external_memory_fd + VK_EXT_external_memory_dma_buf + "
                  "VK_EXT_image_drm_format_modifier + VK_EXT_queue_family_foreign)");
        return FLUX_ERROR_UNSUPPORTED;
    }
    VkFormat vfmt = flux_format_to_vk(desc->format);
    if (vfmt == VK_FORMAT_UNDEFINED) {
        FLUX_FAIL(FLUX_ERROR_UNSUPPORTED, "unsupported dma-buf pixel format");
        return FLUX_ERROR_UNSUPPORTED;
    }
    uint32_t min_bpp = dmabuf_min_bytes_per_pixel(desc->format);
    if (min_bpp == 0) {
        FLUX_FAIL(FLUX_ERROR_UNSUPPORTED, "unsupported dma-buf sampled format");
        return FLUX_ERROR_UNSUPPORTED;
    }
    if (plane->stride / min_bpp < desc->width) {
        FLUX_FAIL(FLUX_ERROR_INVALID_ARGUMENT, "dma-buf stride is smaller than width * pixel size");
        return FLUX_ERROR_INVALID_ARGUMENT;
    }
    if (!format_modifier_supports_sampling(d, vfmt, desc->modifier, desc->plane_count)) {
        FLUX_FAIL(FLUX_ERROR_UNSUPPORTED,
                  "dma-buf format/modifier is not sampleable on this device");
        return FLUX_ERROR_UNSUPPORTED;
    }
    if (!external_image_importable(d, vfmt, desc->modifier, desc->width, desc->height)) {
        FLUX_FAIL(FLUX_ERROR_UNSUPPORTED, "dma-buf format/modifier is not externally importable");
        return FLUX_ERROR_UNSUPPORTED;
    }

    flux_result r = FLUX_OK;
    VkSemaphore acquire_sem = VK_NULL_HANDLE;
    if (desc->has_acquire_sync_fd) {
        r = import_sync_fd_semaphore(d, desc->acquire_sync_fd, &acquire_sem);
        if (r != FLUX_OK)
            return r;
    }

    int import_fd = dup(plane->fd);
    if (import_fd < 0) {
        flux_result dr =
            (errno == EBADF) ? FLUX_ERROR_INVALID_ARGUMENT : FLUX_ERROR_BACKEND_FAILURE;
        FLUX_FAIL(dr, "failed to duplicate dma-buf fd for Vulkan import");
        if (acquire_sem)
            vkDestroySemaphore(d->device, acquire_sem, nullptr);
        return dr;
    }
    bool import_fd_transferred = false;

    PFN_vkGetMemoryFdPropertiesKHR pGetMemoryFdProperties =
        (PFN_vkGetMemoryFdPropertiesKHR)vkGetDeviceProcAddr(d->device,
                                                            "vkGetMemoryFdPropertiesKHR");
    if (!pGetMemoryFdProperties) {
        close(import_fd);
        if (acquire_sem)
            vkDestroySemaphore(d->device, acquire_sem, nullptr);
        FLUX_FAIL(FLUX_ERROR_UNSUPPORTED, "VK_KHR_external_memory_fd entry point missing");
        return FLUX_ERROR_UNSUPPORTED;
    }

    flux_image *im = flux_internal_alloc(d, sizeof(*im));
    if (!im) {
        close(import_fd);
        if (acquire_sem)
            vkDestroySemaphore(d->device, acquire_sem, nullptr);
        return FLUX_ERROR_OUT_OF_MEMORY;
    }
    atomic_init(&im->ref_count, 1u);
    im->device = flux_device_retain(d);
    im->width = desc->width;
    im->height = desc->height;
    im->format = desc->format;
    im->bindless = FLUX_BINDLESS_INVALID;
    im->bindless_storage = FLUX_BINDLESS_INVALID;
    im->current_layout = VK_IMAGE_LAYOUT_UNDEFINED;
    im->imported_memory = VK_NULL_HANDLE;

    VkResult vr;

    /* Image with an explicit DRM modifier and external-memory provenance. */
    VkSubresourceLayout plane_layout = {
        .offset = plane->offset,
        .rowPitch = plane->stride,
    };
    VkImageDrmFormatModifierExplicitCreateInfoEXT mod_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT,
        .drmFormatModifier = desc->modifier,
        .drmFormatModifierPlaneCount = 1,
        .pPlaneLayouts = &plane_layout,
    };
    VkExternalMemoryImageCreateInfo ext_info = {
        .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
        .pNext = &mod_info,
        .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
    };
    VkImageCreateInfo ici = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .pNext = &ext_info,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = vfmt,
        .extent = {desc->width, desc->height, 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT,
        .usage = VK_IMAGE_USAGE_SAMPLED_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    vr = vkCreateImage(d->device, &ici, nullptr, &im->image);
    if (vr != VK_SUCCESS) {
        FLUX_FAIL_VK(FLUX_ERROR_BACKEND_FAILURE, "dma-buf vkCreateImage failed", vr);
        r = FLUX_ERROR_BACKEND_FAILURE;
        goto fail;
    }

    /* Memory type allowed for this fd, intersected with the image's needs. */
    VkMemoryFdPropertiesKHR fd_props = {.sType = VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR};
    vr = pGetMemoryFdProperties(d->device, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
                                import_fd, &fd_props);
    if (vr != VK_SUCCESS) {
        FLUX_FAIL_VK(FLUX_ERROR_BACKEND_FAILURE, "vkGetMemoryFdPropertiesKHR failed", vr);
        r = FLUX_ERROR_BACKEND_FAILURE;
        goto fail;
    }

    VkImageMemoryRequirementsInfo2 mri = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2,
        .image = im->image,
    };
    VkMemoryRequirements2 mreq = {.sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2};
    vkGetImageMemoryRequirements2(d->device, &mri, &mreq);

    uint32_t type_bits = mreq.memoryRequirements.memoryTypeBits & fd_props.memoryTypeBits;
    uint32_t mem_type = flux_vk_find_memory_type(d, type_bits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (mem_type == UINT32_MAX) {
        mem_type = flux_vk_find_memory_type(d, type_bits, 0);
    }
    if (mem_type == UINT32_MAX) {
        FLUX_FAIL(FLUX_ERROR_BACKEND_FAILURE, "no memory type for imported dma-buf");
        r = FLUX_ERROR_BACKEND_FAILURE;
        goto fail;
    }

    /* Import the fd as dedicated memory bound to this image. Vulkan takes
     * ownership of the fd on a successful vkAllocateMemory. */
    VkMemoryDedicatedAllocateInfo dedicated = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
        .image = im->image,
    };
    VkImportMemoryFdInfoKHR import_info = {
        .sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR,
        .pNext = &dedicated,
        .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
        .fd = import_fd,
    };
    VkMemoryAllocateInfo mai = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .pNext = &import_info,
        .allocationSize = mreq.memoryRequirements.size,
        .memoryTypeIndex = mem_type,
    };
    vr = vkAllocateMemory(d->device, &mai, nullptr, &im->imported_memory);
    if (vr == VK_SUCCESS)
        import_fd_transferred = true;
    if (vr != VK_SUCCESS) {
        FLUX_FAIL_VK(FLUX_ERROR_BACKEND_FAILURE, "dma-buf vkAllocateMemory failed", vr);
        r = FLUX_ERROR_BACKEND_FAILURE;
        goto fail;
    }

    VkBindImageMemoryInfo bind = {
        .sType = VK_STRUCTURE_TYPE_BIND_IMAGE_MEMORY_INFO,
        .image = im->image,
        .memory = im->imported_memory,
        .memoryOffset = 0,
    };
    vr = vkBindImageMemory2(d->device, 1, &bind);
    if (vr != VK_SUCCESS) {
        FLUX_FAIL_VK(FLUX_ERROR_BACKEND_FAILURE, "dma-buf vkBindImageMemory2 failed", vr);
        r = FLUX_ERROR_BACKEND_FAILURE;
        goto fail;
    }

    r = dmabuf_transition_image_layout(d, im->image, acquire_sem);
    if (r != FLUX_OK)
        goto fail;
    /* The deferred transition's pending entry now owns the semaphore;
     * clear it so the success/fail paths below do not destroy it. */
    acquire_sem = VK_NULL_HANDLE;
    im->current_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    im->foreign_owned = false;

    VkImageViewCreateInfo ivci = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = im->image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = vfmt,
        .subresourceRange =
            {
                .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                .levelCount = 1,
                .layerCount = 1,
            },
    };
    vr = vkCreateImageView(d->device, &ivci, nullptr, &im->view);
    if (vr != VK_SUCCESS) {
        FLUX_FAIL_VK(FLUX_ERROR_BACKEND_FAILURE, "dma-buf vkCreateImageView failed", vr);
        r = FLUX_ERROR_BACKEND_FAILURE;
        goto fail;
    }

    r = flux_bindless_register_image(d, im->view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                     &im->bindless);
    if (r != FLUX_OK)
        goto fail;

    *out = im;
    /* The import bypasses the slab; count it so stats and the teardown
     * leak warning see it. Uncounted at retire time via imported_size. */
    flux_vk_allocator_note_external(d, mreq.memoryRequirements.size);
    im->imported_size = mreq.memoryRequirements.size;
    close(plane->fd);
    if (desc->has_acquire_sync_fd)
        close(desc->acquire_sync_fd);
    if (acquire_sem)
        vkDestroySemaphore(d->device, acquire_sem, nullptr);
    return FLUX_OK;

fail:
    if (acquire_sem)
        vkDestroySemaphore(d->device, acquire_sem, nullptr);
    if (!import_fd_transferred && import_fd >= 0)
        close(import_fd);
    if (im->bindless != FLUX_BINDLESS_INVALID)
        flux_bindless_release(d, im->bindless);
    if (im->view)
        vkDestroyImageView(d->device, im->view, nullptr);
    if (im->image)
        vkDestroyImage(d->device, im->image, nullptr);
    if (im->imported_memory)
        vkFreeMemory(d->device, im->imported_memory, nullptr);
    flux_device_release(d);
    flux_internal_free(d, im);
    return r;
}
