/*
 * Surface + swapchain / offscreen target + per-frame state.
 *
 * Caller creates VkSurfaceKHR via GLFW/SDL/raw platform code and
 * hands it in via flux_surface_desc.vk_surface_khr. flux owns the
 * VkSwapchainKHR (or, for an offscreen surface, the per-frame RGBA8
 * images per ADR-0013) and the per-frame command pools / semaphores /
 * fence.
 *
 * GPU memory helpers, the transient ring, and one-shot upload paths
 * live in memory.c and oneshot.c.
 */
#include "internal.h"

#include <flux/dmabuf.h>

#ifdef FLUX_HAVE_DMABUF
#include <unistd.h> /* close() on exported dma-buf / sync fds */
#endif

/* FLUX_DRM_FORMAT_MOD_LINEAR == 0 (see drm_fourcc.h). We avoid the libdrm header
 * dependency here because the modifier value is all we need for export, and
 * dmabuf.c (the import side) already keeps the same convention. */
#define FLUX_DRM_FORMAT_MOD_LINEAR 0ULL
#include <flux/vulkan.h>

#include <stdlib.h>
#include <string.h>

typedef struct flux_surface_image_storage {
    uint32_t capacity;
    VkImage *images;
    VkImageView *views;
    VkImageLayout *layouts;
    bool *foreign_owned;
    bool *sync_exported;
    VkSemaphore *render_finished;
    flux_vk_alloc *allocs;
} flux_surface_image_storage;

struct flux_readback {
    flux_device *device;
    flux_staging_buf *staging;
    VkFormat format;
    flux_readback_region region;
};

static void image_storage_free(flux_device *d, flux_surface_image_storage *storage) {
    if (!storage)
        return;
    flux_internal_free(d, storage->images);
    flux_internal_free(d, storage->views);
    flux_internal_free(d, storage->layouts);
    flux_internal_free(d, storage->foreign_owned);
    flux_internal_free(d, storage->sync_exported);
    flux_internal_free(d, storage->render_finished);
    flux_internal_free(d, storage->allocs);
    *storage = (flux_surface_image_storage){0};
}

static void *image_storage_alloc_array(flux_device *d, uint32_t count, size_t element_size) {
    size_t bytes = 0;
    if (!flux_platform_mul_size((size_t)count, element_size, &bytes))
        return nullptr;
    return flux_internal_alloc(d, bytes);
}

static bool image_storage_alloc(flux_device *d, uint32_t count,
                                flux_surface_image_storage *storage) {
    *storage = (flux_surface_image_storage){0};
    if (count == 0)
        return false;

    storage->images = image_storage_alloc_array(d, count, sizeof(*storage->images));
    storage->views = image_storage_alloc_array(d, count, sizeof(*storage->views));
    storage->layouts = image_storage_alloc_array(d, count, sizeof(*storage->layouts));
    storage->foreign_owned = image_storage_alloc_array(d, count, sizeof(*storage->foreign_owned));
    storage->sync_exported = image_storage_alloc_array(d, count, sizeof(*storage->sync_exported));
    storage->render_finished =
        image_storage_alloc_array(d, count, sizeof(*storage->render_finished));
    storage->allocs = image_storage_alloc_array(d, count, sizeof(*storage->allocs));
    if (!storage->images || !storage->views || !storage->layouts || !storage->foreign_owned ||
        !storage->sync_exported || !storage->render_finished || !storage->allocs) {
        image_storage_free(d, storage);
        return false;
    }
    storage->capacity = count;
    return true;
}

static void surface_take_image_storage(flux_surface *s, flux_surface_image_storage *storage,
                                       uint32_t count) {
    s->image_count = count;
    s->image_capacity = storage->capacity;
    s->images = storage->images;
    s->image_views = storage->views;
    s->image_layouts = storage->layouts;
    s->image_foreign_owned = storage->foreign_owned;
    s->image_sync_exported = storage->sync_exported;
    s->render_finished = storage->render_finished;
    s->image_allocs = storage->allocs;
    *storage = (flux_surface_image_storage){0};
}

static void surface_free_image_storage(flux_surface *s) {
    flux_surface_image_storage storage = {
        .capacity = s->image_capacity,
        .images = s->images,
        .views = s->image_views,
        .layouts = s->image_layouts,
        .foreign_owned = s->image_foreign_owned,
        .sync_exported = s->image_sync_exported,
        .render_finished = s->render_finished,
        .allocs = s->image_allocs,
    };
    image_storage_free(s->device, &storage);
    s->image_count = 0;
    s->image_capacity = 0;
    s->images = nullptr;
    s->image_views = nullptr;
    s->image_layouts = nullptr;
    s->image_foreign_owned = nullptr;
    s->image_sync_exported = nullptr;
    s->render_finished = nullptr;
    s->image_allocs = nullptr;
}

/* ------------------------------------------------------------------ */
/*  Swapchain format / present-mode selection                          */
/* ------------------------------------------------------------------ */

static VkSurfaceFormatKHR pick_format(VkPhysicalDevice pd, VkSurfaceKHR srf, bool hdr_preferred) {
    VkSurfaceFormatKHR fallback = {VK_FORMAT_UNDEFINED, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR};
    uint32_t count = 0;
    if (vkGetPhysicalDeviceSurfaceFormatsKHR(pd, srf, &count, nullptr) != VK_SUCCESS)
        return fallback;
    if (count == 0)
        return fallback;
    VkSurfaceFormatKHR *fmts = calloc(count, sizeof(*fmts));
    if (!fmts)
        return fallback;
    if (vkGetPhysicalDeviceSurfaceFormatsKHR(pd, srf, &count, fmts) != VK_SUCCESS) {
        free(fmts);
        return fallback;
    }

    /* HDR first if requested. */
    if (hdr_preferred) {
        for (uint32_t i = 0; i < count; ++i) {
            if (fmts[i].format == VK_FORMAT_A2B10G10R10_UNORM_PACK32 &&
                fmts[i].colorSpace == VK_COLOR_SPACE_HDR10_ST2084_EXT) {
                VkSurfaceFormatKHR r = fmts[i];
                free(fmts);
                return r;
            }
        }
        for (uint32_t i = 0; i < count; ++i) {
            if (fmts[i].colorSpace == VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT) {
                VkSurfaceFormatKHR r = fmts[i];
                free(fmts);
                return r;
            }
        }
    }
    /* SDR baseline: prefer BGRA8 sRGB nonlinear; fall back to first format. */
    for (uint32_t i = 0; i < count; ++i) {
        if (fmts[i].format == VK_FORMAT_B8G8R8A8_UNORM &&
            fmts[i].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            VkSurfaceFormatKHR r = fmts[i];
            free(fmts);
            return r;
        }
    }
    VkSurfaceFormatKHR r = fmts[0];
    free(fmts);
    return r;
}

/* Pick the composite-alpha mode the swapchain will be presented with.
 *
 * For surfaces that want true per-pixel transparency (e.g. an IME popup
 * with rounded corners cleared to rgba(0,0,0,0)), OPAQUE forces the
 * compositor to treat the whole buffer as alpha=1 and the cleared
 * corners show up as solid black. Wayland's dmabuf / wl_shm contract is
 * premultiplied alpha, so PRE_MULTIPLIED is the right pick when the
 * compositor advertises it; INHERIT (defer to the wl_surface, which the
 * caller leaves without an opaque region) is the next best. OPAQUE is
 * only a last resort — it reproduces the "black corners" regression. */
static VkCompositeAlphaFlagBitsKHR pick_composite_alpha(VkCompositeAlphaFlagsKHR supported) {
    if (supported & VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR)
        return VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR;
    if (supported & VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR)
        return VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;
    if (supported & VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR)
        return VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR;
    return VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
}

static VkPresentModeKHR pick_present_mode(VkPhysicalDevice pd, VkSurfaceKHR srf, bool vsync) {
    if (vsync)
        return VK_PRESENT_MODE_FIFO_KHR; /* guaranteed to be supported */

    uint32_t count = 0;
    if (vkGetPhysicalDeviceSurfacePresentModesKHR(pd, srf, &count, nullptr) != VK_SUCCESS) {
        return VK_PRESENT_MODE_FIFO_KHR;
    }
    if (count == 0)
        return VK_PRESENT_MODE_FIFO_KHR;
    VkPresentModeKHR *modes = calloc(count, sizeof(*modes));
    if (!modes)
        return VK_PRESENT_MODE_FIFO_KHR;
    if (vkGetPhysicalDeviceSurfacePresentModesKHR(pd, srf, &count, modes) != VK_SUCCESS) {
        free(modes);
        return VK_PRESENT_MODE_FIFO_KHR;
    }

    VkPresentModeKHR pick = VK_PRESENT_MODE_FIFO_KHR;
    for (uint32_t i = 0; i < count; ++i) {
        if (modes[i] == VK_PRESENT_MODE_MAILBOX_KHR) {
            pick = modes[i];
            break;
        }
        if (modes[i] == VK_PRESENT_MODE_IMMEDIATE_KHR)
            pick = modes[i]; /* keep, but prefer mailbox */
    }
    free(modes);
    return pick;
}

/* ------------------------------------------------------------------ */
/*  Per-frame binary semaphore reset                                  */
/* ------------------------------------------------------------------ */

/* Destroy + recreate acquire semaphores after swapchain failure. A binary
 * semaphore signalled by an acquire whose submit was discarded cannot be
 * reused. Caller must have already waited the device idle. Present-wait
 * semaphores are owned by swapchain image and recreated by swapchain setup. */
static bool reset_frame_semaphores(flux_surface *s) {
    VkDevice vkd = s->device->device;
    VkSemaphoreCreateInfo sem = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    for (uint32_t i = 0; i < s->frames_in_flight; ++i) {
        flux_per_frame *f = &s->frames[i];
        if (f->image_acquired)
            vkDestroySemaphore(vkd, f->image_acquired, nullptr);
        f->image_acquired = VK_NULL_HANDLE;

        VkResult vr = vkCreateSemaphore(vkd, &sem, nullptr, &f->image_acquired);
        if (vr != VK_SUCCESS) {
            FLUX_FAIL_VK(FLUX_ERROR_BACKEND_FAILURE, "vkCreateSemaphore (image_acquired) failed",
                         vr);
            return false;
        }
    }
    return true;
}

/* ------------------------------------------------------------------ */
/*  Swapchain create / destroy                                        */
/* ------------------------------------------------------------------ */

flux_result flux_surface_create_swapchain(flux_surface *s, uint32_t want_w, uint32_t want_h) {
    VkSurfaceCapabilitiesKHR caps;
    VkResult vr =
        vkGetPhysicalDeviceSurfaceCapabilitiesKHR(s->device->physical_device, s->vk_surface, &caps);
    if (vr != VK_SUCCESS) {
        FLUX_FAIL_VK(FLUX_ERROR_BACKEND_FAILURE, "GetPhysicalDeviceSurfaceCapabilities failed", vr);
        return FLUX_ERROR_BACKEND_FAILURE;
    }

    VkExtent2D extent = caps.currentExtent;
    if (extent.width == UINT32_C(0xffffffff)) {
        extent.width = want_w;
        extent.height = want_h;
        if (extent.width < caps.minImageExtent.width)
            extent.width = caps.minImageExtent.width;
        if (extent.height < caps.minImageExtent.height)
            extent.height = caps.minImageExtent.height;
        if (extent.width > caps.maxImageExtent.width)
            extent.width = caps.maxImageExtent.width;
        if (extent.height > caps.maxImageExtent.height)
            extent.height = caps.maxImageExtent.height;
    }
    if (extent.width == 0 || extent.height == 0) {
        /* Minimised; defer swapchain creation until next resize. */
        s->extent = (VkExtent2D){0, 0};
        return FLUX_OK;
    }

    VkSurfaceFormatKHR fmt =
        pick_format(s->device->physical_device, s->vk_surface, s->hdr_preferred);
    VkPresentModeKHR pmode = pick_present_mode(s->device->physical_device, s->vk_surface, s->vsync);

    uint32_t image_count = caps.minImageCount;
    if (image_count < UINT32_MAX)
        image_count++;
    if (caps.maxImageCount > 0 && image_count > caps.maxImageCount)
        image_count = caps.maxImageCount;
    bool readback_supported = (caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) != 0;

    VkSwapchainCreateInfoKHR sci = {
        .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
        .surface = s->vk_surface,
        .minImageCount = image_count,
        .imageFormat = fmt.format,
        .imageColorSpace = fmt.colorSpace,
        .imageExtent = extent,
        .imageArrayLayers = 1,
        .imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                      (readback_supported ? VK_IMAGE_USAGE_TRANSFER_SRC_BIT : 0),
        .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .preTransform = caps.currentTransform,
        .compositeAlpha = pick_composite_alpha(caps.supportedCompositeAlpha),
        .presentMode = pmode,
        .clipped = VK_TRUE,
        .oldSwapchain = s->swapchain, /* may be VK_NULL_HANDLE on first create */
    };

    VkSwapchainKHR new_swapchain = VK_NULL_HANDLE;
    vr = vkCreateSwapchainKHR(s->device->device, &sci, nullptr, &new_swapchain);
    if (vr != VK_SUCCESS) {
        FLUX_FAIL_VK(FLUX_ERROR_BACKEND_FAILURE, "vkCreateSwapchainKHR failed", vr);
        return FLUX_ERROR_BACKEND_FAILURE;
    }

    uint32_t got = 0;
    vr = vkGetSwapchainImagesKHR(s->device->device, new_swapchain, &got, nullptr);
    if (vr != VK_SUCCESS || got == 0) {
        vkDestroySwapchainKHR(s->device->device, new_swapchain, nullptr);
        FLUX_FAIL_VK(FLUX_ERROR_BACKEND_FAILURE, "vkGetSwapchainImagesKHR failed", vr);
        return FLUX_ERROR_BACKEND_FAILURE;
    }
    flux_surface_image_storage storage = {0};
    uint32_t new_count = 0;
    for (uint32_t attempt = 0; attempt < 3; ++attempt) {
        if (!image_storage_alloc(s->device, got, &storage)) {
            vkDestroySwapchainKHR(s->device->device, new_swapchain, nullptr);
            FLUX_FAIL(FLUX_ERROR_OUT_OF_MEMORY, "swapchain image metadata allocation failed");
            return FLUX_ERROR_OUT_OF_MEMORY;
        }
        new_count = got;
        vr = vkGetSwapchainImagesKHR(s->device->device, new_swapchain, &new_count, storage.images);
        if (vr == VK_SUCCESS)
            break;
        image_storage_free(s->device, &storage);
        if (vr != VK_INCOMPLETE ||
            vkGetSwapchainImagesKHR(s->device->device, new_swapchain, &got, nullptr) !=
                VK_SUCCESS ||
            got == 0) {
            vkDestroySwapchainKHR(s->device->device, new_swapchain, nullptr);
            FLUX_FAIL_VK(FLUX_ERROR_BACKEND_FAILURE, "vkGetSwapchainImagesKHR failed", vr);
            return FLUX_ERROR_BACKEND_FAILURE;
        }
    }
    if (vr != VK_SUCCESS || !storage.images) {
        image_storage_free(s->device, &storage);
        vkDestroySwapchainKHR(s->device->device, new_swapchain, nullptr);
        FLUX_FAIL_VK(FLUX_ERROR_BACKEND_FAILURE, "swapchain image count did not stabilise", vr);
        return FLUX_ERROR_BACKEND_FAILURE;
    }

    VkSemaphoreCreateInfo sem_ci = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    for (uint32_t i = 0; i < new_count; ++i) {
        VkImageViewCreateInfo ivci = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .image = storage.images[i],
            .viewType = VK_IMAGE_VIEW_TYPE_2D,
            .format = fmt.format,
            .subresourceRange =
                {
                    .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                    .baseMipLevel = 0,
                    .levelCount = 1,
                    .baseArrayLayer = 0,
                    .layerCount = 1,
                },
        };
        vr = vkCreateImageView(s->device->device, &ivci, nullptr, &storage.views[i]);
        if (vr != VK_SUCCESS) {
            FLUX_FAIL_VK(FLUX_ERROR_BACKEND_FAILURE, "vkCreateImageView failed", vr);
            goto new_swapchain_fail;
        }
        vr = vkCreateSemaphore(s->device->device, &sem_ci, nullptr, &storage.render_finished[i]);
        if (vr != VK_SUCCESS) {
            FLUX_FAIL_VK(FLUX_ERROR_BACKEND_FAILURE, "vkCreateSemaphore (render_finished) failed",
                         vr);
            goto new_swapchain_fail;
        }
    }

    /* Commit only after every image-side resource for the new swapchain exists. */
    flux_surface_destroy_swapchain(s);
    s->swapchain = new_swapchain;
    s->format = fmt.format;
    s->color_space = fmt.colorSpace;
    s->extent = extent;
    s->hdr_actual = (fmt.colorSpace != VK_COLOR_SPACE_SRGB_NONLINEAR_KHR);
    s->readback_supported = readback_supported;
    surface_take_image_storage(s, &storage, new_count);
    for (uint32_t i = 0; i < new_count; ++i) {
        s->image_layouts[i] = VK_IMAGE_LAYOUT_UNDEFINED;
        s->image_foreign_owned[i] = false;
    }

    return FLUX_OK;

new_swapchain_fail:
    for (uint32_t i = 0; i < new_count; ++i) {
        if (storage.render_finished[i])
            vkDestroySemaphore(s->device->device, storage.render_finished[i], nullptr);
        if (storage.views[i])
            vkDestroyImageView(s->device->device, storage.views[i], nullptr);
    }
    image_storage_free(s->device, &storage);
    vkDestroySwapchainKHR(s->device->device, new_swapchain, nullptr);
    return FLUX_ERROR_BACKEND_FAILURE;
}

/* ------------------------------------------------------------------ */
/*  Offscreen images (ADR-0013)                                       */
/* ------------------------------------------------------------------ */

static void offscreen_destroy_images(flux_surface *s) {
    if (s->readback_staging) {
        flux_vk_staging_release(s->device, s->readback_staging);
        s->readback_staging = NULL;
    }
    s->last_readback_slot = UINT32_MAX;
    s->last_readback_region = (flux_readback_region){0};
    for (uint32_t i = 0; i < s->image_count; ++i) {
        if (s->image_views[i])
            vkDestroyImageView(s->device->device, s->image_views[i], nullptr);
        if (s->images[i])
            vkDestroyImage(s->device->device, s->images[i], nullptr);
        if (s->image_allocs[i].memory)
            flux_vk_deallocate(s->device, &s->image_allocs[i]);
        if (s->render_finished[i])
            vkDestroySemaphore(s->device->device, s->render_finished[i], nullptr);
        s->image_views[i] = VK_NULL_HANDLE;
        s->images[i] = VK_NULL_HANDLE;
        s->image_layouts[i] = VK_IMAGE_LAYOUT_UNDEFINED;
        s->image_foreign_owned[i] = false;
        s->image_sync_exported[i] = false;
        s->render_finished[i] = VK_NULL_HANDLE;
        s->image_allocs[i] = (flux_vk_alloc){0};
    }
    surface_free_image_storage(s);
}

static bool offscreen_modifier_exportable(flux_device *d, VkFormat format, uint64_t modifier) {
    VkPhysicalDeviceExternalImageFormatInfo eifi = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO,
        .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
    };
    VkPhysicalDeviceImageDrmFormatModifierInfoEXT mfi = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_DRM_FORMAT_MODIFIER_INFO_EXT,
        .pNext = &eifi,
        .drmFormatModifier = modifier,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    VkPhysicalDeviceImageFormatInfo2 ifi = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2,
        .pNext = &mfi,
        .format = format,
        .type = VK_IMAGE_TYPE_2D,
        .tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT,
        .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                 VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
    };
    VkExternalImageFormatProperties efp = {
        .sType = VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES,
    };
    VkImageFormatProperties2 ifp = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2,
        .pNext = &efp,
    };
    VkResult vr = vkGetPhysicalDeviceImageFormatProperties2(d->physical_device, &ifi, &ifp);
    return vr == VK_SUCCESS && (efp.externalMemoryProperties.externalMemoryFeatures &
                                VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT) != 0;
}

/* One color image per frame slot; image index == frame slot, so the
 * per-slot fence already serialises reuse. `allowed_modifiers == NULL` keeps
 * the general offscreen behaviour; a non-NULL list constrains zero-copy
 * export to modifiers accepted by the external consumer. */
static flux_result offscreen_create_images(flux_surface *s, uint32_t w, uint32_t h,
                                           const uint64_t *allowed_modifiers,
                                           uint32_t allowed_modifier_count) {
    if (w == 0 || h == 0) {
        FLUX_FAIL(FLUX_ERROR_INVALID_ARGUMENT, "offscreen surface needs non-zero width and height");
        return FLUX_ERROR_INVALID_ARGUMENT;
    }

    offscreen_destroy_images(s);

    flux_surface_image_storage storage = {0};
    if (!image_storage_alloc(s->device, s->frames_in_flight, &storage)) {
        FLUX_FAIL(FLUX_ERROR_OUT_OF_MEMORY, "offscreen image metadata allocation failed");
        return FLUX_ERROR_OUT_OF_MEMORY;
    }
    surface_take_image_storage(s, &storage, 0);

    s->format = VK_FORMAT_B8G8R8A8_UNORM;
    s->color_space = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    s->extent = (VkExtent2D){w, h};
    s->hdr_actual = false;

    /* Decide whether offscreen images should be exportable as dma-buf. This
     * requires the device's external-memory / DRM-modifier extensions and an
     * exportable modifier that supports colour-attachment + transfer. When
     * the host wants zero-copy presentation via linux-dmabuf, it enables the
     * extensions at flux_device_create and we pick the exportable path here.
     * Otherwise we fall back to the original OPTIMAL-tiling slab-allocated
     * image (read back via flux_surface_read_pixels). */
    flux_device *d = s->device;
    bool want_export = flux_dmabuf_supported(d) && !s->offscreen_require_readback;
    s->offscreen_exportable = false;
    s->offscreen_modifier = 0;
    s->offscreen_stride = 0;

    uint64_t chosen_modifier = 0;
    bool use_modifier = false;
    if (want_export) {
        /* Query which modifiers support rendering + readback for BGRA8.
         * `allowed_modifiers`, when present, is a producer preference order:
         * compositors put device-native tiled layouts first and LINEAR last.
         * Respecting that order is essential for high-resolution animated
         * scanout targets; unconditionally preferring LINEAR turns every
         * frame into an uncompressed full-image memory write. Without an
         * explicit order, prefer any native layout and keep LINEAR as the
         * compatibility fallback. */
        VkDrmFormatModifierPropertiesListEXT list = {
            .sType = VK_STRUCTURE_TYPE_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_EXT,
        };
        VkFormatProperties2 fprops = {
            .sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2,
            .pNext = &list,
        };
        vkGetPhysicalDeviceFormatProperties2(d->physical_device, s->format, &fprops);
        if (list.drmFormatModifierCount > 0) {
            VkDrmFormatModifierPropertiesEXT *mods =
                flux_internal_alloc(d, sizeof(*mods) * list.drmFormatModifierCount);
            if (mods) {
                list.pDrmFormatModifierProperties = mods;
                vkGetPhysicalDeviceFormatProperties2(d->physical_device, s->format, &fprops);
                const uint32_t need = VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT |
                                      VK_FORMAT_FEATURE_TRANSFER_SRC_BIT |
                                      VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
                if (allowed_modifiers && allowed_modifier_count > 0) {
                    for (uint32_t preferred = 0;
                         preferred < allowed_modifier_count && !use_modifier; ++preferred) {
                        for (uint32_t i = 0; i < list.drmFormatModifierCount; ++i) {
                            if (mods[i].drmFormatModifier != allowed_modifiers[preferred])
                                continue;
                            if ((mods[i].drmFormatModifierTilingFeatures & need) != need ||
                                mods[i].drmFormatModifierPlaneCount != 1)
                                continue;
                            if (!offscreen_modifier_exportable(d, s->format,
                                                               mods[i].drmFormatModifier))
                                continue;
                            chosen_modifier = mods[i].drmFormatModifier;
                            use_modifier = true;
                            break;
                        }
                    }
                } else {
                    /* Two passes: native/tiled first, LINEAR only if no native
                     * exportable modifier is available. */
                    for (uint32_t fallback = 0; fallback < 2 && !use_modifier; ++fallback) {
                        for (uint32_t i = 0; i < list.drmFormatModifierCount; ++i) {
                            bool linear = mods[i].drmFormatModifier == FLUX_DRM_FORMAT_MOD_LINEAR;
                            if (linear != (fallback == 1))
                                continue;
                            if ((mods[i].drmFormatModifierTilingFeatures & need) != need ||
                                mods[i].drmFormatModifierPlaneCount != 1)
                                continue;
                            if (!offscreen_modifier_exportable(d, s->format,
                                                               mods[i].drmFormatModifier))
                                continue;
                            chosen_modifier = mods[i].drmFormatModifier;
                            use_modifier = true;
                            break;
                        }
                    }
                }
                flux_internal_free(d, mods);
            }
        }
    }

    if (allowed_modifiers && !use_modifier) {
        offscreen_destroy_images(s);
        FLUX_FAIL(FLUX_ERROR_UNSUPPORTED, "no shared renderable/exportable dma-buf modifier");
        return FLUX_ERROR_UNSUPPORTED;
    }

    s->offscreen_exportable = use_modifier;
    if (use_modifier)
        s->offscreen_modifier = chosen_modifier;

    /* Image create-info. The modifier path chains an external-memory + DRM-
     * modifier pair; the fallback path uses plain OPTIMAL tiling. */
    VkExternalMemoryImageCreateInfo ext_mem = {
        .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
        .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
    };
    VkImageDrmFormatModifierListCreateInfoEXT mod_list = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_LIST_CREATE_INFO_EXT,
        .pNext = use_modifier ? &ext_mem : nullptr,
        .drmFormatModifierCount = 1,
        .pDrmFormatModifiers = &chosen_modifier,
    };

    VkImageCreateInfo ici = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .pNext = use_modifier ? &mod_list : nullptr,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = s->format,
        .extent = {w, h, 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = use_modifier ? VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT : VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                 VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };

    for (uint32_t i = 0; i < s->frames_in_flight; ++i) {
        if (use_modifier) {
            /* Exportable path: create image, then dedicated-allocate memory
             * bound to it so the VkDeviceMemory can be exported as a dma-buf. */
            VkResult vr = vkCreateImage(d->device, &ici, nullptr, &s->images[i]);
            if (vr != VK_SUCCESS) {
                FLUX_FAIL_VK(FLUX_ERROR_BACKEND_FAILURE, "exportable vkCreateImage", vr);
                offscreen_destroy_images(s);
                return FLUX_ERROR_BACKEND_FAILURE;
            }
            s->image_count = i + 1;

            VkExportMemoryAllocateInfo export_info = {
                .sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO,
                .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
            };
            VkMemoryDedicatedAllocateInfo dedicated = {
                .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
                .pNext = &export_info,
                .image = s->images[i],
            };
            VkMemoryRequirements2 mreq = {.sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2};
            VkImageMemoryRequirementsInfo2 mri = {
                .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2,
                .image = s->images[i],
            };
            vkGetImageMemoryRequirements2(d->device, &mri, &mreq);
            uint32_t mt = flux_vk_find_memory_type(d, mreq.memoryRequirements.memoryTypeBits,
                                                   VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
            if (mt == UINT32_MAX)
                mt = flux_vk_find_memory_type(d, mreq.memoryRequirements.memoryTypeBits, 0);
            if (mt == UINT32_MAX) {
                offscreen_destroy_images(s);
                FLUX_FAIL(FLUX_ERROR_UNSUPPORTED, "no memory type for exportable image");
                return FLUX_ERROR_UNSUPPORTED;
            }
            VkMemoryAllocateInfo mai = {
                .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                .pNext = &dedicated,
                .allocationSize = mreq.memoryRequirements.size,
                .memoryTypeIndex = mt,
            };
            VkDeviceMemory mem = VK_NULL_HANDLE;
            vr = vkAllocateMemory(d->device, &mai, nullptr, &mem);
            if (vr != VK_SUCCESS) {
                offscreen_destroy_images(s);
                FLUX_FAIL_VK(FLUX_ERROR_BACKEND_FAILURE, "exportable vkAllocateMemory", vr);
                return FLUX_ERROR_BACKEND_FAILURE;
            }
            vr = vkBindImageMemory(d->device, s->images[i], mem, 0);
            if (vr != VK_SUCCESS) {
                vkFreeMemory(d->device, mem, nullptr);
                offscreen_destroy_images(s);
                FLUX_FAIL_VK(FLUX_ERROR_BACKEND_FAILURE, "exportable vkBindImageMemory", vr);
                return FLUX_ERROR_BACKEND_FAILURE;
            }
            s->image_allocs[i].memory = mem;
            s->image_allocs[i].offset = 0;
            s->image_allocs[i].size = mreq.memoryRequirements.size;
            s->image_allocs[i].mapped = NULL;
            s->image_allocs[i].block = NULL;
            /* The export bypasses the slab; count it like an import. */
            flux_vk_allocator_note_external(d, mreq.memoryRequirements.size);

            /* Record the stride from memory-plane 0. For DRM-modifier images
             * the aspect must be VK_IMAGE_ASPECT_MEMORY_PLANE_i_BIT_EXT, not
             * COLOR — the memory-plane layout is what the dma-buf consumer
             * sees. BGRA8 is single-plane, so plane 0 is the only one. */
            VkImageSubresource plane0 = {
                .aspectMask = VK_IMAGE_ASPECT_MEMORY_PLANE_0_BIT_EXT,
            };
            VkSubresourceLayout layout;
            vkGetImageSubresourceLayout(d->device, s->images[i], &plane0, &layout);
            if (layout.offset != 0 || layout.rowPitch > UINT32_MAX ||
                (s->offscreen_stride != 0 && s->offscreen_stride != layout.rowPitch)) {
                /* The v1 public API can describe only one plane at offset 0
                 * with one common 32-bit stride. Keep the image usable but do
                 * not advertise an export contract it cannot express. */
                s->offscreen_exportable = false;
            } else {
                s->offscreen_stride = (uint32_t)layout.rowPitch;
            }
        } else {
            /* Non-exportable path: slab-allocated OPTIMAL-tiling image. */
            flux_result r = flux_vk_alloc_image(d, &ici, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                                                &s->images[i], &s->image_allocs[i]);
            if (r != FLUX_OK) {
                offscreen_destroy_images(s);
                return r;
            }
            s->image_count = i + 1;
        }

        VkImageViewCreateInfo ivci = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .image = s->images[i],
            .viewType = VK_IMAGE_VIEW_TYPE_2D,
            .format = s->format,
            .subresourceRange =
                {
                    .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                    .levelCount = 1,
                    .layerCount = 1,
                },
        };
        VkResult vr = vkCreateImageView(d->device, &ivci, nullptr, &s->image_views[i]);
        if (vr != VK_SUCCESS) {
            FLUX_FAIL_VK(FLUX_ERROR_BACKEND_FAILURE, "offscreen vkCreateImageView failed", vr);
            offscreen_destroy_images(s);
            return FLUX_ERROR_BACKEND_FAILURE;
        }
        if (use_modifier && d->has_external_semaphore_fd) {
            VkExportSemaphoreCreateInfo export_info = {
                .sType = VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO,
                .handleTypes = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT,
            };
            VkSemaphoreCreateInfo semaphore_info = {
                .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
                .pNext = &export_info,
            };
            vr = vkCreateSemaphore(d->device, &semaphore_info, nullptr, &s->render_finished[i]);
            if (vr != VK_SUCCESS) {
                FLUX_FAIL_VK(FLUX_ERROR_BACKEND_FAILURE,
                             "offscreen export semaphore creation failed", vr);
                offscreen_destroy_images(s);
                return FLUX_ERROR_BACKEND_FAILURE;
            }
        }
        s->image_layouts[i] = VK_IMAGE_LAYOUT_UNDEFINED;
        s->image_foreign_owned[i] = false;
        s->image_sync_exported[i] = false;
    }
    if (s->offscreen_require_readback) {
        size_t needed;
        if (!flux_platform_mul_size((size_t)w, (size_t)h, &needed) ||
            !flux_platform_mul_size(needed, 4u, &needed)) {
            offscreen_destroy_images(s);
            return FLUX_ERROR_OUT_OF_RANGE;
        }
        flux_result r = flux_vk_staging_acquire(d, needed, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                                &s->readback_staging);
        if (r != FLUX_OK) {
            offscreen_destroy_images(s);
            return r;
        }
    }
    return FLUX_OK;
}

void flux_surface_destroy_swapchain(flux_surface *s) {
    if (!s || !s->device || !s->device->device)
        return;
    if (s->readback_staging) {
        flux_vk_staging_release(s->device, s->readback_staging);
        s->readback_staging = NULL;
    }
    s->last_readback_slot = UINT32_MAX;
    s->last_readback_region = (flux_readback_region){0};
    for (uint32_t i = 0; i < s->image_count; ++i) {
        if (s->image_views[i])
            vkDestroyImageView(s->device->device, s->image_views[i], nullptr);
        if (s->render_finished[i])
            vkDestroySemaphore(s->device->device, s->render_finished[i], nullptr);
        s->image_views[i] = VK_NULL_HANDLE;
        s->images[i] = VK_NULL_HANDLE;
        s->image_layouts[i] = VK_IMAGE_LAYOUT_UNDEFINED;
        s->image_foreign_owned[i] = false;
        s->render_finished[i] = VK_NULL_HANDLE;
    }
    surface_free_image_storage(s);
    if (s->swapchain) {
        vkDestroySwapchainKHR(s->device->device, s->swapchain, nullptr);
        s->swapchain = VK_NULL_HANDLE;
    }
}

/* ------------------------------------------------------------------ */
/*  Per-frame state                                                   */
/* ------------------------------------------------------------------ */

static void destroy_per_frame(flux_surface *s, uint32_t slot);

static flux_result init_per_frame(flux_surface *s, uint32_t slot) {
    flux_per_frame *f = &s->frames[slot];
    *f = (flux_per_frame){0};

    VkCommandPoolCreateInfo pci = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .queueFamilyIndex = s->device->graphics_family,
        .flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT,
    };
    VkResult vr = vkCreateCommandPool(s->device->device, &pci, nullptr, &f->pool);
    if (vr != VK_SUCCESS) {
        FLUX_FAIL_VK(FLUX_ERROR_BACKEND_FAILURE, "vkCreateCommandPool failed", vr);
        return FLUX_ERROR_BACKEND_FAILURE;
    }

    VkCommandBufferAllocateInfo cbai = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = f->pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 2,
    };
    VkCommandBuffer command_buffers[2] = {0};
    vr = vkAllocateCommandBuffers(s->device->device, &cbai, command_buffers);
    if (vr != VK_SUCCESS) {
        FLUX_FAIL_VK(FLUX_ERROR_BACKEND_FAILURE, "vkAllocateCommandBuffers failed", vr);
        goto fail;
    }
    f->cmd = command_buffers[0];
    f->foreign_acquire_cmd = command_buffers[1];

    VkSemaphoreCreateInfo sem = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    vr = vkCreateSemaphore(s->device->device, &sem, nullptr, &f->image_acquired);
    if (vr != VK_SUCCESS) {
        FLUX_FAIL_VK(FLUX_ERROR_BACKEND_FAILURE, "vkCreateSemaphore failed", vr);
        goto fail;
    }
    VkFenceCreateInfo fci = {
        .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
        .flags = VK_FENCE_CREATE_SIGNALED_BIT,
    };
    vr = vkCreateFence(s->device->device, &fci, nullptr, &f->in_flight);
    if (vr != VK_SUCCESS) {
        FLUX_FAIL_VK(FLUX_ERROR_BACKEND_FAILURE, "vkCreateFence failed", vr);
        goto fail;
    }

    /* Per-frame timestamp query pool (only if the device reports a
     * non-zero timestamp period). */
    if (s->device->props.limits.timestampPeriod > 0.0f) {
        VkQueryPoolCreateInfo qpci = {
            .sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
            .queryType = VK_QUERY_TYPE_TIMESTAMP,
            .queryCount = FLUX_MAX_TIMESTAMPS_PER_FRAME * 2, /* begin+end */
        };
        vkCreateQueryPool(s->device->device, &qpci, nullptr, &f->query_pool);
    }

    return FLUX_OK;

fail:
    destroy_per_frame(s, slot);
    return FLUX_ERROR_BACKEND_FAILURE;
}

static void destroy_per_frame(flux_surface *s, uint32_t slot) {
    flux_per_frame *f = &s->frames[slot];
    VkDevice d = s->device->device;
    flux_frame_foreign_images_destroy(s, f);
    if (f->query_pool)
        vkDestroyQueryPool(d, f->query_pool, nullptr);
    if (f->in_flight)
        vkDestroyFence(d, f->in_flight, nullptr);
    if (f->image_acquired)
        vkDestroySemaphore(d, f->image_acquired, nullptr);
    if (f->pool)
        vkDestroyCommandPool(d, f->pool, nullptr); /* frees cmd too */
    *f = (flux_per_frame){0};
}

/* ------------------------------------------------------------------ */
/*  Public surface API                                                */
/* ------------------------------------------------------------------ */

flux_result flux_surface_create(flux_device *device, const flux_surface_desc *desc,
                                flux_surface **out) {
    if (!device || !desc || !out)
        return FLUX_ERROR_INVALID_ARGUMENT;
    if (desc->type != FLUX_TYPE_SURFACE_DESC) {
        FLUX_FAIL(FLUX_ERROR_INVALID_ARGUMENT, "desc->type != FLUX_TYPE_SURFACE_DESC");
        return FLUX_ERROR_INVALID_ARGUMENT;
    }
    *out = nullptr;

    flux_surface *s = flux_internal_alloc(device, sizeof(*s));
    if (!s)
        return FLUX_ERROR_OUT_OF_MEMORY;
    atomic_init(&s->ref_count, 1u);
    s->device = flux_device_retain(device);
    s->vk_surface = (VkSurfaceKHR)desc->vk_surface_khr;
    s->offscreen = (desc->vk_surface_khr == nullptr); /* ADR-0013 */
    s->readback_supported = s->offscreen;
    s->offscreen_require_readback = false;
    s->vsync = desc->vsync;
    s->hdr_preferred = desc->hdr_preferred;
    s->last_submitted_slot = UINT32_MAX;
    s->last_readback_slot = UINT32_MAX;
    s->last_readback_region = (flux_readback_region){0};
    s->frames_in_flight = device->frames_in_flight;
    if (s->frames_in_flight > FLUX_MAX_FRAMES_IN_FLIGHT)
        s->frames_in_flight = FLUX_MAX_FRAMES_IN_FLIGHT;

    flux_result r;
    if (s->offscreen) {
        const struct {
            flux_struct_type type;
            const void *next;
        } *extension = desc->next;
        while (extension) {
            if (extension->type == FLUX_TYPE_SURFACE_DMABUF_DESC) {
                const flux_surface_dmabuf_desc *dmabuf = (const void *)extension;
                if (!dmabuf->modifiers || dmabuf->modifier_count == 0) {
                    FLUX_FAIL(FLUX_ERROR_INVALID_ARGUMENT,
                              "surface dma-buf extension needs modifiers");
                    r = FLUX_ERROR_INVALID_ARGUMENT;
                    goto fail;
                }
                size_t bytes;
                if (!flux_platform_mul_size((size_t)dmabuf->modifier_count, sizeof(uint64_t),
                                            &bytes)) {
                    r = FLUX_ERROR_OUT_OF_RANGE;
                    goto fail;
                }
                s->offscreen_allowed_modifiers = flux_internal_alloc(device, bytes);
                if (!s->offscreen_allowed_modifiers) {
                    r = FLUX_ERROR_OUT_OF_MEMORY;
                    goto fail;
                }
                memcpy(s->offscreen_allowed_modifiers, dmabuf->modifiers, bytes);
                s->offscreen_allowed_modifier_count = dmabuf->modifier_count;
            } else if (extension->type == FLUX_TYPE_SURFACE_READBACK_DESC) {
                const flux_surface_readback_desc *readback = (const void *)extension;
                s->offscreen_require_readback = readback->require_readback;
            }
            extension = extension->next;
        }
        if (s->offscreen_require_readback && s->offscreen_allowed_modifier_count > 0) {
            FLUX_FAIL(FLUX_ERROR_INVALID_ARGUMENT,
                      "surface readback and dma-buf extensions conflict");
            r = FLUX_ERROR_INVALID_ARGUMENT;
            goto fail;
        }
        r = offscreen_create_images(s, desc->width, desc->height, s->offscreen_allowed_modifiers,
                                    s->offscreen_allowed_modifier_count);
        if (r != FLUX_OK)
            goto fail;
    } else {
        /* Confirm the graphics queue can present to this surface. */
        VkBool32 supported = VK_FALSE;
        vkGetPhysicalDeviceSurfaceSupportKHR(device->physical_device, device->graphics_family,
                                             s->vk_surface, &supported);
        if (!supported) {
            FLUX_FAIL(FLUX_ERROR_UNSUPPORTED,
                      "graphics queue family does not support presentation to this surface");
            flux_device_release(s->device);
            flux_internal_free(device, s);
            return FLUX_ERROR_UNSUPPORTED;
        }

        r = flux_surface_create_swapchain(s, desc->width, desc->height);
        if (r != FLUX_OK)
            goto fail;
    }

    for (uint32_t i = 0; i < s->frames_in_flight; ++i) {
        r = init_per_frame(s, i);
        if (r != FLUX_OK)
            goto fail;
    }

    r = flux_transient_ring_init(&s->transient, device, 0); /* 0 = default size */
    if (r != FLUX_OK)
        goto fail;

    *out = s;
    return FLUX_OK;

fail:
    flux_transient_ring_destroy(&s->transient, device);
    for (uint32_t i = 0; i < s->frames_in_flight; ++i)
        destroy_per_frame(s, i);
    if (s->offscreen)
        offscreen_destroy_images(s);
    else
        flux_surface_destroy_swapchain(s);
    flux_internal_free(device, s->offscreen_allowed_modifiers);
    flux_device_release(s->device);
    flux_internal_free(device, s);
    return r;
}

flux_surface *flux_surface_retain(flux_surface *s) {
    if (s)
        atomic_fetch_add_explicit(&s->ref_count, 1u, memory_order_relaxed);
    return s;
}

/* Wait until the GPU is provably done with every batch that could
 * still reference this surface, without stalling unrelated device work.
 *
 * Offscreen surfaces have no presentation engine: every batch that can
 * touch the surface's images and transient ring was submitted on the
 * graphics queue guarded by a per-slot in_flight fence, so waiting
 * those fences is sufficient (persistent readback copies share the frame
 * fence; legacy one-shot readbacks complete before they return).
 *
 * Windowed surfaces must also cover the presentation engine, which can
 * keep reading a presented image after the frame's fence signals; only
 * a queue wait proves that. It is still narrower than the device-wide
 * vkDeviceWaitIdle used previously: the transfer queue and unrelated
 * devices keep running. */
static void surface_wait_quiescent(flux_surface *s) {
    flux_device *d = s->device;
    if (!s->offscreen) {
        /* Host access to the queue: serialised against concurrent
         * submits, same rule as flux_vk_wait_idle. */
        flux_platform_mutex_lock(&d->queue_lock);
        vkQueueWaitIdle(d->graphics_queue);
        flux_platform_mutex_unlock(&d->queue_lock);
        flux_vk_note_graphics_completed(
            d, atomic_load_explicit(&d->submit_serial, memory_order_acquire));
        return;
    }
    uint64_t max_serial = 0;
    for (uint32_t i = 0; i < s->frames_in_flight; ++i) {
        flux_per_frame *pf = &s->frames[i];
        if (pf->in_flight)
            vkWaitForFences(d->device, 1, &pf->in_flight, VK_TRUE, UINT64_MAX);
        if (pf->submitted_serial > max_serial)
            max_serial = pf->submitted_serial;
    }
    if (max_serial)
        flux_vk_note_graphics_completed(d, max_serial);
}

void flux_surface_release(flux_surface *s) {
    if (!s)
        return;
    if (atomic_fetch_sub_explicit(&s->ref_count, 1u, memory_order_acq_rel) != 1u)
        return;

    surface_wait_quiescent(s);
    flux_transient_ring_destroy(&s->transient, s->device);
    for (uint32_t i = 0; i < s->frames_in_flight; ++i)
        destroy_per_frame(s, i);
    if (s->offscreen)
        offscreen_destroy_images(s);
    else
        flux_surface_destroy_swapchain(s);
    flux_device *dev = s->device;
    flux_internal_free(dev, s->offscreen_allowed_modifiers);
    flux_internal_free(dev, s);
    flux_device_release(dev);
}

flux_result flux_surface_resize(flux_surface *s, uint32_t w, uint32_t h) {
    if (!s)
        return FLUX_ERROR_INVALID_ARGUMENT;

    surface_wait_quiescent(s);
    s->frame_active = false;
    s->frame_slot.state = FLUX_FRAME_STATE_INVALID;
    s->needs_recreate = false;
    if (s->offscreen) {
        s->current_frame = 0;
        s->last_submitted_slot = UINT32_MAX; /* old contents are gone */
        s->last_readback_slot = UINT32_MAX;
        s->last_readback_region = (flux_readback_region){0};
        return offscreen_create_images(s, w, h, s->offscreen_allowed_modifiers,
                                       s->offscreen_allowed_modifier_count);
    }
    /* Discard any acquire-signal carried by per-frame semaphores; an
     * acquire that returned OUT_OF_DATE may have left one signalled. */
    if (!reset_frame_semaphores(s)) {
        return FLUX_ERROR_BACKEND_FAILURE;
    }
    s->current_frame = 0;
    return flux_surface_create_swapchain(s, w, h);
}

void flux_surface_get_info(const flux_surface *s, flux_surface_info *out) {
    if (!out)
        return;
    if (!s) {
        *out = (flux_surface_info){0};
        return;
    }
    out->width = s->extent.width;
    out->height = s->extent.height;
    out->image_count = s->image_count;
    out->hdr = s->hdr_actual;
}

/* ------------------------------------------------------------------ */
/*  Frame readback                                                    */
/* ------------------------------------------------------------------ */

static flux_result readback_byte_size(uint32_t width, uint32_t height, size_t *out) {
    if (!width || !height)
        return FLUX_ERROR_INVALID_ARGUMENT;
    size_t needed;
    if (!flux_platform_mul_size((size_t)width, (size_t)height, &needed) ||
        !flux_platform_mul_size(needed, 4u, &needed))
        return FLUX_ERROR_OUT_OF_RANGE;
    *out = needed;
    return FLUX_OK;
}

static flux_result ensure_readback_staging(flux_surface *s, uint32_t width, uint32_t height) {
    if (!s)
        return FLUX_ERROR_INVALID_ARGUMENT;
    if (!s->readback_supported) {
        FLUX_FAIL(FLUX_ERROR_UNSUPPORTED, "surface images do not support transfer-source readback");
        return FLUX_ERROR_UNSUPPORTED;
    }
    if (!width || !height)
        return FLUX_ERROR_INVALID_ARGUMENT;
    if (width > s->extent.width || height > s->extent.height) {
        FLUX_FAIL(FLUX_ERROR_OUT_OF_RANGE, "readback extent exceeds surface extent");
        return FLUX_ERROR_OUT_OF_RANGE;
    }
    size_t needed;
    flux_result r = readback_byte_size(width, height, &needed);
    if (r != FLUX_OK)
        return r;
    if (s->readback_staging && s->readback_staging->capacity >= needed)
        return FLUX_OK;

    /* Allocate first so allocation failure preserves the previous immutable
     * snapshot. A larger successful request replaces it only after its frame
     * fence proves the old staging is no longer referenced by the GPU. */
    flux_staging_buf *replacement = NULL;
    r = flux_vk_staging_acquire(s->device, needed, VK_BUFFER_USAGE_TRANSFER_DST_BIT, &replacement);
    if (r != FLUX_OK)
        return r;
    if (s->readback_staging && s->last_readback_slot != UINT32_MAX) {
        VkResult vr =
            vkWaitForFences(s->device->device, 1, &s->frames[s->last_readback_slot].in_flight,
                            VK_TRUE, FLUX_DEFAULT_FRAME_TIMEOUT_NS);
        if (vr == VK_TIMEOUT) {
            flux_vk_staging_release(s->device, replacement);
            return FLUX_ERROR_TIMEOUT;
        }
        if (vr != VK_SUCCESS) {
            flux_result wait_result =
                vr == VK_ERROR_DEVICE_LOST ? FLUX_ERROR_DEVICE_LOST : FLUX_ERROR_BACKEND_FAILURE;
            FLUX_FAIL_VK(wait_result, "readback staging resize fence wait failed", vr);
            flux_vk_staging_release(s->device, replacement);
            return wait_result;
        }
    }
    flux_staging_buf *previous = s->readback_staging;
    s->readback_staging = replacement;
    if (previous) {
        flux_vk_staging_release(s->device, previous);
        s->last_readback_slot = UINT32_MAX;
        s->last_readback_region = (flux_readback_region){0};
    }
    return FLUX_OK;
}

flux_result flux_surface_prepare_readback_region(flux_surface *s, uint32_t width, uint32_t height) {
    if (s && s->offscreen_require_readback &&
        (width != s->extent.width || height != s->extent.height)) {
        FLUX_FAIL(FLUX_ERROR_UNSUPPORTED,
                  "region staging is unsupported on require_readback surfaces");
        return FLUX_ERROR_UNSUPPORTED;
    }
    return ensure_readback_staging(s, width, height);
}

flux_result flux_surface_prepare_readback(flux_surface *s) {
    if (!s)
        return FLUX_ERROR_INVALID_ARGUMENT;
    return ensure_readback_staging(s, s->extent.width, s->extent.height);
}

flux_result flux_surface_read_pixels_ready(flux_surface *s, bool *out_ready) {
    if (!s || !out_ready)
        return FLUX_ERROR_INVALID_ARGUMENT;
    *out_ready = false;
    if (!s->readback_staging) {
        FLUX_FAIL(FLUX_ERROR_UNSUPPORTED, "surface has no persistent readback staging");
        return FLUX_ERROR_UNSUPPORTED;
    }
    if (s->last_readback_slot == UINT32_MAX) {
        FLUX_FAIL(FLUX_ERROR_INVALID_STATE, "no frame has been captured for readback");
        return FLUX_ERROR_INVALID_STATE;
    }
    VkResult vr = vkGetFenceStatus(s->device->device, s->frames[s->last_readback_slot].in_flight);
    if (vr == VK_SUCCESS) {
        *out_ready = true;
        return FLUX_OK;
    }
    if (vr == VK_NOT_READY)
        return FLUX_OK;
    flux_result r =
        vr == VK_ERROR_DEVICE_LOST ? FLUX_ERROR_DEVICE_LOST : FLUX_ERROR_BACKEND_FAILURE;
    FLUX_FAIL_VK(r, "read_pixels_ready fence query failed", vr);
    return r;
}

flux_result flux_surface_take_readback(flux_surface *s, flux_readback **out) {
    if (!s || !out)
        return FLUX_ERROR_INVALID_ARGUMENT;
    *out = NULL;
    if (s->offscreen_require_readback) {
        FLUX_FAIL(FLUX_ERROR_UNSUPPORTED, "require_readback staging remains owned by its surface");
        return FLUX_ERROR_UNSUPPORTED;
    }
    if (s->frame_active && s->frame_slot.readback_requested) {
        FLUX_FAIL(FLUX_ERROR_INVALID_STATE,
                  "cannot detach readback staging before the requesting frame is submitted");
        return FLUX_ERROR_INVALID_STATE;
    }
    if (!s->readback_staging || s->last_readback_slot == UINT32_MAX) {
        FLUX_FAIL(FLUX_ERROR_INVALID_STATE, "no captured frame is available to detach");
        return FLUX_ERROR_INVALID_STATE;
    }
    VkResult vr = vkGetFenceStatus(s->device->device, s->frames[s->last_readback_slot].in_flight);
    if (vr == VK_NOT_READY) {
        FLUX_FAIL(FLUX_ERROR_INVALID_STATE, "captured frame is not ready");
        return FLUX_ERROR_INVALID_STATE;
    }
    if (vr != VK_SUCCESS) {
        flux_result r =
            vr == VK_ERROR_DEVICE_LOST ? FLUX_ERROR_DEVICE_LOST : FLUX_ERROR_BACKEND_FAILURE;
        FLUX_FAIL_VK(r, "captured frame fence query failed", vr);
        return r;
    }

    flux_readback *readback = flux_internal_alloc(s->device, sizeof(*readback));
    if (!readback)
        return FLUX_ERROR_OUT_OF_MEMORY;
    *readback = (flux_readback){
        .device = flux_device_retain(s->device),
        .staging = s->readback_staging,
        .format = s->format,
        .region = s->last_readback_region,
    };
    s->readback_staging = NULL;
    s->last_readback_slot = UINT32_MAX;
    s->last_readback_region = (flux_readback_region){0};
    *out = readback;
    return FLUX_OK;
}

flux_result flux_readback_read_pixels(const flux_readback *readback, void *dst, size_t bytes) {
    if (!readback || !dst)
        return FLUX_ERROR_INVALID_ARGUMENT;
    size_t needed;
    if (!flux_platform_mul_size((size_t)readback->region.width, (size_t)readback->region.height,
                                &needed) ||
        !flux_platform_mul_size(needed, 4u, &needed))
        return FLUX_ERROR_OUT_OF_RANGE;
    if (bytes < needed) {
        FLUX_FAIL(FLUX_ERROR_INVALID_ARGUMENT, "dst too small for readback pixels");
        return FLUX_ERROR_INVALID_ARGUMENT;
    }
    const uint8_t *src = readback->staging->alloc.mapped;
    if (!src) {
        FLUX_FAIL(FLUX_ERROR_BACKEND_FAILURE, "readback staging is not mapped");
        return FLUX_ERROR_BACKEND_FAILURE;
    }
    if (readback->format == VK_FORMAT_B8G8R8A8_UNORM ||
        readback->format == VK_FORMAT_B8G8R8A8_SRGB) {
        uint8_t *out_px = dst;
        for (size_t i = 0; i < (size_t)readback->region.width * readback->region.height; ++i) {
            out_px[i * 4u + 0u] = src[i * 4u + 2u];
            out_px[i * 4u + 1u] = src[i * 4u + 1u];
            out_px[i * 4u + 2u] = src[i * 4u + 0u];
            out_px[i * 4u + 3u] = src[i * 4u + 3u];
        }
    } else {
        memcpy(dst, src, needed);
    }
    return FLUX_OK;
}

void flux_readback_get_region(const flux_readback *readback, flux_readback_region *out_region) {
    if (!out_region)
        return;
    *out_region = readback ? readback->region : (flux_readback_region){0};
}

void flux_readback_release(flux_readback *readback) {
    if (!readback)
        return;
    flux_device *device = readback->device;
    flux_vk_staging_release(device, readback->staging);
    flux_internal_free(device, readback);
    flux_device_release(device);
}

flux_result flux_surface_read_pixels(flux_surface *s, void *dst, size_t bytes) {
    if (!s || !dst)
        return FLUX_ERROR_INVALID_ARGUMENT;
    bool persistent_staging = s->readback_staging != NULL;
    uint32_t slot = persistent_staging ? s->last_readback_slot : s->last_submitted_slot;
    if (slot == UINT32_MAX) {
        FLUX_FAIL(FLUX_ERROR_INVALID_STATE, "no frame is available for readback");
        return FLUX_ERROR_INVALID_STATE;
    }
    if (!persistent_staging && !s->offscreen) {
        FLUX_FAIL(FLUX_ERROR_UNSUPPORTED, "windowed readback requires flux_frame_request_readback");
        return FLUX_ERROR_UNSUPPORTED;
    }
    uint32_t read_width = persistent_staging ? s->last_readback_region.width : s->extent.width;
    uint32_t read_height = persistent_staging ? s->last_readback_region.height : s->extent.height;
    size_t needed;
    flux_result size_result = readback_byte_size(read_width, read_height, &needed);
    if (size_result != FLUX_OK)
        return size_result;
    if (bytes < needed) {
        FLUX_FAIL(FLUX_ERROR_INVALID_ARGUMENT, "dst too small for captured readback extent");
        return FLUX_ERROR_INVALID_ARGUMENT;
    }

    flux_device *d = s->device;
    flux_per_frame *pf = &s->frames[slot];
    if (!persistent_staging && s->image_foreign_owned[slot]) {
        FLUX_FAIL(FLUX_ERROR_INVALID_STATE,
                  "offscreen image is still owned by an external dma-buf consumer");
        return FLUX_ERROR_INVALID_STATE;
    }

    /* flux_frame_submit's barrier left the image in TRANSFER_SRC_OPTIMAL;
     * the fence wait makes that write visible to this copy. */
    VkResult vr =
        vkWaitForFences(d->device, 1, &pf->in_flight, VK_TRUE, FLUX_DEFAULT_FRAME_TIMEOUT_NS);
    if (vr == VK_TIMEOUT)
        return FLUX_ERROR_TIMEOUT;
    if (vr != VK_SUCCESS) {
        FLUX_FAIL_VK(FLUX_ERROR_BACKEND_FAILURE, "read_pixels fence wait failed", vr);
        return FLUX_ERROR_BACKEND_FAILURE;
    }

    flux_staging_buf *staging = s->readback_staging;
    flux_result r = FLUX_OK;
    if (!persistent_staging) {
        r = flux_vk_staging_acquire(d, needed, VK_BUFFER_USAGE_TRANSFER_DST_BIT, &staging);
        if (r != FLUX_OK)
            return r;
    }

    if (!staging->alloc.mapped) {
        FLUX_FAIL(FLUX_ERROR_BACKEND_FAILURE,
                  "readback staging allocation came back un-mapped (allocator invariant violated)");
        r = FLUX_ERROR_BACKEND_FAILURE;
        goto out;
    }

    if (!persistent_staging) {
        VkCommandPool pool = VK_NULL_HANDLE;
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        vr = flux_vk_new_transient_cmd(d, d->graphics_family, &pool, &cmd);
        if (vr != VK_SUCCESS) {
            FLUX_FAIL_VK(FLUX_ERROR_BACKEND_FAILURE, "read_pixels cmd alloc failed", vr);
            r = FLUX_ERROR_BACKEND_FAILURE;
            goto out;
        }

        VkBufferImageCopy region = {
            .imageSubresource = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .layerCount = 1},
            .imageExtent = {s->extent.width, s->extent.height, 1},
        };
        vkCmdCopyImageToBuffer(cmd, s->images[slot], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               staging->buffer, 1, &region);

        VkMemoryBarrier2 mb = {
            .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
            .srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT,
            .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
            .dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT,
            .dstAccessMask = VK_ACCESS_2_HOST_READ_BIT,
        };
        VkDependencyInfo di = {
            .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
            .memoryBarrierCount = 1,
            .pMemoryBarriers = &mb,
        };
        vkCmdPipelineBarrier2(cmd, &di);

        vr = vkEndCommandBuffer(cmd);
        if (vr == VK_SUCCESS)
            vr = flux_vk_submit_one_shot_and_wait(d, cmd);
        vkDestroyCommandPool(d->device, pool, nullptr);
        if (vr != VK_SUCCESS) {
            FLUX_FAIL_VK(FLUX_ERROR_BACKEND_FAILURE, "read_pixels copy submit failed", vr);
            r = (vr == VK_TIMEOUT) ? FLUX_ERROR_TIMEOUT : FLUX_ERROR_BACKEND_FAILURE;
            goto out;
        }
    }

    if (s->format == VK_FORMAT_B8G8R8A8_UNORM || s->format == VK_FORMAT_B8G8R8A8_SRGB) {
        const uint8_t *src = (const uint8_t *)staging->alloc.mapped;
        uint8_t *out_px = (uint8_t *)dst;
        for (size_t i = 0; i < (size_t)read_width * read_height; ++i) {
            out_px[i * 4u + 0u] = src[i * 4u + 2u];
            out_px[i * 4u + 1u] = src[i * 4u + 1u];
            out_px[i * 4u + 2u] = src[i * 4u + 0u];
            out_px[i * 4u + 3u] = src[i * 4u + 3u];
        }
    } else {
        memcpy(dst, staging->alloc.mapped, needed);
    }
    r = FLUX_OK;

out:
    if (!persistent_staging)
        flux_vk_staging_release(d, staging);
    return r;
}

/* ------------------------------------------------------------------ */
/*  Offscreen dma-buf export (see ADR-0013 offscreen surface)            */
/* ------------------------------------------------------------------ */

bool flux_surface_exportable(const flux_surface *s) {
    return s && s->offscreen && s->offscreen_exportable;
}

uint32_t flux_surface_last_slot(const flux_surface *s) {
    return s ? s->last_submitted_slot : UINT32_MAX;
}

uint64_t flux_surface_dmabuf_modifier(const flux_surface *s) {
    return s ? s->offscreen_modifier : 0;
}

uint32_t flux_surface_dmabuf_stride(const flux_surface *s) {
    return s ? s->offscreen_stride : 0;
}

/* The fd-based export machinery is Linux-only (dma-buf + sync_file). On
 * other platforms the public entry points remain, but report
 * FLUX_ERROR_UNSUPPORTED; surfaces are simply never created exportable
 * there (flux_dmabuf_supported is false, see the dmabuf stub). */
#ifdef FLUX_HAVE_DMABUF
static flux_result export_surface_memory_fd(flux_surface *s, uint32_t slot, int *out_fd) {
    flux_device *d = s->device;
    PFN_vkGetMemoryFdKHR pGetMemoryFd =
        (PFN_vkGetMemoryFdKHR)vkGetDeviceProcAddr(d->device, "vkGetMemoryFdKHR");
    if (!pGetMemoryFd) {
        FLUX_FAIL(FLUX_ERROR_UNSUPPORTED, "vkGetMemoryFdKHR entry point missing");
        return FLUX_ERROR_UNSUPPORTED;
    }
    VkMemoryGetFdInfoKHR fd_info = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR,
        .memory = s->image_allocs[slot].memory,
        .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
    };
    VkResult vr = pGetMemoryFd(d->device, &fd_info, out_fd);
    if (vr != VK_SUCCESS) {
        FLUX_FAIL_VK(FLUX_ERROR_BACKEND_FAILURE, "vkGetMemoryFdKHR failed", vr);
        return FLUX_ERROR_BACKEND_FAILURE;
    }
    return FLUX_OK;
}

static flux_result export_surface_sync_fd(flux_surface *s, uint32_t slot, int *out_fd) {
    flux_device *d = s->device;
    *out_fd = -1;
    if (!d->has_external_semaphore_fd || s->render_finished[slot] == VK_NULL_HANDLE) {
        FLUX_FAIL(FLUX_ERROR_UNSUPPORTED, "explicit dma-buf sync is unavailable");
        return FLUX_ERROR_UNSUPPORTED;
    }
    if (s->image_sync_exported[slot]) {
        FLUX_FAIL(FLUX_ERROR_INVALID_STATE, "frame sync fd was already exported");
        return FLUX_ERROR_INVALID_STATE;
    }
    PFN_vkGetSemaphoreFdKHR pGetSemaphoreFd =
        (PFN_vkGetSemaphoreFdKHR)vkGetDeviceProcAddr(d->device, "vkGetSemaphoreFdKHR");
    if (!pGetSemaphoreFd) {
        FLUX_FAIL(FLUX_ERROR_UNSUPPORTED, "vkGetSemaphoreFdKHR entry point missing");
        return FLUX_ERROR_UNSUPPORTED;
    }
    VkSemaphoreGetFdInfoKHR info = {
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR,
        .semaphore = s->render_finished[slot],
        .handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT,
    };
    VkResult vr = pGetSemaphoreFd(d->device, &info, out_fd);
    if (vr != VK_SUCCESS) {
        FLUX_FAIL_VK(FLUX_ERROR_BACKEND_FAILURE, "vkGetSemaphoreFdKHR failed", vr);
        return FLUX_ERROR_BACKEND_FAILURE;
    }
    s->image_sync_exported[slot] = true;
    return FLUX_OK;
}

/* Export the most recently submitted frame's image memory as a dma-buf fd.
 * The caller owns the returned fd and must close() it. Waits for the frame's
 * GPU work to complete first (same fence as flux_surface_read_pixels), so the
 * written pixels are visible. No GPU->CPU pixel copy occurs — this is a zero-
 * copy handle export. Returns FLUX_ERROR_UNSUPPORTED if the surface was not
 * created exportable. */
flux_result flux_surface_export_dmabuf(flux_surface *s, int *out_fd) {
    if (!s || !out_fd)
        return FLUX_ERROR_INVALID_ARGUMENT;
    *out_fd = -1;
    if (!s->offscreen || !s->offscreen_exportable) {
        FLUX_FAIL(FLUX_ERROR_UNSUPPORTED,
                  "flux_surface_export_dmabuf needs an exportable offscreen surface");
        return FLUX_ERROR_UNSUPPORTED;
    }
    if (s->last_submitted_slot == UINT32_MAX) {
        FLUX_FAIL(FLUX_ERROR_INVALID_STATE, "no frame has been submitted to export");
        return FLUX_ERROR_INVALID_STATE;
    }

    flux_device *d = s->device;
    uint32_t slot = s->last_submitted_slot;
    flux_per_frame *pf = &s->frames[slot];

    /* Wait for the frame's GPU work to finish before handing the memory to a
     * foreign consumer. Same fence wait as flux_surface_read_pixels. */
    VkResult vr =
        vkWaitForFences(d->device, 1, &pf->in_flight, VK_TRUE, FLUX_DEFAULT_FRAME_TIMEOUT_NS);
    if (vr == VK_TIMEOUT)
        return FLUX_ERROR_TIMEOUT;
    if (vr != VK_SUCCESS) {
        FLUX_FAIL_VK(FLUX_ERROR_BACKEND_FAILURE, "export_dmabuf fence wait failed", vr);
        return FLUX_ERROR_BACKEND_FAILURE;
    }

    if (!s->image_foreign_owned[slot]) {
        VkCommandPool pool = VK_NULL_HANDLE;
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        vr = flux_vk_new_transient_cmd(d, d->graphics_family, &pool, &cmd);
        if (vr != VK_SUCCESS) {
            FLUX_FAIL_VK(FLUX_ERROR_BACKEND_FAILURE, "export_dmabuf command buffer failed", vr);
            return FLUX_ERROR_BACKEND_FAILURE;
        }
        VkImageMemoryBarrier2 release = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
            .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
            .srcAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
            .dstStageMask = VK_PIPELINE_STAGE_2_NONE,
            .dstAccessMask = 0,
            .oldLayout = s->image_layouts[slot],
            .newLayout = VK_IMAGE_LAYOUT_GENERAL,
            .srcQueueFamilyIndex = d->graphics_family,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_FOREIGN_EXT,
            .image = s->images[slot],
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
            .pImageMemoryBarriers = &release,
        };
        vkCmdPipelineBarrier2(cmd, &di);
        vr = vkEndCommandBuffer(cmd);
        if (vr == VK_SUCCESS)
            vr = flux_vk_submit_one_shot_and_wait(d, cmd);
        vkDestroyCommandPool(d->device, pool, nullptr);
        if (vr != VK_SUCCESS) {
            flux_result r = vr == VK_TIMEOUT             ? FLUX_ERROR_TIMEOUT
                            : vr == VK_ERROR_DEVICE_LOST ? FLUX_ERROR_DEVICE_LOST
                                                         : FLUX_ERROR_BACKEND_FAILURE;
            FLUX_FAIL_VK(r, "export_dmabuf ownership release failed", vr);
            return r;
        }
        s->image_layouts[slot] = VK_IMAGE_LAYOUT_GENERAL;
        s->image_foreign_owned[slot] = true;
    }

    flux_result result = export_surface_memory_fd(s, slot, out_fd);
    if (result != FLUX_OK)
        return result;

    /* A SYNC_FD export transfers the temporary semaphore payload. Legacy
     * callers already waited the fence, so consume and close that payload to
     * leave the binary semaphore reusable on this frame slot. */
    if (d->has_external_semaphore_fd && s->render_finished[slot] != VK_NULL_HANDLE &&
        !s->image_sync_exported[slot]) {
        int sync_fd = -1;
        result = export_surface_sync_fd(s, slot, &sync_fd);
        if (result != FLUX_OK) {
            close(*out_fd);
            *out_fd = -1;
            return result;
        }
        close(sync_fd);
    }
    return FLUX_OK;
}

flux_result flux_surface_export_dmabuf_explicit(flux_surface *s, int *out_fd, int *out_sync_fd) {
    if (!s || !out_fd || !out_sync_fd)
        return FLUX_ERROR_INVALID_ARGUMENT;
    *out_fd = -1;
    *out_sync_fd = -1;
    if (!s->offscreen || !s->offscreen_exportable) {
        FLUX_FAIL(FLUX_ERROR_UNSUPPORTED,
                  "explicit dma-buf export needs an exportable offscreen surface");
        return FLUX_ERROR_UNSUPPORTED;
    }
    if (s->last_submitted_slot == UINT32_MAX) {
        FLUX_FAIL(FLUX_ERROR_INVALID_STATE, "no frame has been submitted to export");
        return FLUX_ERROR_INVALID_STATE;
    }
    uint32_t slot = s->last_submitted_slot;
    if (!s->image_foreign_owned[slot]) {
        FLUX_FAIL(FLUX_ERROR_INVALID_STATE, "submitted image was not released to FOREIGN");
        return FLUX_ERROR_INVALID_STATE;
    }

    flux_result result = export_surface_sync_fd(s, slot, out_sync_fd);
    if (result != FLUX_OK)
        return result;
    result = export_surface_memory_fd(s, slot, out_fd);
    if (result != FLUX_OK) {
        close(*out_sync_fd);
        *out_sync_fd = -1;
        return result;
    }
    return FLUX_OK;
}
#else  /* !FLUX_HAVE_DMABUF */
flux_result flux_surface_export_dmabuf(flux_surface *s, int *out_fd) {
    if (!s || !out_fd)
        return FLUX_ERROR_INVALID_ARGUMENT;
    *out_fd = -1;
    FLUX_FAIL(FLUX_ERROR_UNSUPPORTED, "dma-buf export is only available on Linux");
    return FLUX_ERROR_UNSUPPORTED;
}

flux_result flux_surface_export_dmabuf_explicit(flux_surface *s, int *out_fd, int *out_sync_fd) {
    if (!s || !out_fd || !out_sync_fd)
        return FLUX_ERROR_INVALID_ARGUMENT;
    *out_fd = -1;
    *out_sync_fd = -1;
    FLUX_FAIL(FLUX_ERROR_UNSUPPORTED, "dma-buf export is only available on Linux");
    return FLUX_ERROR_UNSUPPORTED;
}
#endif /* FLUX_HAVE_DMABUF */

/* ------------------------------------------------------------------ */
/*  Raw VK accessors (vulkan.h)                                       */
/* ------------------------------------------------------------------ */

VkSurfaceKHR flux_surface_vk_handle(const flux_surface *s) {
    return s ? s->vk_surface : VK_NULL_HANDLE;
}
VkSwapchainKHR flux_surface_vk_swapchain(const flux_surface *s) {
    return s ? s->swapchain : VK_NULL_HANDLE;
}
VkFormat flux_surface_vk_format(const flux_surface *s) {
    return s ? s->format : VK_FORMAT_UNDEFINED;
}
