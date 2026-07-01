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

#include <errno.h>
#include <unistd.h>

/* FLUX_DRM_FORMAT_MOD_LINEAR == 0 (see drm_fourcc.h). We avoid the libdrm header
 * dependency here because the modifier value is all we need for export, and
 * dmabuf.c (the import side) already keeps the same convention. */
#define FLUX_DRM_FORMAT_MOD_LINEAR 0ULL
#include <flux/vulkan.h>

#include <stdlib.h>
#include <string.h>

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

/* Destroy + recreate every per-frame binary semaphore. A binary
 * semaphore signalled by an acquire whose submit was discarded by
 * VK_ERROR_OUT_OF_DATE_KHR remains signalled with no consumer; the
 * next acquire that signals it is undefined behaviour. The only
 * portable cure is to recycle the semaphores when the swapchain is
 * recreated. Caller must have already waited the device idle.
 * Returns true on success; false if any semaphore creation failed. */
static bool reset_frame_semaphores(flux_surface *s) {
    VkDevice vkd = s->device->device;
    VkSemaphoreCreateInfo sem = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    for (uint32_t i = 0; i < s->frames_in_flight; ++i) {
        flux_per_frame *f = &s->frames[i];
        if (f->image_acquired)
            vkDestroySemaphore(vkd, f->image_acquired, nullptr);
        if (f->render_finished)
            vkDestroySemaphore(vkd, f->render_finished, nullptr);
        f->image_acquired = VK_NULL_HANDLE;
        f->render_finished = VK_NULL_HANDLE;

        VkResult vr = vkCreateSemaphore(vkd, &sem, nullptr, &f->image_acquired);
        if (vr != VK_SUCCESS) {
            FLUX_FAIL_VK(FLUX_ERROR_BACKEND_FAILURE, "vkCreateSemaphore (image_acquired) failed",
                         vr);
            return false;
        }
        vr = vkCreateSemaphore(vkd, &sem, nullptr, &f->render_finished);
        if (vr != VK_SUCCESS) {
            FLUX_FAIL_VK(FLUX_ERROR_BACKEND_FAILURE, "vkCreateSemaphore (render_finished) failed",
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

    uint32_t image_count = caps.minImageCount + 1;
    if (caps.maxImageCount > 0 && image_count > caps.maxImageCount)
        image_count = caps.maxImageCount;
    if (image_count > FLUX_MAX_SWAPCHAIN_IMAGES)
        image_count = FLUX_MAX_SWAPCHAIN_IMAGES;

    VkSwapchainCreateInfoKHR sci = {
        .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
        .surface = s->vk_surface,
        .minImageCount = image_count,
        .imageFormat = fmt.format,
        .imageColorSpace = fmt.colorSpace,
        .imageExtent = extent,
        .imageArrayLayers = 1,
        .imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
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

    /* Destroy any prior image views before swapchain handle. */
    for (uint32_t i = 0; i < s->image_count; ++i) {
        if (s->image_views[i])
            vkDestroyImageView(s->device->device, s->image_views[i], nullptr);
        s->image_views[i] = VK_NULL_HANDLE;
        s->images[i] = VK_NULL_HANDLE;
    }
    if (s->swapchain)
        vkDestroySwapchainKHR(s->device->device, s->swapchain, nullptr);

    s->swapchain = new_swapchain;
    s->format = fmt.format;
    s->color_space = fmt.colorSpace;
    s->extent = extent;
    s->hdr_actual = (fmt.colorSpace != VK_COLOR_SPACE_SRGB_NONLINEAR_KHR);

    uint32_t got = 0;
    vr = vkGetSwapchainImagesKHR(s->device->device, s->swapchain, &got, nullptr);
    if (vr != VK_SUCCESS || got == 0) {
        FLUX_FAIL_VK(FLUX_ERROR_BACKEND_FAILURE, "vkGetSwapchainImagesKHR failed", vr);
        return FLUX_ERROR_BACKEND_FAILURE;
    }
    if (got > FLUX_MAX_SWAPCHAIN_IMAGES)
        got = FLUX_MAX_SWAPCHAIN_IMAGES;
    s->image_count = got;
    vr = vkGetSwapchainImagesKHR(s->device->device, s->swapchain, &got, s->images);
    if (vr != VK_SUCCESS) {
        FLUX_FAIL_VK(FLUX_ERROR_BACKEND_FAILURE, "vkGetSwapchainImagesKHR failed", vr);
        return FLUX_ERROR_BACKEND_FAILURE;
    }

    for (uint32_t i = 0; i < s->image_count; ++i) {
        VkImageViewCreateInfo ivci = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .image = s->images[i],
            .viewType = VK_IMAGE_VIEW_TYPE_2D,
            .format = s->format,
            .subresourceRange =
                {
                    .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                    .baseMipLevel = 0,
                    .levelCount = 1,
                    .baseArrayLayer = 0,
                    .layerCount = 1,
                },
        };
        vr = vkCreateImageView(s->device->device, &ivci, nullptr, &s->image_views[i]);
        if (vr != VK_SUCCESS) {
            FLUX_FAIL_VK(FLUX_ERROR_BACKEND_FAILURE, "vkCreateImageView failed", vr);
            return FLUX_ERROR_BACKEND_FAILURE;
        }
    }

    return FLUX_OK;
}

/* ------------------------------------------------------------------ */
/*  Offscreen images (ADR-0013)                                       */
/* ------------------------------------------------------------------ */

static void offscreen_destroy_images(flux_surface *s) {
    for (uint32_t i = 0; i < s->image_count; ++i) {
        if (s->image_views[i])
            vkDestroyImageView(s->device->device, s->image_views[i], nullptr);
        if (s->images[i])
            vkDestroyImage(s->device->device, s->images[i], nullptr);
        if (s->image_allocs[i].memory)
            flux_vk_deallocate(s->device, &s->image_allocs[i]);
        s->image_views[i] = VK_NULL_HANDLE;
        s->images[i] = VK_NULL_HANDLE;
        s->image_allocs[i] = (flux_vk_alloc){0};
    }
    s->image_count = 0;
}

/* One color image per frame slot; image index == frame slot, so the
 * per-slot fence already serialises reuse. */
static flux_result offscreen_create_images(flux_surface *s, uint32_t w, uint32_t h) {
    if (w == 0 || h == 0) {
        FLUX_FAIL(FLUX_ERROR_INVALID_ARGUMENT, "offscreen surface needs non-zero width and height");
        return FLUX_ERROR_INVALID_ARGUMENT;
    }

    offscreen_destroy_images(s);

    s->format = VK_FORMAT_B8G8R8A8_UNORM;
    s->color_space = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    s->extent = (VkExtent2D){w, h};
    s->hdr_actual = false;

    /* Decide whether offscreen images should be exportable as dma-buf. This
     * requires the device's external-memory / DRM-modifier extensions and a
     * linear-tiling modifier that supports colour-attachment + transfer. When
     * the host wants zero-copy presentation via linux-dmabuf, it enables the
     * extensions at flux_device_create and we pick the exportable path here.
     * Otherwise we fall back to the original OPTIMAL-tiling slab-allocated
     * image (read back via flux_surface_read_pixels). */
    flux_device *d = s->device;
    bool want_export = flux_dmabuf_supported(d);
    s->offscreen_exportable = false;
    s->offscreen_modifier = 0;
    s->offscreen_stride = 0;

    uint64_t chosen_modifier = 0;
    bool use_modifier = false;
    if (want_export) {
        /* Query which modifiers support rendering + readback for BGRA8. We
         * prefer LINEAR (portable, single-plane, always present on the
         * export side); fall back to the first supported modifier. */
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
                bool found_linear = false;
                bool found_any = false;
                for (uint32_t i = 0; i < list.drmFormatModifierCount; ++i) {
                    if ((mods[i].drmFormatModifierTilingFeatures & need) != need)
                        continue;
                    if (mods[i].drmFormatModifier == FLUX_DRM_FORMAT_MOD_LINEAR) {
                        found_linear = true;
                        break;
                    }
                    if (!found_any) {
                        chosen_modifier = mods[i].drmFormatModifier;
                        found_any = true;
                    }
                }
                if (found_linear)
                    chosen_modifier = FLUX_DRM_FORMAT_MOD_LINEAR;
                use_modifier = found_linear || found_any;
                flux_internal_free(d, mods);
            }
        }
    }

    if (use_modifier) {
        /* Validate the chosen modifier is actually exportable as a dma-buf. */
        VkPhysicalDeviceExternalImageFormatInfo eifi = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO,
            .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT_KHR,
        };
        VkPhysicalDeviceImageDrmFormatModifierInfoEXT mfi = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_DRM_FORMAT_MODIFIER_INFO_EXT,
            .pNext = &eifi,
            .drmFormatModifier = chosen_modifier,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        };
        VkPhysicalDeviceImageFormatInfo2 ifi = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2,
            .pNext = &mfi,
            .format = s->format,
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
        bool exportable = (vr == VK_SUCCESS) &&
                          (efp.externalMemoryProperties.externalMemoryFeatures &
                           VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT) != 0;
        if (!exportable)
            use_modifier = false;
    }

    s->offscreen_exportable = use_modifier;
    if (use_modifier)
        s->offscreen_modifier = chosen_modifier;

    /* Image create-info. The modifier path chains an external-memory + DRM-
     * modifier pair; the fallback path uses plain OPTIMAL tiling. */
    VkExternalMemoryImageCreateInfo ext_mem = {
        .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
        .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT_KHR,
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
        .tiling = use_modifier ? VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT
                               : VK_IMAGE_TILING_OPTIMAL,
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

            VkMemoryDedicatedAllocateInfo dedicated = {
                .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
                .image = s->images[i],
            };
            VkMemoryRequirements2 mreq = {.sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2};
            VkImageMemoryRequirementsInfo2 mri = {
                .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2,
                .image = s->images[i],
            };
            vkGetImageMemoryRequirements2(d->device, &mri, &mreq);
            uint32_t mt = flux_vk_find_memory_type(
                d, mreq.memoryRequirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
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
            flux_vk_allocator *a = &d->mem_allocator;
            pthread_mutex_lock(&a->lock);
            a->bytes_in_use += mreq.memoryRequirements.size;
            a->bytes_reserved += mreq.memoryRequirements.size;
            a->live_allocations++;
            pthread_mutex_unlock(&a->lock);

            /* Record the stride from memory-plane 0. For DRM-modifier images
             * the aspect must be VK_IMAGE_ASPECT_MEMORY_PLANE_i_BIT_EXT, not
             * COLOR — the memory-plane layout is what the dma-buf consumer
             * sees. BGRA8 is single-plane, so plane 0 is the only one. */
            if (s->offscreen_stride == 0) {
                VkImageSubresource plane0 = {
                    .aspectMask = VK_IMAGE_ASPECT_MEMORY_PLANE_0_BIT_EXT,
                };
                VkSubresourceLayout layout;
                vkGetImageSubresourceLayout(d->device, s->images[i], &plane0, &layout);
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
    }
    return FLUX_OK;
}

void flux_surface_destroy_swapchain(flux_surface *s) {
    if (!s || !s->device || !s->device->device)
        return;
    for (uint32_t i = 0; i < s->image_count; ++i) {
        if (s->image_views[i])
            vkDestroyImageView(s->device->device, s->image_views[i], nullptr);
        s->image_views[i] = VK_NULL_HANDLE;
        s->images[i] = VK_NULL_HANDLE;
    }
    s->image_count = 0;
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
        .commandBufferCount = 1,
    };
    vr = vkAllocateCommandBuffers(s->device->device, &cbai, &f->cmd);
    if (vr != VK_SUCCESS) {
        FLUX_FAIL_VK(FLUX_ERROR_BACKEND_FAILURE, "vkAllocateCommandBuffers failed", vr);
        goto fail;
    }

    VkSemaphoreCreateInfo sem = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    vr = vkCreateSemaphore(s->device->device, &sem, nullptr, &f->image_acquired);
    if (vr != VK_SUCCESS) {
        FLUX_FAIL_VK(FLUX_ERROR_BACKEND_FAILURE, "vkCreateSemaphore failed", vr);
        goto fail;
    }
    vr = vkCreateSemaphore(s->device->device, &sem, nullptr, &f->render_finished);
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
    if (f->query_pool)
        vkDestroyQueryPool(d, f->query_pool, nullptr);
    if (f->in_flight)
        vkDestroyFence(d, f->in_flight, nullptr);
    if (f->render_finished)
        vkDestroySemaphore(d, f->render_finished, nullptr);
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
    s->vsync = desc->vsync;
    s->hdr_preferred = desc->hdr_preferred;
    s->last_submitted_slot = UINT32_MAX;
    s->frames_in_flight = device->frames_in_flight;
    if (s->frames_in_flight > FLUX_MAX_FRAMES_IN_FLIGHT)
        s->frames_in_flight = FLUX_MAX_FRAMES_IN_FLIGHT;

    flux_result r;
    if (s->offscreen) {
        r = offscreen_create_images(s, desc->width, desc->height);
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
    flux_device_release(s->device);
    flux_internal_free(device, s);
    return r;
}

flux_surface *flux_surface_retain(flux_surface *s) {
    if (s)
        atomic_fetch_add_explicit(&s->ref_count, 1u, memory_order_relaxed);
    return s;
}

void flux_surface_release(flux_surface *s) {
    if (!s)
        return;
    if (atomic_fetch_sub_explicit(&s->ref_count, 1u, memory_order_acq_rel) != 1u)
        return;

    vkDeviceWaitIdle(s->device->device);
    flux_transient_ring_destroy(&s->transient, s->device);
    for (uint32_t i = 0; i < s->frames_in_flight; ++i)
        destroy_per_frame(s, i);
    if (s->offscreen)
        offscreen_destroy_images(s);
    else
        flux_surface_destroy_swapchain(s);
    flux_device *dev = s->device;
    flux_internal_free(dev, s);
    flux_device_release(dev);
}

flux_result flux_surface_resize(flux_surface *s, uint32_t w, uint32_t h) {
    if (!s)
        return FLUX_ERROR_INVALID_ARGUMENT;

    vkDeviceWaitIdle(s->device->device);
    if (s->offscreen) {
        s->current_frame = 0;
        s->last_submitted_slot = UINT32_MAX; /* old contents are gone */
        return offscreen_create_images(s, w, h);
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
/*  Offscreen readback (ADR-0013)                                     */
/* ------------------------------------------------------------------ */

flux_result flux_surface_read_pixels(flux_surface *s, void *dst, size_t bytes) {
    if (!s || !dst)
        return FLUX_ERROR_INVALID_ARGUMENT;
    if (!s->offscreen) {
        FLUX_FAIL(FLUX_ERROR_UNSUPPORTED, "flux_surface_read_pixels needs an offscreen surface");
        return FLUX_ERROR_UNSUPPORTED;
    }
    if (s->last_submitted_slot == UINT32_MAX) {
        FLUX_FAIL(FLUX_ERROR_INVALID_STATE, "no frame has been submitted to read back");
        return FLUX_ERROR_INVALID_STATE;
    }
    size_t needed = (size_t)s->extent.width * s->extent.height * 4u;
    if (bytes < needed) {
        FLUX_FAIL(FLUX_ERROR_INVALID_ARGUMENT, "dst too small for width * height * 4 bytes");
        return FLUX_ERROR_INVALID_ARGUMENT;
    }

    flux_device *d = s->device;
    uint32_t slot = s->last_submitted_slot;
    flux_per_frame *pf = &s->frames[slot];

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

    VkBuffer staging = VK_NULL_HANDLE;
    flux_vk_alloc staging_alloc = {0};
    flux_result r = flux_vk_alloc_buffer(d, needed, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                         VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                             VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                                         /*wants_device_address=*/false, &staging, &staging_alloc);
    if (r != FLUX_OK)
        return r;

    if (!staging_alloc.mapped) {
        FLUX_FAIL(FLUX_ERROR_BACKEND_FAILURE,
                  "readback staging allocation came back un-mapped (allocator invariant violated)");
        r = FLUX_ERROR_BACKEND_FAILURE;
        goto out;
    }

    {
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
        vkCmdCopyImageToBuffer(cmd, s->images[slot], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, staging,
                               1, &region);

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
        const uint8_t *src = (const uint8_t *)staging_alloc.mapped;
        uint8_t *out_px = (uint8_t *)dst;
        for (size_t i = 0; i < (size_t)s->extent.width * s->extent.height; ++i) {
            out_px[i * 4u + 0u] = src[i * 4u + 2u];
            out_px[i * 4u + 1u] = src[i * 4u + 1u];
            out_px[i * 4u + 2u] = src[i * 4u + 0u];
            out_px[i * 4u + 3u] = src[i * 4u + 3u];
        }
    } else {
        memcpy(dst, staging_alloc.mapped, needed);
    }
    r = FLUX_OK;

out:
    if (staging)
        vkDestroyBuffer(d->device, staging, nullptr);
    if (staging_alloc.memory)
        flux_vk_deallocate(d, &staging_alloc);
    return r;
}

/* ------------------------------------------------------------------ */
/*  Offscreen dma-buf export (ADR-0040 follow-on)                     */
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

    PFN_vkGetMemoryFdKHR pGetMemoryFd =
        (PFN_vkGetMemoryFdKHR)vkGetDeviceProcAddr(d->device, "vkGetMemoryFdKHR");
    if (!pGetMemoryFd) {
        FLUX_FAIL(FLUX_ERROR_UNSUPPORTED, "vkGetMemoryFdKHR entry point missing");
        return FLUX_ERROR_UNSUPPORTED;
    }

    VkMemoryGetFdInfoKHR fd_info = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR,
        .memory = s->image_allocs[slot].memory,
        .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT_KHR,
    };
    int fd = -1;
    vr = pGetMemoryFd(d->device, &fd_info, &fd);
    if (vr != VK_SUCCESS) {
        FLUX_FAIL_VK(FLUX_ERROR_BACKEND_FAILURE, "vkGetMemoryFdKHR failed", vr);
        return FLUX_ERROR_BACKEND_FAILURE;
    }

    *out_fd = fd;
    return FLUX_OK;
}

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
