#include "../core/image_internal.h"

#include <stdio.h>
#include <string.h>

/* ADR-0070: GPU parameters for non-default content color spaces,
 * consumed by canvas_image.frag through a buffer device address.
 * std430 layout — keep in sync with the shader. */
typedef struct flux_image_color_params {
    float primaries[3][4]; /* content -> working space, column-major mat3 */
    uint32_t transfer;     /* flux_transfer_func of the content */
    float gamma;           /* FLUX_TRANSFER_GAMMA exponent */
    uint32_t lut_handle;   /* bindless 3D LUT, or FLUX_BINDLESS_INVALID */
    uint32_t lut_size;
} flux_image_color_params;

/* The content space an untagged image is interpreted as (ADR-0069). */
static flux_color_space image_default_space(flux_format f) {
    if (f == FLUX_FORMAT_RGBA16_SFLOAT)
        return (flux_color_space)FLUX_COLOR_SPACE_SCRGB;
    return (flux_color_space)FLUX_COLOR_SPACE_SRGB;
}

/* Create the 3D LUT image for a baked ICC profile: stored R-fastest in
 * the profile, re-laid as a 2D (N² × N) image so the stock 2D bindless
 * heap serves it; the shader does the slice lerp. RGBA32F keeps the
 * upload a plain widening of the baked floats. */
static flux_result image_create_lut(flux_device *d, flux_image *im, const float *lut, uint32_t n) {
    uint32_t w = n * n, h = n;
    size_t texel_count = (size_t)w * h;
    float *rgba = flux_internal_alloc(d, texel_count * 4 * sizeof(float));
    if (!rgba)
        return FLUX_ERROR_OUT_OF_MEMORY;
    for (size_t i = 0; i < texel_count; ++i) {
        rgba[i * 4 + 0] = lut[i * 3 + 0];
        rgba[i * 4 + 1] = lut[i * 3 + 1];
        rgba[i * 4 + 2] = lut[i * 3 + 2];
        rgba[i * 4 + 3] = 1.0f;
    }

    flux_result r = FLUX_OK;
    VkImageCreateInfo ici = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = VK_FORMAT_R32G32B32A32_SFLOAT,
        .extent = {w, h, 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    im->lut_bindless = FLUX_BINDLESS_INVALID;
    r = flux_vk_alloc_image(d, &ici, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &im->lut_image,
                            &im->lut_alloc);
    if (r != FLUX_OK)
        goto out_free;
    r = flux_vk_upload_to_image(d, im->lut_image, 0, 0, w, h, VK_IMAGE_LAYOUT_UNDEFINED, rgba,
                                texel_count * 4 * sizeof(float));
    if (r != FLUX_OK)
        goto out_free;
    VkImageViewCreateInfo ivci = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = im->lut_image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = VK_FORMAT_R32G32B32A32_SFLOAT,
        .subresourceRange =
            {
                .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                .levelCount = 1,
                .layerCount = 1,
            },
    };
    if (vkCreateImageView(d->device, &ivci, nullptr, &im->lut_view) != VK_SUCCESS) {
        FLUX_FAIL(FLUX_ERROR_BACKEND_FAILURE, "ICC LUT view failed");
        r = FLUX_ERROR_BACKEND_FAILURE;
        goto out_free;
    }
    r = flux_bindless_register_image(d, im->lut_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                     &im->lut_bindless);

out_free:
    flux_internal_free(d, rgba);
    return r;
}

/* ADR-0069/0070: resolve the image's content color space from the
 * color-space desc on the creation desc's `next` chain (flux_image_desc
 * or flux_dmabuf_image_desc) and build the GPU-side parameter block when
 * the content leaves the format-derived fast path. */
flux_result flux_image_init_color(flux_device *d, flux_image *im, const void *next_chain) {
    flux_color_space content = image_default_space(im->format);
    const float *lut = nullptr;
    uint32_t lut_size = 0;
    bool tagged = false;

    const struct {
        flux_struct_type type;
        const void *next;
    } *extension = next_chain;
    while (extension) {
        if (extension->type == FLUX_TYPE_IMAGE_COLOR_SPACE_DESC) {
            const flux_image_color_space_desc *cs = (const void *)extension;
            if (cs->space) {
                if (!flux_color_space_is_valid(*cs->space)) {
                    FLUX_FAIL(FLUX_ERROR_INVALID_ARGUMENT, "image color space tag is invalid");
                    return FLUX_ERROR_INVALID_ARGUMENT;
                }
                content = *cs->space;
                tagged = true;
            }
            if (cs->icc) {
                flux_color_space extracted;
                /* Profiles are immutable apart from the atomic refcount,
                 * so retaining through the desc's const pointer is sound. */
                im->icc = flux_icc_profile_retain((flux_icc_profile *)cs->icc);
                if (flux_icc_profile_color_space(im->icc, &extracted)) {
                    content = extracted;
                } else {
                    lut = flux_icc_profile_lut(im->icc, &lut_size);
                    if (!lut) {
                        FLUX_FAIL(FLUX_ERROR_INVALID_STATE, "ICC profile carries no transform");
                        return FLUX_ERROR_INVALID_STATE;
                    }
                }
                tagged = true;
            }
        }
        extension = extension->next;
    }

    if (!tagged)
        return FLUX_OK;
    /* An explicit tag equal to the format default needs no GPU state. */
    if (!lut && flux_color_space_equal(content, image_default_space(im->format)))
        return FLUX_OK;

    if (lut) {
        flux_result r = image_create_lut(d, im, lut, lut_size);
        if (r != FLUX_OK)
            return r;
    }

    flux_image_color_params params = {0};
    flux_mat3 to_working;
    flux_color_space working = FLUX_COLOR_SPACE_SCRGB;
    flux_color_space_transform_matrix(content, working, &to_working);
    for (int col = 0; col < 3; ++col) {
        for (int row = 0; row < 3; ++row)
            params.primaries[col][row] = to_working.m[col * 3 + row];
        params.primaries[col][3] = 0.0f;
    }
    params.transfer = (uint32_t)content.transfer;
    params.gamma = content.gamma;
    params.lut_handle = im->lut_bindless;
    params.lut_size = lut_size;

    flux_buffer_desc bd = FLUX_BUFFER_DESC_INIT;
    bd.size = sizeof(params);
    bd.usage = FLUX_BUFFER_USAGE_STORAGE;
    bd.location = FLUX_BUFFER_HOST_VISIBLE;
    bd.device_address = true;
    bd.initial_data = &params;
    flux_result r = flux_buffer_create(d, &bd, &im->color_params);
    if (r != FLUX_OK)
        return r;
    im->color_params_address = flux_buffer_device_address(im->color_params);
    if (im->color_params_address == 0) {
        FLUX_FAIL(FLUX_ERROR_BACKEND_FAILURE, "color params buffer has no device address");
        return FLUX_ERROR_BACKEND_FAILURE;
    }
    return FLUX_OK;
}

static uint32_t bytes_per_pixel(flux_format f) {
    switch (f) {
    case FLUX_FORMAT_R8_UNORM:
        return 1;
    case FLUX_FORMAT_RGBA8_UNORM:
    case FLUX_FORMAT_BGRA8_UNORM:
    case FLUX_FORMAT_RGBA8_SRGB:
    case FLUX_FORMAT_BGRA8_SRGB:
        return 4;
    case FLUX_FORMAT_RGBA16_SFLOAT:
        return 8; /* ADR-0069 working-space content */
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
    im->lut_bindless = FLUX_BINDLESS_INVALID;
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
    {
        char name[80];
        snprintf(name, sizeof(name), "flux_image %ux%u fmt=%d", desc->width, desc->height,
                 (int)desc->format);
        flux_vk_set_name(d, VK_OBJECT_TYPE_IMAGE, (uint64_t)im->image, name);
    }
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

    r = flux_image_init_color(d, im, desc->next);
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
    if (im->lut_bindless != FLUX_BINDLESS_INVALID)
        flux_bindless_release(d, im->lut_bindless);
    if (im->lut_view)
        vkDestroyImageView(d->device, im->lut_view, nullptr);
    if (im->lut_image)
        vkDestroyImage(d->device, im->lut_image, nullptr);
    if (im->lut_alloc.memory)
        flux_vk_deallocate(d, &im->lut_alloc);
    if (im->color_params)
        flux_buffer_release(im->color_params);
    if (im->icc)
        flux_icc_profile_release(im->icc);
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

/* Shared validation for the update-region family. On success writes the
 * packed byte count the upload needs to `*packed_bytes` and returns FLUX_OK;
 * on failure reports through FLUX_FAIL and returns the error code.
 * `row_bytes` == 0 means the packed variant (stride derived from width);
 * otherwise it is the source row pitch and must be at least width * bpp. */
static flux_result update_region_validate(const flux_image *im, uint32_t x, uint32_t y, uint32_t w,
                                          uint32_t h, const void *data, size_t row_bytes,
                                          size_t bytes, size_t *packed_bytes) {
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
    size_t row_min = (size_t)w * bpp;
    size_t stride = row_bytes ? row_bytes : row_min;
    if (stride < row_min) {
        FLUX_FAIL(FLUX_ERROR_INVALID_ARGUMENT, "update row_bytes is smaller than width * bpp");
        return FLUX_ERROR_INVALID_ARGUMENT;
    }
    size_t needed = (h - 1) * stride + row_min;
    if (bytes < needed) {
        FLUX_FAIL(FLUX_ERROR_INVALID_ARGUMENT, "update data too small for the row stride");
        return FLUX_ERROR_INVALID_ARGUMENT;
    }
    /* The upload is recorded with old_layout = SHADER_READ_ONLY_OPTIMAL
     * (and transitions back to it). Anything else — a compute-writable
     * image in GENERAL, a render target mid-capture in
     * COLOR_ATTACHMENT_OPTIMAL — would record a barrier with the wrong
     * oldLayout (undefined contents), so refuse it loudly. */
    if (im->current_layout != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
        FLUX_FAIL(FLUX_ERROR_INVALID_STATE,
                  "image update requires SHADER_READ_ONLY_OPTIMAL current layout");
        return FLUX_ERROR_INVALID_STATE;
    }
    *packed_bytes = row_min * h;
    return FLUX_OK;
}

flux_result flux_image_update_region(flux_image *im, uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                                     const void *data, size_t bytes) {
    size_t needed;
    flux_result v = update_region_validate(im, x, y, w, h, data, 0, bytes, &needed);
    if (v != FLUX_OK)
        return v;

    return flux_vk_upload_to_image(im->device, im->image, (int32_t)x, (int32_t)y, w, h,
                                   VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, data, needed);
}

flux_result flux_image_update_region_strided(flux_image *im, uint32_t x, uint32_t y, uint32_t w,
                                             uint32_t h, const void *data, size_t row_bytes,
                                             size_t bytes) {
    size_t packed;
    flux_result v = update_region_validate(im, x, y, w, h, data, row_bytes, bytes, &packed);
    if (v != FLUX_OK)
        return v;

    size_t row_min = (size_t)w * bytes_per_pixel(im->format);
    if (row_bytes == 0 || row_bytes == row_min)
        return flux_vk_upload_to_image(im->device, im->image, (int32_t)x, (int32_t)y, w, h,
                                       VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, data, packed);

    /* Strided source: repack rows into a scratch buffer, then upload packed.
     * The scratch is sized to the tightly packed extent (not the strided
     * span) so the staging copy stays minimal. */
    void *scratch = flux_internal_alloc(im->device, packed);
    if (!scratch)
        return FLUX_ERROR_OUT_OF_MEMORY;
    const uint8_t *src = data;
    uint8_t *dst = scratch;
    for (uint32_t row = 0; row < h; ++row) {
        memcpy(dst, src, row_min);
        src += row_bytes;
        dst += row_min;
    }
    flux_result r =
        flux_vk_upload_to_image(im->device, im->image, (int32_t)x, (int32_t)y, w, h,
                                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, scratch, packed);
    flux_internal_free(im->device, scratch);
    return r;
}

flux_result flux_image_update_region_premultiply(flux_image *im, uint32_t x, uint32_t y, uint32_t w,
                                                 uint32_t h, const void *data, size_t row_bytes,
                                                 size_t bytes) {
    if (im && im->format != FLUX_FORMAT_RGBA8_UNORM) {
        FLUX_FAIL(FLUX_ERROR_UNSUPPORTED, "premultiply upload requires FLUX_FORMAT_RGBA8_UNORM");
        return FLUX_ERROR_UNSUPPORTED;
    }
    size_t packed;
    flux_result v = update_region_validate(im, x, y, w, h, data, row_bytes, bytes, &packed);
    if (v != FLUX_OK)
        return v;

    size_t stride = row_bytes ? row_bytes : (size_t)w * 4u;
    uint8_t *scratch = flux_internal_alloc(im->device, packed);
    if (!scratch)
        return FLUX_ERROR_OUT_OF_MEMORY;
    const uint8_t *src = data;
    uint8_t *dst = scratch;
    for (uint32_t row = 0; row < h; ++row) {
        for (uint32_t px = 0; px < w; ++px) {
            const uint8_t *s = src + (size_t)px * 4u;
            uint8_t a = s[3];
            if (a == 255u || a == 0u) {
                memcpy(dst, s, 4); /* fast paths, identical to flux_color_rgba_premul */
            } else {
                dst[0] = (uint8_t)((s[0] * a + 127u) / 255u);
                dst[1] = (uint8_t)((s[1] * a + 127u) / 255u);
                dst[2] = (uint8_t)((s[2] * a + 127u) / 255u);
                dst[3] = a;
            }
            dst += 4u;
        }
        src += stride;
    }
    flux_result r =
        flux_vk_upload_to_image(im->device, im->image, (int32_t)x, (int32_t)y, w, h,
                                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, scratch, packed);
    flux_internal_free(im->device, scratch);
    return r;
}

VkImage flux_image_vk_image(const flux_image *im) {
    return im ? im->image : VK_NULL_HANDLE;
}
VkImageView flux_image_vk_image_view(const flux_image *im) {
    return im ? im->view : VK_NULL_HANDLE;
}
uint32_t flux_image_width(const flux_image *im) {
    return im ? im->width : 0;
}
uint32_t flux_image_height(const flux_image *im) {
    return im ? im->height : 0;
}
flux_format flux_image_format(const flux_image *im) {
    return im ? im->format : FLUX_FORMAT_UNDEFINED;
}
flux_bindless_handle flux_image_bindless_handle(const flux_image *im) {
    return im ? im->bindless : FLUX_BINDLESS_INVALID;
}

flux_bindless_handle flux_image_bindless_storage_handle(const flux_image *im) {
    return im ? im->bindless_storage : FLUX_BINDLESS_INVALID;
}

flux_device *flux_image_device(const flux_image *im) {
    return im ? im->device : nullptr;
}

void flux_image_release(flux_image *im) {
    if (!im)
        return;
    if (atomic_fetch_sub_explicit(&im->ref_count, 1u, memory_order_acq_rel) != 1u)
        return;
    flux_device *d = im->device;
    /* The image may still be sampled by batches in flight on the graphics
     * queue (frames recorded before this release). Destroying the view /
     * image or freeing the memory inline can fault the engine mid-batch —
     * on i915 this was observed as a GPU hang and context reset, surfacing
     * in hosts as a fence timeout, a frozen UI thread, then
     * VK_ERROR_DEVICE_LOST. Park the pieces on the device retire queue;
     * they are destroyed once the queue provably passed every batch that
     * could reference them. */
    flux_device_retire_image(d, im->view, im->image, &im->alloc, im->imported_memory,
                             im->imported_size, im->bindless, im->bindless_storage);
    if (im->lut_image || im->lut_view)
        flux_device_retire_image(d, im->lut_view, im->lut_image, &im->lut_alloc, VK_NULL_HANDLE, 0,
                                 im->lut_bindless, FLUX_BINDLESS_INVALID);
    if (im->color_params)
        flux_buffer_release(im->color_params);
    if (im->icc)
        flux_icc_profile_release(im->icc);
    bool weak = im->device_weak;
    flux_internal_free(d, im);
    if (!weak)
        flux_device_release(d);
}

bool flux_device_supports_image_usage(const flux_device *d, flux_format format,
                                      flux_image_usage_query usage) {
    if (!d)
        return false;

    /* The flux format set maps 1:1 onto these Vulkan formats; the switch
     * doubles as the "format exists at all" check. */
    VkFormat vk;
    switch (format) {
    case FLUX_FORMAT_R8_UNORM:
        vk = VK_FORMAT_R8_UNORM;
        break;
    case FLUX_FORMAT_RGBA8_UNORM:
        vk = VK_FORMAT_R8G8B8A8_UNORM;
        break;
    case FLUX_FORMAT_BGRA8_UNORM:
        vk = VK_FORMAT_B8G8R8A8_UNORM;
        break;
    case FLUX_FORMAT_RGBA8_SRGB:
        vk = VK_FORMAT_R8G8B8A8_SRGB;
        break;
    case FLUX_FORMAT_BGRA8_SRGB:
        vk = VK_FORMAT_B8G8R8A8_SRGB;
        break;
    case FLUX_FORMAT_RGB10A2_UNORM:
        vk = VK_FORMAT_A2B10G10R10_UNORM_PACK32;
        break;
    case FLUX_FORMAT_RGBA16_SFLOAT:
        vk = VK_FORMAT_R16G16B16A16_SFLOAT;
        break;
    case FLUX_FORMAT_D32_SFLOAT:
        vk = VK_FORMAT_D32_SFLOAT;
        break;
    case FLUX_FORMAT_D24_UNORM_S8:
        vk = VK_FORMAT_D24_UNORM_S8_UINT;
        break;
    case FLUX_FORMAT_D32_SFLOAT_S8:
        vk = VK_FORMAT_D32_SFLOAT_S8_UINT;
        break;
    case FLUX_FORMAT_R32_SFLOAT:
        vk = VK_FORMAT_R32_SFLOAT;
        break;
    case FLUX_FORMAT_RG32_SFLOAT:
        vk = VK_FORMAT_R32G32_SFLOAT;
        break;
    case FLUX_FORMAT_RGB32_SFLOAT:
        vk = VK_FORMAT_R32G32B32_SFLOAT;
        break;
    case FLUX_FORMAT_RGBA32_SFLOAT:
        vk = VK_FORMAT_R32G32B32A32_SFLOAT;
        break;
    default:
        return false;
    }

    VkFormatProperties props;
    vkGetPhysicalDeviceFormatProperties(d->physical_device, vk, &props);

    switch (usage) {
    case FLUX_IMAGE_USAGE_SAMPLED:
        return (props.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) != 0;
    case FLUX_IMAGE_USAGE_COMPUTE_WRITE:
        /* Mirrors flux_image_create_compute_writable's policy exactly:
         * storage access, with the sRGB-view portability carve-out. */
        if (format == FLUX_FORMAT_RGBA8_SRGB || format == FLUX_FORMAT_BGRA8_SRGB)
            return false;
        return (props.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) != 0;
    }
    return false;
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
    /* Storage-image access requires a format with STORAGE_IMAGE support.
     * sRGB views over storage are not portable; reject early to avoid
     * driver surprises. RGBA8/BGRA8 UNORM are guaranteed by the spec;
     * RGBA16_SFLOAT (ADR-0069 working-space effects) needs the optional
     * feature bit, checked here. */
    if (fmt != FLUX_FORMAT_RGBA8_UNORM && fmt != FLUX_FORMAT_BGRA8_UNORM &&
        fmt != FLUX_FORMAT_RGBA16_SFLOAT) {
        FLUX_FAIL(FLUX_ERROR_UNSUPPORTED, "compute-writable image format not storage-compatible "
                                          "(use RGBA8/BGRA8_UNORM or RGBA16_SFLOAT)");
        return FLUX_ERROR_UNSUPPORTED;
    }
    if (fmt == FLUX_FORMAT_RGBA16_SFLOAT) {
        VkFormatProperties props;
        vkGetPhysicalDeviceFormatProperties(d->physical_device, VK_FORMAT_R16G16B16A16_SFLOAT,
                                            &props);
        if ((props.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) == 0) {
            FLUX_FAIL(FLUX_ERROR_UNSUPPORTED,
                      "compute-writable RGBA16_SFLOAT needs rgba16f storage support");
            return FLUX_ERROR_UNSUPPORTED;
        }
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

flux_result flux_frame_prepare_image_target(flux_frame *f, flux_image *target) {
    if (!f || !target)
        return FLUX_ERROR_INVALID_ARGUMENT;
    if (!target->render_target) {
        FLUX_FAIL(FLUX_ERROR_INVALID_ARGUMENT,
                  "image target was not created with flux_image_create_render_target");
        return FLUX_ERROR_INVALID_ARGUMENT;
    }
    if (f->state != FLUX_FRAME_STATE_RECORDING || f->pass_active) {
        FLUX_FAIL(FLUX_ERROR_INVALID_STATE, "image target prepare outside frame pass boundary");
        return FLUX_ERROR_INVALID_STATE;
    }
    if (target->current_layout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL)
        return FLUX_OK;
    bool first_use = target->current_layout == VK_IMAGE_LAYOUT_UNDEFINED;
    if (!first_use && target->current_layout != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
        FLUX_FAIL(FLUX_ERROR_INVALID_STATE,
                  "image target is neither new nor sampleable before prepare");
        return FLUX_ERROR_INVALID_STATE;
    }

    VkCommandBuffer cmd = flux_frame_vk_command_buffer(f);
    VkImageMemoryBarrier2 barrier = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .srcStageMask = first_use ? VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT
                                  : (VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT |
                                     VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT),
        .srcAccessMask = first_use ? 0 : VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
        .dstAccessMask =
            VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
        .oldLayout = target->current_layout,
        .newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .image = target->image,
        .subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                             .levelCount = 1,
                             .layerCount = 1},
    };
    VkDependencyInfo dependency = {
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .imageMemoryBarrierCount = 1,
        .pImageMemoryBarriers = &barrier,
    };
    vkCmdPipelineBarrier2(cmd, &dependency);
    target->current_layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    return FLUX_OK;
}

flux_result flux_frame_finish_image_target(flux_frame *f, flux_image *target) {
    if (!f || !target)
        return FLUX_ERROR_INVALID_ARGUMENT;
    if (!target->render_target) {
        FLUX_FAIL(FLUX_ERROR_INVALID_ARGUMENT,
                  "image target was not created with flux_image_create_render_target");
        return FLUX_ERROR_INVALID_ARGUMENT;
    }
    if (f->state != FLUX_FRAME_STATE_RECORDING || f->pass_active) {
        FLUX_FAIL(FLUX_ERROR_INVALID_STATE, "image target finish outside frame pass boundary");
        return FLUX_ERROR_INVALID_STATE;
    }
    if (target->current_layout != VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL) {
        FLUX_FAIL(FLUX_ERROR_INVALID_STATE, "image target was not prepared for colour output");
        return FLUX_ERROR_INVALID_STATE;
    }

    VkCommandBuffer cmd = flux_frame_vk_command_buffer(f);
    VkImageMemoryBarrier2 barrier = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
        .srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
        .dstStageMask =
            VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        .dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        .image = target->image,
        .subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                             .levelCount = 1,
                             .layerCount = 1},
    };
    VkDependencyInfo dependency = {
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .imageMemoryBarrierCount = 1,
        .pImageMemoryBarriers = &barrier,
    };
    vkCmdPipelineBarrier2(cmd, &dependency);
    target->current_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    return FLUX_OK;
}

/* ------------------------------------------------------------------ */
/*  Render-target image (ADR-0017)                                    */
/* ------------------------------------------------------------------ */

/* Create an image suitable as a flux_canvas_begin_target destination:
 * COLOR_ATTACHMENT | SAMPLED | TRANSFER_DST, 1 sample. Contents and layout
 * stay undefined until the first caller-recorded target pass; this avoids a
 * synchronous one-shot queue submission during frame recording. Strong device
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
    im->render_target = true;

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
