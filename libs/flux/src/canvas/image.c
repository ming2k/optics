#include "internal.h"

#include <string.h>

static uint32_t bytes_per_pixel(flux_format f) {
    switch (f) {
    case FLUX_FORMAT_R8_UNORM:
        return 1;
    case FLUX_FORMAT_RGBA8_UNORM:
    case FLUX_FORMAT_BGRA8_UNORM:
    case FLUX_FORMAT_RGBA8_SRGB:
    case FLUX_FORMAT_BGRA8_SRGB:
        return 4;
    default:
        return 0;
    }
}

flux_result flux_image_create(flux_device *d, const flux_image_desc *desc, flux_image **out) {
    if (!d || !desc || !out)
        return FLUX_ERROR_INVALID_ARGUMENT;
    if (desc->type != FLUX_TYPE_IMAGE_DESC) {
        FLUX_FAIL(FLUX_ERROR_INVALID_ARGUMENT, "desc->type != FLUX_TYPE_IMAGE_DESC");
        return FLUX_ERROR_INVALID_ARGUMENT;
    }
    if (desc->width == 0 || desc->height == 0) {
        FLUX_FAIL(FLUX_ERROR_INVALID_ARGUMENT, "image zero-extent");
        return FLUX_ERROR_INVALID_ARGUMENT;
    }
    VkFormat vfmt = flux_format_to_vk(desc->format);
    uint32_t bpp = bytes_per_pixel(desc->format);
    if (vfmt == VK_FORMAT_UNDEFINED || bpp == 0) {
        FLUX_FAIL(FLUX_ERROR_UNSUPPORTED, "unsupported pixel format");
        return FLUX_ERROR_UNSUPPORTED;
    }
    *out = nullptr;

    flux_image *im = flux_internal_alloc(d, sizeof(*im));
    if (!im)
        return FLUX_ERROR_OUT_OF_MEMORY;
    atomic_init(&im->ref_count, 1u);
    im->device = flux_device_retain(d);
    im->width = desc->width;
    im->height = desc->height;
    im->format = desc->format;
    im->bindless = FLUX_BINDLESS_INVALID;
    im->bindless_storage = FLUX_BINDLESS_INVALID;
    im->current_layout = VK_IMAGE_LAYOUT_UNDEFINED;

    /* Image */
    VkImageCreateInfo ici = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = vfmt,
        .extent = {desc->width, desc->height, 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    flux_result r =
        flux_vk_alloc_image(d, &ici, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &im->image, &im->alloc);
    if (r != FLUX_OK)
        goto fail;
    VkResult vr;

    if (desc->initial_data) {
        size_t bytes = (size_t)desc->width * desc->height * bpp;
        r = flux_vk_upload_to_image(d, im->image, 0, 0, desc->width, desc->height,
                                    VK_IMAGE_LAYOUT_UNDEFINED, desc->initial_data, bytes);
        if (r != FLUX_OK)
            goto fail;
    } else {
        /* No initial data — transition to SHADER_READ_ONLY_OPTIMAL so
         * the sampled view is immediately usable. */
        r = flux_vk_transition_image_layout(d, im->image, VK_IMAGE_LAYOUT_UNDEFINED,
                                            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        if (r != FLUX_OK)
            goto fail;
    }
    im->current_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

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
        FLUX_FAIL_VK(FLUX_ERROR_BACKEND_FAILURE, "vkCreateImageView failed", vr);
        goto fail;
    }

    r = flux_bindless_register_image(d, im->view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                     &im->bindless);
    if (r != FLUX_OK)
        goto fail;

    *out = im;
    return FLUX_OK;

fail:
    /* Match compute_writable's fail path: release any bindless slots
     * that were registered before the failure. Robust against future
     * steps being inserted between register and return. */
    if (im->bindless != FLUX_BINDLESS_INVALID)
        flux_bindless_release(d, im->bindless);
    if (im->bindless_storage != FLUX_BINDLESS_INVALID)
        flux_bindless_release(d, im->bindless_storage);
    if (im->view)
        vkDestroyImageView(d->device, im->view, nullptr);
    if (im->image)
        vkDestroyImage(d->device, im->image, nullptr);
    if (im->alloc.memory)
        flux_vk_deallocate(d, &im->alloc);
    flux_device_release(d);
    flux_internal_free(d, im);
    return r;
}

flux_image *flux_image_retain(flux_image *im) {
    if (im)
        atomic_fetch_add_explicit(&im->ref_count, 1u, memory_order_relaxed);
    return im;
}

flux_result flux_image_update_region(flux_image *im, uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                                     const void *data, size_t bytes) {
    if (!im || !data)
        return FLUX_ERROR_INVALID_ARGUMENT;
    if (w == 0 || h == 0)
        return FLUX_ERROR_INVALID_ARGUMENT;
    if (x > im->width || y > im->height || x + w > im->width || y + h > im->height) {
        FLUX_FAIL(FLUX_ERROR_OUT_OF_RANGE, "update region exceeds image extent");
        return FLUX_ERROR_OUT_OF_RANGE;
    }
    uint32_t bpp = bytes_per_pixel(im->format);
    if (bpp == 0) {
        FLUX_FAIL(FLUX_ERROR_UNSUPPORTED, "image has no known bytes-per-pixel");
        return FLUX_ERROR_UNSUPPORTED;
    }
    size_t needed = (size_t)w * h * bpp;
    if (bytes < needed) {
        FLUX_FAIL(FLUX_ERROR_INVALID_ARGUMENT, "update data too small for w*h*bpp");
        return FLUX_ERROR_INVALID_ARGUMENT;
    }

    return flux_vk_upload_to_image(im->device, im->image, (int32_t)x, (int32_t)y, w, h,
                                   VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, data, needed);
}

VkImage flux_image_vk_image(const flux_image *im) {
    return im ? im->image : VK_NULL_HANDLE;
}
VkImageView flux_image_vk_image_view(const flux_image *im) {
    return im ? im->view : VK_NULL_HANDLE;
}
flux_bindless_handle flux_image_bindless_handle(const flux_image *im) {
    return im ? im->bindless : FLUX_BINDLESS_INVALID;
}

void flux_image_release(flux_image *im) {
    if (!im)
        return;
    if (atomic_fetch_sub_explicit(&im->ref_count, 1u, memory_order_acq_rel) != 1u)
        return;
    flux_device *d = im->device;
    if (im->bindless != FLUX_BINDLESS_INVALID)
        flux_bindless_release(d, im->bindless);
    if (im->bindless_storage != FLUX_BINDLESS_INVALID)
        flux_bindless_release(d, im->bindless_storage);
    if (im->view)
        vkDestroyImageView(d->device, im->view, nullptr);
    if (im->image)
        vkDestroyImage(d->device, im->image, nullptr);
    if (im->alloc.memory)
        flux_vk_deallocate(d, &im->alloc);
    if (im->imported_memory)
        vkFreeMemory(d->device, im->imported_memory, nullptr);
    bool weak = im->device_weak;
    flux_internal_free(d, im);
    if (!weak)
        flux_device_release(d);
}

flux_result flux_image_create_compute_writable(flux_device *d, uint32_t width, uint32_t height,
                                               flux_format fmt, flux_image **out) {
    if (!d || !out)
        return FLUX_ERROR_INVALID_ARGUMENT;
    if (width == 0 || height == 0) {
        FLUX_FAIL(FLUX_ERROR_INVALID_ARGUMENT, "compute-writable image zero-extent");
        return FLUX_ERROR_INVALID_ARGUMENT;
    }
    VkFormat vfmt = flux_format_to_vk(fmt);
    if (vfmt == VK_FORMAT_UNDEFINED) {
        FLUX_FAIL(FLUX_ERROR_UNSUPPORTED, "compute-writable image unsupported format");
        return FLUX_ERROR_UNSUPPORTED;
    }
    /* Storage-image access requires a UNORM-class format. sRGB views
     * over storage are not portable; reject early to avoid driver
     * surprises. RGBA8/BGRA8 UNORM are guaranteed by the spec for
     * STORAGE_IMAGE without format features. */
    if (fmt != FLUX_FORMAT_RGBA8_UNORM && fmt != FLUX_FORMAT_BGRA8_UNORM) {
        FLUX_FAIL(FLUX_ERROR_UNSUPPORTED, "compute-writable image format not storage-compatible "
                                          "(use RGBA8_UNORM or BGRA8_UNORM)");
        return FLUX_ERROR_UNSUPPORTED;
    }
    *out = nullptr;

    flux_image *im = flux_internal_alloc(d, sizeof(*im));
    if (!im)
        return FLUX_ERROR_OUT_OF_MEMORY;
    atomic_init(&im->ref_count, 1u);
    /* Effect-pool transients are owned by the device's effect state
     * and released by effect_state_destroy inside flux_device_release;
     * a strong device ref here would cycle and keep the refcount from
     * ever reaching zero (the whole device would leak). */
    im->device = d;
    im->device_weak = true;
    im->width = width;
    im->height = height;
    im->format = fmt;
    im->bindless = FLUX_BINDLESS_INVALID;
    im->bindless_storage = FLUX_BINDLESS_INVALID;
    im->current_layout = VK_IMAGE_LAYOUT_UNDEFINED;

    VkImageCreateInfo ici = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = vfmt,
        .extent = {width, height, 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                 VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    flux_result r =
        flux_vk_alloc_image(d, &ici, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &im->image, &im->alloc);
    if (r != FLUX_OK)
        goto fail;

    /* Compute writes happen in GENERAL; sampling also works from
     * GENERAL (at the cost of some driver-side optimisations the
     * SAMPLED_OPTIMAL layout enables). The transient target is
     * short-lived and re-used; GENERAL is the simpler choice. */
    r = flux_vk_transition_image_layout(d, im->image, VK_IMAGE_LAYOUT_UNDEFINED,
                                        VK_IMAGE_LAYOUT_GENERAL);
    if (r != FLUX_OK)
        goto fail;
    im->current_layout = VK_IMAGE_LAYOUT_GENERAL;

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
    VkResult vr = vkCreateImageView(d->device, &ivci, nullptr, &im->view);
    if (vr != VK_SUCCESS) {
        FLUX_FAIL_VK(FLUX_ERROR_BACKEND_FAILURE, "vkCreateImageView failed", vr);
        r = FLUX_ERROR_BACKEND_FAILURE;
        goto fail;
    }

    r = flux_bindless_register_image(d, im->view, VK_IMAGE_LAYOUT_GENERAL, &im->bindless);
    if (r != FLUX_OK)
        goto fail;

    r = flux_bindless_register_storage_image(d, im->view, VK_IMAGE_LAYOUT_GENERAL,
                                             &im->bindless_storage);
    if (r != FLUX_OK)
        goto fail;

    *out = im;
    return FLUX_OK;

fail:
    if (im->bindless != FLUX_BINDLESS_INVALID)
        flux_bindless_release(d, im->bindless);
    if (im->bindless_storage != FLUX_BINDLESS_INVALID)
        flux_bindless_release(d, im->bindless_storage);
    if (im->view)
        vkDestroyImageView(d->device, im->view, nullptr);
    if (im->image)
        vkDestroyImage(d->device, im->image, nullptr);
    if (im->alloc.memory)
        flux_vk_deallocate(d, &im->alloc);
    flux_internal_free(d, im);
    return r;
}

/* ------------------------------------------------------------------ */
/*  Render-target image (ADR-0017)                                    */
/* ------------------------------------------------------------------ */

/* Create an image suitable as a flux_canvas_begin_target destination:
 * COLOR_ATTACHMENT | SAMPLED | TRANSFER_DST, 1 sample, transitioned to
 * SHADER_READ_ONLY_OPTIMAL so it is immediately sampleable. Strong device
 * ref (caller-owned, like flux_image_create — not the effect-pool weak ref). */
flux_result flux_image_create_render_target(flux_device *d, uint32_t width, uint32_t height,
                                            flux_format fmt, flux_image **out) {
    if (!d || !out)
        return FLUX_ERROR_INVALID_ARGUMENT;
    if (width == 0 || height == 0) {
        FLUX_FAIL(FLUX_ERROR_INVALID_ARGUMENT, "render-target image zero-extent");
        return FLUX_ERROR_INVALID_ARGUMENT;
    }
    VkFormat vfmt = flux_format_to_vk(fmt);
    if (vfmt == VK_FORMAT_UNDEFINED) {
        FLUX_FAIL(FLUX_ERROR_UNSUPPORTED, "render-target image unsupported format");
        return FLUX_ERROR_UNSUPPORTED;
    }
    *out = nullptr;

    flux_image *im = flux_internal_alloc(d, sizeof(*im));
    if (!im)
        return FLUX_ERROR_OUT_OF_MEMORY;
    atomic_init(&im->ref_count, 1u);
    im->device = flux_device_retain(d);
    im->device_weak = false;
    im->width = width;
    im->height = height;
    im->format = fmt;
    im->bindless = FLUX_BINDLESS_INVALID;
    im->bindless_storage = FLUX_BINDLESS_INVALID;
    im->current_layout = VK_IMAGE_LAYOUT_UNDEFINED;

    VkImageCreateInfo ici = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = vfmt,
        .extent = {width, height, 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                 VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    flux_result r =
        flux_vk_alloc_image(d, &ici, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &im->image, &im->alloc);
    if (r != FLUX_OK)
        goto fail;

    r = flux_vk_transition_image_layout(d, im->image, VK_IMAGE_LAYOUT_UNDEFINED,
                                        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    if (r != FLUX_OK)
        goto fail;
    im->current_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

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
    VkResult vr = vkCreateImageView(d->device, &ivci, nullptr, &im->view);
    if (vr != VK_SUCCESS) {
        FLUX_FAIL_VK(FLUX_ERROR_BACKEND_FAILURE, "vkCreateImageView failed", vr);
        r = FLUX_ERROR_BACKEND_FAILURE;
        goto fail;
    }

    r = flux_bindless_register_image(d, im->view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                     &im->bindless);
    if (r != FLUX_OK)
        goto fail;

    *out = im;
    return FLUX_OK;

fail:
    if (im->bindless != FLUX_BINDLESS_INVALID)
        flux_bindless_release(d, im->bindless);
    if (im->bindless_storage != FLUX_BINDLESS_INVALID)
        flux_bindless_release(d, im->bindless_storage);
    if (im->view)
        vkDestroyImageView(d->device, im->view, nullptr);
    if (im->image)
        vkDestroyImage(d->device, im->image, nullptr);
    if (im->alloc.memory)
        flux_vk_deallocate(d, &im->alloc);
    flux_device_release(d);
    flux_internal_free(d, im);
    return r;
}
