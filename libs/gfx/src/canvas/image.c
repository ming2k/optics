#include "../core/image_internal.h"

#include <stdio.h>
#include <string.h>

/* ADR-0070: GPU parameters for non-default content color spaces,
 * consumed by canvas_image.frag through a buffer device address.
 * std430 layout — keep in sync with the shader. */
typedef struct optics_gfx_image_color_params {
    float primaries[3][4]; /* content -> working space, column-major mat3 */
    uint32_t transfer;     /* flux_transfer_func of the content */
    float gamma;           /* FLUX_TRANSFER_GAMMA exponent */
    uint32_t lut_handle;   /* bindless 3D LUT, or FLUX_BINDLESS_INVALID */
    uint32_t lut_size;
} optics_gfx_image_color_params;

/* The content space an untagged image is interpreted as (ADR-0069). */
static optics_gfx_color_space image_default_space(optics_gfx_format f) {
    if (f == OPTICS_GFX_FORMAT_RGBA16_SFLOAT)
        return (optics_gfx_color_space)OPTICS_GFX_COLOR_SPACE_SCRGB;
    return (optics_gfx_color_space)OPTICS_GFX_COLOR_SPACE_SRGB;
}

/* Create the 3D LUT image for a baked ICC profile: stored R-fastest in
 * the profile, re-laid as a 2D (N² × N) image so the stock 2D bindless
 * heap serves it; the shader does the slice lerp. RGBA32F keeps the
 * upload a plain widening of the baked floats. */
static optics_gfx_result image_create_lut(optics_gfx_device *d, optics_gfx_image_color *color,
                                          const float *lut, uint32_t n) {
    uint32_t w = n * n, h = n;
    size_t texel_count = (size_t)w * h;
    float *rgba = optics_gfx_internal_alloc(d, texel_count * 4 * sizeof(float));
    if (!rgba)
        return OPTICS_GFX_ERROR_OUT_OF_MEMORY;
    for (size_t i = 0; i < texel_count; ++i) {
        rgba[i * 4 + 0] = lut[i * 3 + 0];
        rgba[i * 4 + 1] = lut[i * 3 + 1];
        rgba[i * 4 + 2] = lut[i * 3 + 2];
        rgba[i * 4 + 3] = 1.0f;
    }

    optics_gfx_result r = OPTICS_GFX_OK;
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
    color->lut_bindless = OPTICS_GFX_BINDLESS_INVALID;
    r = optics_gfx_vk_alloc_image(d, &ici, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &color->lut_image,
                                  &color->lut_alloc);
    if (r != OPTICS_GFX_OK)
        goto out_free;
    r = optics_gfx_vk_upload_to_image(d, nullptr, color->lut_image, 0, 0, w, h,
                                      VK_IMAGE_LAYOUT_UNDEFINED, rgba,
                                      texel_count * 4 * sizeof(float));
    if (r != OPTICS_GFX_OK)
        goto out_free;
    VkImageViewCreateInfo ivci = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = color->lut_image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = VK_FORMAT_R32G32B32A32_SFLOAT,
        .subresourceRange =
            {
                .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                .levelCount = 1,
                .layerCount = 1,
            },
    };
    if (vkCreateImageView(d->device, &ivci, nullptr, &color->lut_view) != VK_SUCCESS) {
        OPTICS_GFX_FAIL(OPTICS_GFX_ERROR_BACKEND_FAILURE, "ICC LUT view failed");
        r = OPTICS_GFX_ERROR_BACKEND_FAILURE;
        goto out_free;
    }
    r = optics_gfx_bindless_register_image(
        d, color->lut_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, &color->lut_bindless);

out_free:
    optics_gfx_internal_free(d, rgba);
    return r;
}

void optics_gfx_image_release_color(optics_gfx_device *d, optics_gfx_image_color *color) {
    if (!color)
        return;
    if (atomic_fetch_sub_explicit(&color->refs, 1u, memory_order_acq_rel) != 1u)
        return;
    if (color->lut_image || color->lut_view)
        optics_gfx_device_retire_image(d, color->lut_view, color->lut_image, &color->lut_alloc,
                                       VK_NULL_HANDLE, 0, color->lut_bindless,
                                       OPTICS_GFX_BINDLESS_INVALID);
    if (color->color_params)
        optics_gfx_buffer_release(color->color_params);
    if (color->icc)
        optics_gfx_icc_profile_release(color->icc);
    optics_gfx_internal_free(d, color);
}

optics_gfx_result optics_gfx_image_init_color(optics_gfx_device *d, optics_gfx_image *im,
                                              const void *next_chain) {
    optics_gfx_color_space content = image_default_space(im->format);
    const float *lut = nullptr;
    uint32_t lut_size = 0;
    bool tagged = false;
    optics_gfx_icc_profile *icc = nullptr;

    const struct {
        optics_gfx_struct_type type;
        const void *next;
    } *extension = next_chain;
    while (extension) {
        if (extension->type == OPTICS_GFX_TYPE_IMAGE_COLOR_SPACE_DESC) {
            const optics_gfx_image_color_space_desc *cs = (const void *)extension;
            if (cs->space) {
                if (!optics_gfx_color_space_is_valid(*cs->space)) {
                    if (icc)
                        optics_gfx_icc_profile_release(icc);
                    OPTICS_GFX_FAIL(OPTICS_GFX_ERROR_INVALID_ARGUMENT,
                                    "image color space tag is invalid");
                    return OPTICS_GFX_ERROR_INVALID_ARGUMENT;
                }
                content = *cs->space;
                tagged = true;
            }
            if (cs->icc) {
                optics_gfx_color_space extracted;
                /* Profiles are immutable apart from the atomic refcount,
                 * so retaining through the desc's const pointer is sound.
                 * A malformed chain with two ICC tags keeps only the last;
                 * release the earlier retain instead of leaking it. */
                if (icc)
                    optics_gfx_icc_profile_release(icc);
                icc = optics_gfx_icc_profile_retain((optics_gfx_icc_profile *)cs->icc);
                if (optics_gfx_icc_profile_color_space(icc, &extracted)) {
                    content = extracted;
                } else {
                    lut = optics_gfx_icc_profile_lut(icc, &lut_size);
                    if (!lut) {
                        optics_gfx_icc_profile_release(icc);
                        OPTICS_GFX_FAIL(OPTICS_GFX_ERROR_INVALID_STATE,
                                        "ICC profile carries no transform");
                        return OPTICS_GFX_ERROR_INVALID_STATE;
                    }
                }
                tagged = true;
            }
        }
        extension = extension->next;
    }

    if (!tagged) {
        if (icc)
            optics_gfx_icc_profile_release(icc);
        return OPTICS_GFX_OK;
    }
    /* An explicit tag equal to the format default needs no GPU state. */
    if (!lut && optics_gfx_color_space_equal(content, image_default_space(im->format))) {
        if (icc)
            optics_gfx_icc_profile_release(icc);
        return OPTICS_GFX_OK;
    }

    optics_gfx_image_color *color = optics_gfx_internal_alloc(d, sizeof(*color));
    if (!color) {
        if (icc)
            optics_gfx_icc_profile_release(icc);
        return OPTICS_GFX_ERROR_OUT_OF_MEMORY;
    }
    *color = (optics_gfx_image_color){
        .lut_bindless = OPTICS_GFX_BINDLESS_INVALID,
        .icc = icc,
    };
    atomic_init(&color->refs, 1u);

    if (lut) {
        optics_gfx_result r = image_create_lut(d, color, lut, lut_size);
        if (r != OPTICS_GFX_OK) {
            optics_gfx_image_release_color(d, color);
            return r;
        }
    }

    optics_gfx_image_color_params params = {};
    optics_gfx_mat3 to_working;
    optics_gfx_color_space working = OPTICS_GFX_COLOR_SPACE_SCRGB;
    optics_gfx_color_space_transform_matrix(content, working, &to_working);
    for (int col = 0; col < 3; ++col) {
        for (int row = 0; row < 3; ++row)
            params.primaries[col][row] = to_working.m[col * 3 + row];
        params.primaries[col][3] = 0.0f;
    }
    params.transfer = (uint32_t)content.transfer;
    params.gamma = content.gamma;
    params.lut_handle = color->lut_bindless;
    params.lut_size = lut_size;

    optics_gfx_buffer_desc bd = OPTICS_GFX_BUFFER_DESC_INIT;
    bd.size = sizeof(params);
    bd.usage = OPTICS_GFX_BUFFER_USAGE_STORAGE;
    bd.location = OPTICS_GFX_BUFFER_HOST_VISIBLE;
    bd.device_address = true;
    bd.initial_data = &params;
    optics_gfx_result r = optics_gfx_buffer_create(d, &bd, &color->color_params);
    if (r != OPTICS_GFX_OK) {
        optics_gfx_image_release_color(d, color);
        return r;
    }
    color->color_params_address = optics_gfx_buffer_device_address(color->color_params);
    if (color->color_params_address == 0) {
        optics_gfx_image_release_color(d, color);
        OPTICS_GFX_FAIL(OPTICS_GFX_ERROR_BACKEND_FAILURE,
                        "color params buffer has no device address");
        return OPTICS_GFX_ERROR_BACKEND_FAILURE;
    }
    im->color = color;
    return OPTICS_GFX_OK;
}

static uint32_t bytes_per_pixel(optics_gfx_format f) {
    switch (f) {
    case OPTICS_GFX_FORMAT_R8_UNORM:
        return 1;
    case OPTICS_GFX_FORMAT_RGBA8_UNORM:
    case OPTICS_GFX_FORMAT_BGRA8_UNORM:
    case OPTICS_GFX_FORMAT_RGBA8_SRGB:
    case OPTICS_GFX_FORMAT_BGRA8_SRGB:
        return 4;
    case OPTICS_GFX_FORMAT_RGBA16_SFLOAT:
        return 8; /* ADR-0069 working-space content */
    default:
        return 0;
    }
}

static optics_gfx_result image_create_compute_writable(optics_gfx_device *d, uint32_t width,
                                                       uint32_t height, optics_gfx_format fmt,
                                                       optics_gfx_image **out);
static optics_gfx_result image_create_render_target(optics_gfx_device *d, uint32_t width,
                                                    uint32_t height, optics_gfx_format fmt,
                                                    optics_gfx_image **out);

optics_gfx_result optics_gfx_image_create(optics_gfx_device *d, const optics_gfx_image_desc *desc,
                                          optics_gfx_image **out) {
    if (!d || !desc || !out)
        return OPTICS_GFX_ERROR_INVALID_ARGUMENT;
    if (desc->type != OPTICS_GFX_TYPE_IMAGE_DESC) {
        OPTICS_GFX_FAIL(OPTICS_GFX_ERROR_INVALID_ARGUMENT, "desc->type != FLUX_TYPE_IMAGE_DESC");
        return OPTICS_GFX_ERROR_INVALID_ARGUMENT;
    }
    if (desc->usage != OPTICS_GFX_IMAGE_USAGE_SAMPLED) {
        /* Attachment and storage images start with undefined contents: pixel
         * data and a colour-space tag describe uploads, which they never take. */
        if (desc->initial_data || desc->next) {
            OPTICS_GFX_FAIL(OPTICS_GFX_ERROR_INVALID_ARGUMENT,
                            "initial_data / extensions apply to FLUX_IMAGE_USAGE_SAMPLED only");
            return OPTICS_GFX_ERROR_INVALID_ARGUMENT;
        }
        if (desc->usage == OPTICS_GFX_IMAGE_USAGE_COMPUTE_WRITE)
            return image_create_compute_writable(d, desc->width, desc->height, desc->format, out);
        if (desc->usage == OPTICS_GFX_IMAGE_USAGE_RENDER_TARGET)
            return image_create_render_target(d, desc->width, desc->height, desc->format, out);
        OPTICS_GFX_FAIL(OPTICS_GFX_ERROR_INVALID_ARGUMENT, "unknown flux_image_usage");
        return OPTICS_GFX_ERROR_INVALID_ARGUMENT;
    }
    if (desc->width == 0 || desc->height == 0) {
        OPTICS_GFX_FAIL(OPTICS_GFX_ERROR_INVALID_ARGUMENT, "image zero-extent");
        return OPTICS_GFX_ERROR_INVALID_ARGUMENT;
    }
    VkFormat vfmt = optics_gfx_format_to_vk(desc->format);
    uint32_t bpp = bytes_per_pixel(desc->format);
    if (vfmt == VK_FORMAT_UNDEFINED || bpp == 0) {
        OPTICS_GFX_FAIL(OPTICS_GFX_ERROR_UNSUPPORTED, "unsupported pixel format");
        return OPTICS_GFX_ERROR_UNSUPPORTED;
    }
    *out = nullptr;

    optics_gfx_image *im = optics_gfx_internal_alloc(d, sizeof(*im));
    if (!im)
        return OPTICS_GFX_ERROR_OUT_OF_MEMORY;
    atomic_init(&im->ref_count, 1u);
    im->shared = nullptr;
    im->mutable = true;
    im->imported = false;
    im->snapshot_allowed = true;
    im->color = nullptr;
    im->device = optics_gfx_device_retain(d);
    im->width = desc->width;
    im->height = desc->height;
    im->format = desc->format;
    im->bindless = OPTICS_GFX_BINDLESS_INVALID;
    im->bindless_storage = OPTICS_GFX_BINDLESS_INVALID;
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
        .usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                 VK_IMAGE_USAGE_SAMPLED_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    optics_gfx_result r = optics_gfx_vk_alloc_image(d, &ici, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                                                    &im->image, &im->alloc);
    if (r != OPTICS_GFX_OK)
        goto fail;
    /* Ledger-derived per-instance name (ADR-0132): unique, and rendered into
     * storage that outlives the call. */
    optics_gfx_vk_set_name_from_ledger(d, VK_OBJECT_TYPE_IMAGE, (uint64_t)im->image,
                                       OPTICS_GFX_SCOPE_DEVICE, "vk_image", "flux/core");
    VkResult vr;

    if (desc->initial_data) {
        size_t bytes = (size_t)desc->width * desc->height * bpp;
        r = optics_gfx_vk_upload_to_image(d, im, im->image, 0, 0, desc->width, desc->height,
                                          VK_IMAGE_LAYOUT_UNDEFINED, desc->initial_data, bytes);
        if (r != OPTICS_GFX_OK)
            goto fail;
    } else {
        /* No initial data — transition to SHADER_READ_ONLY_OPTIMAL so
         * the sampled view is immediately usable. */
        r = optics_gfx_vk_transition_image_layout(d, im->image, VK_IMAGE_LAYOUT_UNDEFINED,
                                                  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        if (r != OPTICS_GFX_OK)
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
        OPTICS_GFX_FAIL_VK(OPTICS_GFX_ERROR_BACKEND_FAILURE, "vkCreateImageView failed", vr);
        goto fail;
    }

    r = optics_gfx_bindless_register_image(d, im->view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                           &im->bindless);
    if (r != OPTICS_GFX_OK)
        goto fail;

    r = optics_gfx_image_init_color(d, im, desc->next);
    if (r != OPTICS_GFX_OK)
        goto fail;

    *out = im;
    OPTICS_GFX_OBSERVE_ACQUIRE(d, OPTICS_GFX_SCOPE_DEVICE, OPTICS_GFX_LIFETIME_CONTEXT, "vk_image",
                               "flux/core", 0, OPTICS_GFX_SITE_VK_IMAGE);
    return OPTICS_GFX_OK;

fail:
    /* Match compute_writable's fail path: release any bindless slots
     * that were registered before the failure. Robust against future
     * steps being inserted between register and return. */
    if (im->bindless != OPTICS_GFX_BINDLESS_INVALID)
        optics_gfx_bindless_release(d, im->bindless);
    if (im->bindless_storage != OPTICS_GFX_BINDLESS_INVALID)
        optics_gfx_bindless_release(d, im->bindless_storage);
    optics_gfx_image_release_color(d, im->color);
    im->color = nullptr;
    if (im->view)
        vkDestroyImageView(d->device, im->view, nullptr);
    if (im->image)
        vkDestroyImage(d->device, im->image, nullptr);
    if (im->alloc.memory)
        optics_gfx_vk_deallocate(d, &im->alloc);
    optics_gfx_device_release(d);
    optics_gfx_internal_free(d, im);
    return r;
}

static optics_gfx_image_shared *image_shared_create(optics_gfx_image *im) {
    optics_gfx_image_shared *sh = optics_gfx_internal_alloc(im->device, sizeof(*sh));
    if (!sh)
        return nullptr;
    atomic_init(&sh->refs, 1u);
    sh->image = im->image;
    sh->alloc = im->alloc;
    sh->imported_memory = im->imported_memory;
    sh->imported_size = im->imported_size;
    sh->view = im->view;
    sh->bindless = im->bindless;
    sh->bindless_storage = im->bindless_storage;
    return sh;
}

optics_gfx_result optics_gfx_image_snapshot(optics_gfx_image *im, optics_gfx_image **out) {
    if (!out)
        return OPTICS_GFX_ERROR_INVALID_ARGUMENT;
    *out = nullptr;
    if (!im)
        return OPTICS_GFX_ERROR_INVALID_ARGUMENT;
    if (!im->snapshot_allowed) {
        OPTICS_GFX_FAIL(OPTICS_GFX_ERROR_INVALID_STATE,
                        "borrowed dma-buf images cannot be snapshotted; bind a dynamic input slot");
        return OPTICS_GFX_ERROR_INVALID_STATE;
    }
    optics_gfx_device *d = im->device;
    optics_gfx_platform_mutex_lock(&d->image_version_lock);
    if (!im->shared) {
        im->shared = image_shared_create(im);
        if (!im->shared) {
            optics_gfx_platform_mutex_unlock(&d->image_version_lock);
            return OPTICS_GFX_ERROR_OUT_OF_MEMORY;
        }
    }
    optics_gfx_image *snap = optics_gfx_internal_alloc(d, sizeof(*snap));
    if (!snap) {
        optics_gfx_platform_mutex_unlock(&d->image_version_lock);
        return OPTICS_GFX_ERROR_OUT_OF_MEMORY;
    }
    memcpy(snap, im, sizeof(*snap));
    atomic_init(&snap->ref_count, 1u);
    snap->device = optics_gfx_device_retain(d);
    snap->device_weak = false;
    snap->mutable = false;
    if (im->color)
        atomic_fetch_add_explicit(&im->color->refs, 1u, memory_order_relaxed);
    atomic_fetch_add_explicit(&im->shared->refs, 1u, memory_order_relaxed);
    optics_gfx_platform_mutex_unlock(&d->image_version_lock);
    *out = snap;
    return OPTICS_GFX_OK;
}

/* Copy an image into a freshly allocated, owned image. Used only by COW when
 * an update would otherwise mutate content a snapshot still references. */
static optics_gfx_result image_copy_owned(optics_gfx_image *im, VkImage *out_image,
                                          optics_gfx_vk_alloc *out_alloc, VkImageView *out_view,
                                          optics_gfx_bindless_handle *out_bindless,
                                          optics_gfx_bindless_handle *out_storage) {
    if (im->imported || im->foreign_owned)
        return OPTICS_GFX_ERROR_UNSUPPORTED;
    VkImageCreateInfo ici = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = optics_gfx_format_to_vk(im->format),
        .extent = {im->width, im->height, 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage =
            VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
            VK_IMAGE_USAGE_SAMPLED_BIT |
            (im->render_target ? VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT : 0) |
            (im->bindless_storage != OPTICS_GFX_BINDLESS_INVALID ? VK_IMAGE_USAGE_STORAGE_BIT : 0),
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    optics_gfx_result r = optics_gfx_vk_alloc_image(
        im->device, &ici, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, out_image, out_alloc);
    if (r != OPTICS_GFX_OK)
        return r;

    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkResult vr =
        optics_gfx_vk_new_transient_cmd(im->device, im->device->graphics_family, &pool, &cmd);
    if (vr != VK_SUCCESS) {
        vkDestroyImage(im->device->device, *out_image, nullptr);
        *out_image = VK_NULL_HANDLE;
        optics_gfx_vk_deallocate(im->device, out_alloc);
        return OPTICS_GFX_ERROR_BACKEND_FAILURE;
    }

    VkImageMemoryBarrier2 barriers[2] = {
        {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
            .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
            .dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = im->image,
            .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
        },
        {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
            .srcStageMask = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT,
            .dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
            .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = *out_image,
            .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
        },
    };
    VkDependencyInfo dep = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.imageMemoryBarrierCount = 2;
    dep.pImageMemoryBarriers = barriers;
    vkCmdPipelineBarrier2(cmd, &dep);

    VkImageCopy region = {
        .srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
        .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
        .extent = {im->width, im->height, 1},
    };
    vkCmdCopyImage(cmd, im->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, *out_image,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    barriers[0].srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
    barriers[0].dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    barriers[0].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    barriers[0].newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barriers[1].srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
    barriers[1].dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    barriers[1].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barriers[1].newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    vkCmdPipelineBarrier2(cmd, &dep);

    vr = vkEndCommandBuffer(cmd);
    if (vr == VK_SUCCESS)
        vr = optics_gfx_vk_submit_one_shot_and_wait(im->device, cmd);
    vkDestroyCommandPool(im->device->device, pool, nullptr);
    if (vr != VK_SUCCESS) {
        vkDestroyImage(im->device->device, *out_image, nullptr);
        *out_image = VK_NULL_HANDLE;
        optics_gfx_vk_deallocate(im->device, out_alloc);
        return OPTICS_GFX_ERROR_BACKEND_FAILURE;
    }

    VkImageView view = VK_NULL_HANDLE;
    VkImageViewCreateInfo ivci = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = *out_image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = optics_gfx_format_to_vk(im->format),
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
    };
    if (vkCreateImageView(im->device->device, &ivci, nullptr, &view) != VK_SUCCESS) {
        vkDestroyImage(im->device->device, *out_image, nullptr);
        *out_image = VK_NULL_HANDLE;
        optics_gfx_vk_deallocate(im->device, out_alloc);
        return OPTICS_GFX_ERROR_BACKEND_FAILURE;
    }
    optics_gfx_bindless_handle bindless = OPTICS_GFX_BINDLESS_INVALID;
    if (optics_gfx_bindless_register_image(im->device, view,
                                           VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                           &bindless) != OPTICS_GFX_OK) {
        vkDestroyImageView(im->device->device, view, nullptr);
        vkDestroyImage(im->device->device, *out_image, nullptr);
        *out_image = VK_NULL_HANDLE;
        optics_gfx_vk_deallocate(im->device, out_alloc);
        return OPTICS_GFX_ERROR_BACKEND_FAILURE;
    }
    optics_gfx_bindless_handle storage = OPTICS_GFX_BINDLESS_INVALID;
    if (im->bindless_storage != OPTICS_GFX_BINDLESS_INVALID &&
        optics_gfx_bindless_register_storage_image(im->device, view, VK_IMAGE_LAYOUT_GENERAL,
                                                   &storage) != OPTICS_GFX_OK) {
        optics_gfx_bindless_release(im->device, bindless);
        vkDestroyImageView(im->device->device, view, nullptr);
        vkDestroyImage(im->device->device, *out_image, nullptr);
        *out_image = VK_NULL_HANDLE;
        optics_gfx_vk_deallocate(im->device, out_alloc);
        return OPTICS_GFX_ERROR_BACKEND_FAILURE;
    }
    *out_view = view;
    *out_bindless = bindless;
    *out_storage = storage;
    return OPTICS_GFX_OK;
}

static optics_gfx_result image_cow(optics_gfx_image *im) {
    if (!im->mutable)
        return OPTICS_GFX_ERROR_INVALID_STATE;
    if (!im->shared || atomic_load_explicit(&im->shared->refs, memory_order_acquire) <= 1)
        return OPTICS_GFX_OK;
    VkImage image = VK_NULL_HANDLE;
    optics_gfx_vk_alloc alloc = {};
    VkImageView view = VK_NULL_HANDLE;
    optics_gfx_bindless_handle bindless = OPTICS_GFX_BINDLESS_INVALID;
    optics_gfx_bindless_handle storage = OPTICS_GFX_BINDLESS_INVALID;
    optics_gfx_result r = image_copy_owned(im, &image, &alloc, &view, &bindless, &storage);
    if (r != OPTICS_GFX_OK)
        return r;

    optics_gfx_image_shared *old = im->shared;
    im->shared = nullptr;
    im->image = image;
    im->alloc = alloc;
    im->view = view;
    im->bindless = bindless;
    im->bindless_storage = storage;
    im->current_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    im->imported_memory = VK_NULL_HANDLE;
    im->imported_size = 0;
    im->foreign_owned = false;
    atomic_fetch_sub_explicit(&old->refs, 1u, memory_order_acq_rel);
    return OPTICS_GFX_OK;
}

size_t optics_gfx_image_size_bytes(const optics_gfx_image *im) {
    if (!im)
        return 0;
    uint32_t bpp = bytes_per_pixel(im->format);
    if (bpp == 0)
        return 0;
    size_t pixels = (size_t)im->width * (size_t)im->height;
    if (pixels > SIZE_MAX / bpp)
        return SIZE_MAX;
    return pixels * bpp;
}

optics_gfx_image *optics_gfx_image_retain(optics_gfx_image *im) {
    if (im)
        atomic_fetch_add_explicit(&im->ref_count, 1u, memory_order_relaxed);
    return im;
}

/* Shared validation for the update-region family. On success writes the
 * packed byte count the upload needs to `*packed_bytes` and returns FLUX_OK;
 * on failure reports through FLUX_FAIL and returns the error code.
 * `row_bytes` == 0 means the packed variant (stride derived from width);
 * otherwise it is the source row pitch and must be at least width * bpp. */
static optics_gfx_result update_region_validate(const optics_gfx_image *im, uint32_t x, uint32_t y,
                                                uint32_t w, uint32_t h, const void *data,
                                                size_t row_bytes, size_t bytes,
                                                size_t *packed_bytes) {
    if (!im || !data)
        return OPTICS_GFX_ERROR_INVALID_ARGUMENT;
    if (!im->mutable)
        return OPTICS_GFX_ERROR_INVALID_STATE;
    if (w == 0 || h == 0)
        return OPTICS_GFX_ERROR_INVALID_ARGUMENT;
    if (x > im->width || y > im->height || x + w > im->width || y + h > im->height) {
        OPTICS_GFX_FAIL(OPTICS_GFX_ERROR_OUT_OF_RANGE, "update region exceeds image extent");
        return OPTICS_GFX_ERROR_OUT_OF_RANGE;
    }
    uint32_t bpp = bytes_per_pixel(im->format);
    if (bpp == 0) {
        OPTICS_GFX_FAIL(OPTICS_GFX_ERROR_UNSUPPORTED, "image has no known bytes-per-pixel");
        return OPTICS_GFX_ERROR_UNSUPPORTED;
    }
    size_t row_min = (size_t)w * bpp;
    size_t stride = row_bytes ? row_bytes : row_min;
    if (stride < row_min) {
        OPTICS_GFX_FAIL(OPTICS_GFX_ERROR_INVALID_ARGUMENT,
                        "update row_bytes is smaller than width * bpp");
        return OPTICS_GFX_ERROR_INVALID_ARGUMENT;
    }
    size_t needed = (h - 1) * stride + row_min;
    if (bytes < needed) {
        OPTICS_GFX_FAIL(OPTICS_GFX_ERROR_INVALID_ARGUMENT,
                        "update data too small for the row stride");
        return OPTICS_GFX_ERROR_INVALID_ARGUMENT;
    }
    /* The upload is recorded with old_layout = SHADER_READ_ONLY_OPTIMAL
     * (and transitions back to it). Anything else — a compute-writable
     * image in GENERAL, a render target mid-capture in
     * COLOR_ATTACHMENT_OPTIMAL — would record a barrier with the wrong
     * oldLayout (undefined contents), so refuse it loudly. */
    if (im->current_layout != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
        OPTICS_GFX_FAIL(OPTICS_GFX_ERROR_INVALID_STATE,
                        "image update requires SHADER_READ_ONLY_OPTIMAL current layout");
        return OPTICS_GFX_ERROR_INVALID_STATE;
    }
    *packed_bytes = row_min * h;
    return OPTICS_GFX_OK;
}

optics_gfx_result optics_gfx_image_update_region(optics_gfx_image *im,
                                                 const optics_gfx_image_update_region_opts *opts) {
    if (!im || !opts)
        return OPTICS_GFX_ERROR_INVALID_ARGUMENT;
    const uint32_t x = opts->x;
    const uint32_t y = opts->y;
    const uint32_t w = opts->width;
    const uint32_t h = opts->height;
    const void *data = opts->data;
    const size_t row_bytes = opts->row_bytes;
    const size_t bytes = opts->bytes;
    if (opts->premultiply && im->format != OPTICS_GFX_FORMAT_RGBA8_UNORM) {
        OPTICS_GFX_FAIL(OPTICS_GFX_ERROR_UNSUPPORTED,
                        "premultiply upload requires FLUX_FORMAT_RGBA8_UNORM");
        return OPTICS_GFX_ERROR_UNSUPPORTED;
    }
    optics_gfx_device *d = im->device;
    optics_gfx_platform_mutex_lock(&d->image_version_lock);
    size_t packed;
    optics_gfx_result v = update_region_validate(im, x, y, w, h, data, row_bytes, bytes, &packed);
    if (v != OPTICS_GFX_OK)
        goto out;
    v = image_cow(im);
    if (v != OPTICS_GFX_OK)
        goto out;

    size_t row_min = (size_t)w * bytes_per_pixel(im->format);
    if (!opts->premultiply && (row_bytes == 0 || row_bytes == row_min)) {
        v = optics_gfx_vk_upload_to_image(d, im, im->image, (int32_t)x, (int32_t)y, w, h,
                                          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, data, packed);
        goto out;
    }

    uint8_t *scratch = optics_gfx_internal_alloc(d, packed);
    if (!scratch) {
        v = OPTICS_GFX_ERROR_OUT_OF_MEMORY;
        goto out;
    }
    size_t stride = row_bytes ? row_bytes : row_min;
    const uint8_t *src = data;
    uint8_t *dst = scratch;
    if (opts->premultiply) {
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
    } else {
        for (uint32_t row = 0; row < h; ++row) {
            memcpy(dst, src, row_min);
            src += stride;
            dst += row_min;
        }
    }
    v = optics_gfx_vk_upload_to_image(d, im, im->image, (int32_t)x, (int32_t)y, w, h,
                                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, scratch, packed);
    optics_gfx_internal_free(d, scratch);
out:
    optics_gfx_platform_mutex_unlock(&d->image_version_lock);
    return v;
}

VkImage optics_gfx_image_vk_image(const optics_gfx_image *im) {
    return im ? im->image : VK_NULL_HANDLE;
}
VkImageView optics_gfx_image_vk_image_view(const optics_gfx_image *im) {
    return im ? im->view : VK_NULL_HANDLE;
}
uint32_t optics_gfx_image_width(const optics_gfx_image *im) {
    return im ? im->width : 0;
}
uint32_t optics_gfx_image_height(const optics_gfx_image *im) {
    return im ? im->height : 0;
}
optics_gfx_format optics_gfx_image_format(const optics_gfx_image *im) {
    return im ? im->format : OPTICS_GFX_FORMAT_UNDEFINED;
}
optics_gfx_bindless_handle optics_gfx_image_bindless_handle(const optics_gfx_image *im) {
    return im ? im->bindless : OPTICS_GFX_BINDLESS_INVALID;
}

optics_gfx_bindless_handle optics_gfx_image_bindless_storage_handle(const optics_gfx_image *im) {
    return im ? im->bindless_storage : OPTICS_GFX_BINDLESS_INVALID;
}

optics_gfx_device *optics_gfx_image_device(const optics_gfx_image *im) {
    return im ? im->device : nullptr;
}

static void image_retire_resources(optics_gfx_device *d, optics_gfx_image *im) {
    /* The image may still be sampled by batches in flight on the graphics
     * queue (frames recorded before this release). Destroying the view /
     * image or freeing the memory inline can fault the engine mid-batch —
     * on i915 this was observed as a GPU hang and context reset, surfacing
     * in hosts as a fence timeout, a frozen UI thread, then
     * VK_ERROR_DEVICE_LOST. Park the pieces on the device retire queue;
     * they are destroyed once the queue provably passed every batch that
     * could reference them. */
    optics_gfx_device_retire_image(d, im->view, im->image, &im->alloc, im->imported_memory,
                                   im->imported_size, im->bindless, im->bindless_storage);
}

void optics_gfx_image_release(optics_gfx_image *im) {
    if (!im)
        return;
    if (atomic_fetch_sub_explicit(&im->ref_count, 1u, memory_order_acq_rel) != 1u)
        return;
    optics_gfx_device *d = im->device;
    bool retire = true;
    optics_gfx_image_shared *shared = im->shared;
    if (shared) {
        if (atomic_fetch_sub_explicit(&shared->refs, 1u, memory_order_acq_rel) != 1u)
            retire = false;
    }
    if (retire)
        image_retire_resources(d, im);
    optics_gfx_image_release_color(d, im->color);
    OPTICS_GFX_OBSERVE_RELEASE(d, OPTICS_GFX_SCOPE_DEVICE, "vk_image", "flux/core", 0);
    bool weak = im->device_weak;
    if (shared && retire)
        optics_gfx_internal_free(d, shared);
    optics_gfx_internal_free(d, im);
    if (!weak)
        optics_gfx_device_release(d);
}

bool optics_gfx_device_supports_image_usage(const optics_gfx_device *d, optics_gfx_format format,
                                            optics_gfx_image_usage usage) {
    if (!d)
        return false;

    /* The flux format set maps 1:1 onto these Vulkan formats; the switch
     * doubles as the "format exists at all" check. */
    VkFormat vk;
    switch (format) {
    case OPTICS_GFX_FORMAT_R8_UNORM:
        vk = VK_FORMAT_R8_UNORM;
        break;
    case OPTICS_GFX_FORMAT_RGBA8_UNORM:
        vk = VK_FORMAT_R8G8B8A8_UNORM;
        break;
    case OPTICS_GFX_FORMAT_BGRA8_UNORM:
        vk = VK_FORMAT_B8G8R8A8_UNORM;
        break;
    case OPTICS_GFX_FORMAT_RGBA8_SRGB:
        vk = VK_FORMAT_R8G8B8A8_SRGB;
        break;
    case OPTICS_GFX_FORMAT_BGRA8_SRGB:
        vk = VK_FORMAT_B8G8R8A8_SRGB;
        break;
    case OPTICS_GFX_FORMAT_RGB10A2_UNORM:
        vk = VK_FORMAT_A2B10G10R10_UNORM_PACK32;
        break;
    case OPTICS_GFX_FORMAT_RGBA16_SFLOAT:
        vk = VK_FORMAT_R16G16B16A16_SFLOAT;
        break;
    case OPTICS_GFX_FORMAT_D32_SFLOAT:
        vk = VK_FORMAT_D32_SFLOAT;
        break;
    case OPTICS_GFX_FORMAT_D24_UNORM_S8:
        vk = VK_FORMAT_D24_UNORM_S8_UINT;
        break;
    case OPTICS_GFX_FORMAT_D32_SFLOAT_S8:
        vk = VK_FORMAT_D32_SFLOAT_S8_UINT;
        break;
    case OPTICS_GFX_FORMAT_R32_SFLOAT:
        vk = VK_FORMAT_R32_SFLOAT;
        break;
    case OPTICS_GFX_FORMAT_RG32_SFLOAT:
        vk = VK_FORMAT_R32G32_SFLOAT;
        break;
    case OPTICS_GFX_FORMAT_RGB32_SFLOAT:
        vk = VK_FORMAT_R32G32B32_SFLOAT;
        break;
    case OPTICS_GFX_FORMAT_RGBA32_SFLOAT:
        vk = VK_FORMAT_R32G32B32A32_SFLOAT;
        break;
    default:
        return false;
    }

    VkFormatProperties props;
    vkGetPhysicalDeviceFormatProperties(d->physical_device, vk, &props);

    switch (usage) {
    case OPTICS_GFX_IMAGE_USAGE_SAMPLED:
        return (props.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) != 0;
    case OPTICS_GFX_IMAGE_USAGE_COMPUTE_WRITE:
        /* Mirrors the COMPUTE_WRITE creation policy exactly: storage access,
         * with the sRGB-view portability carve-out. */
        if (format == OPTICS_GFX_FORMAT_RGBA8_SRGB || format == OPTICS_GFX_FORMAT_BGRA8_SRGB)
            return false;
        return (props.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) != 0;
    case OPTICS_GFX_IMAGE_USAGE_RENDER_TARGET:
        return (props.optimalTilingFeatures & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT) != 0 &&
               (props.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) != 0;
    }
    return false;
}

static optics_gfx_result image_create_compute_writable(optics_gfx_device *d, uint32_t width,
                                                       uint32_t height, optics_gfx_format fmt,
                                                       optics_gfx_image **out) {
    if (!d || !out)
        return OPTICS_GFX_ERROR_INVALID_ARGUMENT;
    if (width == 0 || height == 0) {
        OPTICS_GFX_FAIL(OPTICS_GFX_ERROR_INVALID_ARGUMENT, "compute-writable image zero-extent");
        return OPTICS_GFX_ERROR_INVALID_ARGUMENT;
    }
    VkFormat vfmt = optics_gfx_format_to_vk(fmt);
    if (vfmt == VK_FORMAT_UNDEFINED) {
        OPTICS_GFX_FAIL(OPTICS_GFX_ERROR_UNSUPPORTED, "compute-writable image unsupported format");
        return OPTICS_GFX_ERROR_UNSUPPORTED;
    }
    /* Storage-image access requires a format with STORAGE_IMAGE support.
     * sRGB views over storage are not portable; reject early to avoid
     * driver surprises. RGBA8/BGRA8 UNORM are guaranteed by the spec;
     * RGBA16_SFLOAT (ADR-0069 working-space effects) needs the optional
     * feature bit, checked here. */
    if (fmt != OPTICS_GFX_FORMAT_RGBA8_UNORM && fmt != OPTICS_GFX_FORMAT_BGRA8_UNORM &&
        fmt != OPTICS_GFX_FORMAT_RGBA16_SFLOAT) {
        OPTICS_GFX_FAIL(OPTICS_GFX_ERROR_UNSUPPORTED,
                        "compute-writable image format not storage-compatible "
                        "(use RGBA8/BGRA8_UNORM or RGBA16_SFLOAT)");
        return OPTICS_GFX_ERROR_UNSUPPORTED;
    }
    if (fmt == OPTICS_GFX_FORMAT_RGBA16_SFLOAT) {
        VkFormatProperties props;
        vkGetPhysicalDeviceFormatProperties(d->physical_device, VK_FORMAT_R16G16B16A16_SFLOAT,
                                            &props);
        if ((props.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) == 0) {
            OPTICS_GFX_FAIL(OPTICS_GFX_ERROR_UNSUPPORTED,
                            "compute-writable RGBA16_SFLOAT needs rgba16f storage support");
            return OPTICS_GFX_ERROR_UNSUPPORTED;
        }
    }
    *out = nullptr;

    optics_gfx_image *im = optics_gfx_internal_alloc(d, sizeof(*im));
    if (!im)
        return OPTICS_GFX_ERROR_OUT_OF_MEMORY;
    atomic_init(&im->ref_count, 1u);
    im->shared = nullptr;
    im->mutable = true;
    im->imported = false;
    im->snapshot_allowed = true;
    im->color = nullptr;
    /* Effect-pool transients are owned by the device's effect state
     * and released by effect_state_destroy inside flux_device_release;
     * a strong device ref here would cycle and keep the refcount from
     * ever reaching zero (the whole device would leak). */
    im->device = d;
    im->device_weak = true;
    im->width = width;
    im->height = height;
    im->format = fmt;
    im->bindless = OPTICS_GFX_BINDLESS_INVALID;
    im->bindless_storage = OPTICS_GFX_BINDLESS_INVALID;
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
    optics_gfx_result r = optics_gfx_vk_alloc_image(d, &ici, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                                                    &im->image, &im->alloc);
    if (r != OPTICS_GFX_OK)
        goto fail;

    /* Compute writes happen in GENERAL; sampling also works from
     * GENERAL (at the cost of some driver-side optimisations the
     * SAMPLED_OPTIMAL layout enables). The transient target is
     * short-lived and re-used; GENERAL is the simpler choice. */
    r = optics_gfx_vk_transition_image_layout(d, im->image, VK_IMAGE_LAYOUT_UNDEFINED,
                                              VK_IMAGE_LAYOUT_GENERAL);
    if (r != OPTICS_GFX_OK)
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
        OPTICS_GFX_FAIL_VK(OPTICS_GFX_ERROR_BACKEND_FAILURE, "vkCreateImageView failed", vr);
        r = OPTICS_GFX_ERROR_BACKEND_FAILURE;
        goto fail;
    }

    r = optics_gfx_bindless_register_image(d, im->view, VK_IMAGE_LAYOUT_GENERAL, &im->bindless);
    if (r != OPTICS_GFX_OK)
        goto fail;

    r = optics_gfx_bindless_register_storage_image(d, im->view, VK_IMAGE_LAYOUT_GENERAL,
                                                   &im->bindless_storage);
    if (r != OPTICS_GFX_OK)
        goto fail;

    *out = im;
    OPTICS_GFX_OBSERVE_ACQUIRE(d, OPTICS_GFX_SCOPE_DEVICE, OPTICS_GFX_LIFETIME_CONTEXT, "vk_image",
                               "flux/core", 0, OPTICS_GFX_SITE_VK_IMAGE);
    return OPTICS_GFX_OK;

fail:
    if (im->bindless != OPTICS_GFX_BINDLESS_INVALID)
        optics_gfx_bindless_release(d, im->bindless);
    if (im->bindless_storage != OPTICS_GFX_BINDLESS_INVALID)
        optics_gfx_bindless_release(d, im->bindless_storage);
    if (im->view)
        vkDestroyImageView(d->device, im->view, nullptr);
    if (im->image)
        vkDestroyImage(d->device, im->image, nullptr);
    if (im->alloc.memory)
        optics_gfx_vk_deallocate(d, &im->alloc);
    optics_gfx_internal_free(d, im);
    return r;
}

optics_gfx_result optics_gfx_frame_prepare_image_target(optics_gfx_frame *f,
                                                        optics_gfx_image *target) {
    if (!f || !target)
        return OPTICS_GFX_ERROR_INVALID_ARGUMENT;
    if (!target->render_target) {
        OPTICS_GFX_FAIL(
            OPTICS_GFX_ERROR_INVALID_ARGUMENT,
            "image target was not created with flux_image_create (FLUX_IMAGE_USAGE_RENDER_TARGET)");
        return OPTICS_GFX_ERROR_INVALID_ARGUMENT;
    }
    if (f->state != OPTICS_GFX_FRAME_STATE_RECORDING || optics_gfx_frame_has_active_pass(f)) {
        OPTICS_GFX_FAIL(OPTICS_GFX_ERROR_INVALID_STATE,
                        "image target prepare outside frame pass boundary");
        return OPTICS_GFX_ERROR_INVALID_STATE;
    }
    if (target->current_layout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL)
        return OPTICS_GFX_OK;
    bool first_use = target->current_layout == VK_IMAGE_LAYOUT_UNDEFINED;
    if (!first_use && target->current_layout != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
        OPTICS_GFX_FAIL(OPTICS_GFX_ERROR_INVALID_STATE,
                        "image target is neither new nor sampleable before prepare");
        return OPTICS_GFX_ERROR_INVALID_STATE;
    }

    VkCommandBuffer cmd = optics_gfx_frame_vk_command_buffer(f);
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
    return OPTICS_GFX_OK;
}

optics_gfx_result optics_gfx_frame_finish_image_target(optics_gfx_frame *f,
                                                       optics_gfx_image *target) {
    if (!f || !target)
        return OPTICS_GFX_ERROR_INVALID_ARGUMENT;
    if (!target->render_target) {
        OPTICS_GFX_FAIL(
            OPTICS_GFX_ERROR_INVALID_ARGUMENT,
            "image target was not created with flux_image_create (FLUX_IMAGE_USAGE_RENDER_TARGET)");
        return OPTICS_GFX_ERROR_INVALID_ARGUMENT;
    }
    if (f->state != OPTICS_GFX_FRAME_STATE_RECORDING || optics_gfx_frame_has_active_pass(f)) {
        OPTICS_GFX_FAIL(OPTICS_GFX_ERROR_INVALID_STATE,
                        "image target finish outside frame pass boundary");
        return OPTICS_GFX_ERROR_INVALID_STATE;
    }
    if (target->current_layout != VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL) {
        OPTICS_GFX_FAIL(OPTICS_GFX_ERROR_INVALID_STATE,
                        "image target was not prepared for colour output");
        return OPTICS_GFX_ERROR_INVALID_STATE;
    }

    VkCommandBuffer cmd = optics_gfx_frame_vk_command_buffer(f);
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
    return OPTICS_GFX_OK;
}

/* ------------------------------------------------------------------ */
/*  Render-target image (ADR-0017)                                    */
/* ------------------------------------------------------------------ */

/* Create an image suitable as a flux_canvas_begin_target destination:
 * COLOR_ATTACHMENT | SAMPLED | TRANSFER_DST, 1 sample. Contents and layout
 * stay undefined until the first caller-recorded target pass; this avoids a
 * synchronous one-shot queue submission during frame recording. Strong device
 * ref (caller-owned, like flux_image_create — not the effect-pool weak ref). */
static optics_gfx_result image_create_render_target(optics_gfx_device *d, uint32_t width,
                                                    uint32_t height, optics_gfx_format fmt,
                                                    optics_gfx_image **out) {
    if (!d || !out)
        return OPTICS_GFX_ERROR_INVALID_ARGUMENT;
    if (width == 0 || height == 0) {
        OPTICS_GFX_FAIL(OPTICS_GFX_ERROR_INVALID_ARGUMENT, "render-target image zero-extent");
        return OPTICS_GFX_ERROR_INVALID_ARGUMENT;
    }
    VkFormat vfmt = optics_gfx_format_to_vk(fmt);
    if (vfmt == VK_FORMAT_UNDEFINED) {
        OPTICS_GFX_FAIL(OPTICS_GFX_ERROR_UNSUPPORTED, "render-target image unsupported format");
        return OPTICS_GFX_ERROR_UNSUPPORTED;
    }
    *out = nullptr;

    optics_gfx_image *im = optics_gfx_internal_alloc(d, sizeof(*im));
    if (!im)
        return OPTICS_GFX_ERROR_OUT_OF_MEMORY;
    atomic_init(&im->ref_count, 1u);
    im->shared = nullptr;
    im->mutable = true;
    im->imported = false;
    im->snapshot_allowed = true;
    im->color = nullptr;
    im->device = optics_gfx_device_retain(d);
    im->device_weak = false;
    im->width = width;
    im->height = height;
    im->format = fmt;
    im->bindless = OPTICS_GFX_BINDLESS_INVALID;
    im->bindless_storage = OPTICS_GFX_BINDLESS_INVALID;
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
                 VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    optics_gfx_result r = optics_gfx_vk_alloc_image(d, &ici, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                                                    &im->image, &im->alloc);
    if (r != OPTICS_GFX_OK)
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
        OPTICS_GFX_FAIL_VK(OPTICS_GFX_ERROR_BACKEND_FAILURE, "vkCreateImageView failed", vr);
        r = OPTICS_GFX_ERROR_BACKEND_FAILURE;
        goto fail;
    }

    r = optics_gfx_bindless_register_image(d, im->view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                           &im->bindless);
    if (r != OPTICS_GFX_OK)
        goto fail;

    *out = im;
    OPTICS_GFX_OBSERVE_ACQUIRE(d, OPTICS_GFX_SCOPE_DEVICE, OPTICS_GFX_LIFETIME_CONTEXT, "vk_image",
                               "flux/core", 0, OPTICS_GFX_SITE_VK_IMAGE);
    return OPTICS_GFX_OK;

fail:
    if (im->bindless != OPTICS_GFX_BINDLESS_INVALID)
        optics_gfx_bindless_release(d, im->bindless);
    if (im->bindless_storage != OPTICS_GFX_BINDLESS_INVALID)
        optics_gfx_bindless_release(d, im->bindless_storage);
    if (im->view)
        vkDestroyImageView(d->device, im->view, nullptr);
    if (im->image)
        vkDestroyImage(d->device, im->image, nullptr);
    if (im->alloc.memory)
        optics_gfx_vk_deallocate(d, &im->alloc);
    optics_gfx_device_release(d);
    optics_gfx_internal_free(d, im);
    return r;
}
