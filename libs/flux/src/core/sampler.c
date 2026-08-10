/*
 * Refcounted sampler with auto-bindless-registration.
 *
 * Each flux_sampler owns one VkSampler and one bindless slot. The
 * sampler is registered into the device's bindless heap at create
 * time so it can be referenced from any shader by handle. Release
 * defers both the slot release and the VkSampler destruction to the
 * device retire queue: batches in flight may still carry the slot
 * number in their push constants, so an inline release could recycle
 * the slot mid-batch (silent mis-sampling) and violates
 * VUID-vkDestroySampler-sampler-01070.
 */
#include "internal.h"
#include <flux/vulkan.h>

#include <stdatomic.h>

struct flux_sampler {
    atomic_uint ref_count;
    flux_device *device; /* retained */
    VkSampler sampler;
    flux_bindless_handle bindless;
};

static VkFilter to_vk_filter(flux_filter f) {
    return f == FLUX_FILTER_NEAREST ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
}

static VkSamplerMipmapMode to_vk_mipmap(flux_filter f) {
    return f == FLUX_FILTER_NEAREST ? VK_SAMPLER_MIPMAP_MODE_NEAREST
                                    : VK_SAMPLER_MIPMAP_MODE_LINEAR;
}

static VkSamplerAddressMode to_vk_address(flux_address_mode a) {
    switch (a) {
    case FLUX_ADDRESS_REPEAT:
        return VK_SAMPLER_ADDRESS_MODE_REPEAT;
    case FLUX_ADDRESS_CLAMP_TO_EDGE:
        return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    case FLUX_ADDRESS_MIRRORED_REPEAT:
        return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
    case FLUX_ADDRESS_CLAMP_TO_BORDER:
        return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    }
    return VK_SAMPLER_ADDRESS_MODE_REPEAT;
}

flux_result flux_sampler_create(flux_device *d, const flux_sampler_desc *desc, flux_sampler **out) {
    if (!d || !desc || !out)
        return FLUX_ERROR_INVALID_ARGUMENT;
    if (desc->type != FLUX_TYPE_SAMPLER_DESC) {
        FLUX_FAIL(FLUX_ERROR_INVALID_ARGUMENT, "desc->type != FLUX_TYPE_SAMPLER_DESC");
        return FLUX_ERROR_INVALID_ARGUMENT;
    }
    *out = nullptr;

    flux_sampler *s = flux_internal_alloc(d, sizeof(*s));
    if (!s)
        return FLUX_ERROR_OUT_OF_MEMORY;
    atomic_init(&s->ref_count, 1u);
    s->device = flux_device_retain(d);
    s->bindless = FLUX_BINDLESS_INVALID;

    /* Clamp anisotropy to the device cap. 1.0 (or below) disables. */
    float aniso = desc->max_anisotropy;
    if (aniso < 1.0f)
        aniso = 1.0f;
    float max_cap = d->props.limits.maxSamplerAnisotropy;
    if (aniso > max_cap)
        aniso = max_cap;

    VkSamplerCreateInfo sci = {
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter = to_vk_filter(desc->mag_filter),
        .minFilter = to_vk_filter(desc->min_filter),
        .mipmapMode = to_vk_mipmap(desc->mipmap_mode),
        .addressModeU = to_vk_address(desc->address_u),
        .addressModeV = to_vk_address(desc->address_v),
        .addressModeW = to_vk_address(desc->address_w),
        .mipLodBias = 0.0f,
        .anisotropyEnable = aniso > 1.0f ? VK_TRUE : VK_FALSE,
        .maxAnisotropy = aniso,
        .compareEnable = VK_FALSE,
        .minLod = 0.0f,
        .maxLod = VK_LOD_CLAMP_NONE,
        .borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK,
    };
    VkResult vr = vkCreateSampler(d->device, &sci, nullptr, &s->sampler);
    if (vr != VK_SUCCESS) {
        FLUX_FAIL_VK(FLUX_ERROR_BACKEND_FAILURE, "vkCreateSampler failed", vr);
        flux_device_release(d);
        flux_internal_free(d, s);
        return FLUX_ERROR_BACKEND_FAILURE;
    }

    flux_result r = flux_bindless_register_sampler(d, s->sampler, &s->bindless);
    if (r != FLUX_OK) {
        vkDestroySampler(d->device, s->sampler, nullptr);
        flux_device_release(d);
        flux_internal_free(d, s);
        return r;
    }

    *out = s;
    return FLUX_OK;
}

flux_sampler *flux_sampler_retain(flux_sampler *s) {
    if (s)
        atomic_fetch_add_explicit(&s->ref_count, 1u, memory_order_relaxed);
    return s;
}

void flux_sampler_release(flux_sampler *s) {
    if (!s)
        return;
    if (atomic_fetch_sub_explicit(&s->ref_count, 1u, memory_order_acq_rel) != 1u)
        return;
    flux_device *d = s->device;
    /* The bindless slot may still be referenced by batches in flight on
     * the graphics queue (push constants recorded before this release).
     * Recycling the slot inline can reassign it to a new sampler while an
     * in-flight batch still samples through it, and destroying the
     * VkSampler mid-batch is a VUID-vkDestroySampler-sampler-01070
     * violation. Park both on the device retire queue, same contract as
     * flux_image_release / flux_buffer_release. */
    flux_device_retire_sampler(d, s->sampler, s->bindless);
    flux_internal_free(d, s);
    flux_device_release(d);
}

VkSampler flux_sampler_vk_sampler(const flux_sampler *s) {
    return s ? s->sampler : VK_NULL_HANDLE;
}

flux_bindless_handle flux_sampler_bindless_handle(const flux_sampler *s) {
    return s ? s->bindless : FLUX_BINDLESS_INVALID;
}
