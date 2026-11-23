/*
 * Refcounted sampler with auto-bindless-registration.
 *
 * Each optics_gfx_sampler owns one VkSampler and one bindless slot. The
 * sampler is registered into the device's bindless heap at create
 * time so it can be referenced from any shader by handle. Release
 * defers both the slot release and the VkSampler destruction to the
 * device retire queue: batches in flight may still carry the slot
 * number in their push constants, so an inline release could recycle
 * the slot mid-batch (silent mis-sampling) and violates
 * VUID-vkDestroySampler-sampler-01070.
 */
#include "internal.h"
#include <optics/gfx/vulkan.h>

#include <stdatomic.h>

struct optics_gfx_sampler {
    atomic_uint ref_count;
    optics_gfx_device *device; /* retained */
    VkSampler sampler;
    optics_gfx_bindless_handle bindless;
};

static VkFilter to_vk_filter(optics_gfx_filter f) {
    return f == OPTICS_GFX_FILTER_NEAREST ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
}

static VkSamplerMipmapMode to_vk_mipmap(optics_gfx_filter f) {
    return f == OPTICS_GFX_FILTER_NEAREST ? VK_SAMPLER_MIPMAP_MODE_NEAREST
                                          : VK_SAMPLER_MIPMAP_MODE_LINEAR;
}

static VkSamplerAddressMode to_vk_address(optics_gfx_address_mode a) {
    switch (a) {
    case OPTICS_GFX_ADDRESS_REPEAT:
        return VK_SAMPLER_ADDRESS_MODE_REPEAT;
    case OPTICS_GFX_ADDRESS_CLAMP_TO_EDGE:
        return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    case OPTICS_GFX_ADDRESS_MIRRORED_REPEAT:
        return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
    case OPTICS_GFX_ADDRESS_CLAMP_TO_BORDER:
        return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    }
    return VK_SAMPLER_ADDRESS_MODE_REPEAT;
}

optics_gfx_result optics_gfx_sampler_create(optics_gfx_device *d,
                                            const optics_gfx_sampler_desc *desc,
                                            optics_gfx_sampler **out) {
    if (!d || !desc || !out)
        return OPTICS_GFX_ERROR_INVALID_ARGUMENT;
    if (desc->type != OPTICS_GFX_TYPE_SAMPLER_DESC) {
        OPTICS_GFX_FAIL(OPTICS_GFX_ERROR_INVALID_ARGUMENT,
                        "desc->type != OPTICS_GFX_TYPE_SAMPLER_DESC");
        return OPTICS_GFX_ERROR_INVALID_ARGUMENT;
    }
    *out = nullptr;

    optics_gfx_sampler *s = optics_gfx_internal_alloc(d, sizeof(*s));
    if (!s)
        return OPTICS_GFX_ERROR_OUT_OF_MEMORY;
    atomic_init(&s->ref_count, 1u);
    s->device = optics_gfx_device_retain(d);
    s->bindless = OPTICS_GFX_BINDLESS_INVALID;

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
        OPTICS_GFX_FAIL_VK(OPTICS_GFX_ERROR_BACKEND_FAILURE, "vkCreateSampler failed", vr);
        optics_gfx_device_release(d);
        optics_gfx_internal_free(d, s);
        return OPTICS_GFX_ERROR_BACKEND_FAILURE;
    }

    optics_gfx_result r = optics_gfx_bindless_register_sampler(d, s->sampler, &s->bindless);
    if (r != OPTICS_GFX_OK) {
        vkDestroySampler(d->device, s->sampler, nullptr);
        optics_gfx_device_release(d);
        optics_gfx_internal_free(d, s);
        return r;
    }

    *out = s;
    OPTICS_GFX_OBSERVE_ACQUIRE(d, OPTICS_GFX_SCOPE_DEVICE, OPTICS_GFX_LIFETIME_CONTEXT,
                               "vk_sampler", "gfx/core", 0, OPTICS_GFX_SITE_VK_SAMPLER);
    return OPTICS_GFX_OK;
}

optics_gfx_sampler *optics_gfx_sampler_retain(optics_gfx_sampler *s) {
    if (s)
        atomic_fetch_add_explicit(&s->ref_count, 1u, memory_order_relaxed);
    return s;
}

void optics_gfx_sampler_release(optics_gfx_sampler *s) {
    if (!s)
        return;
    if (atomic_fetch_sub_explicit(&s->ref_count, 1u, memory_order_acq_rel) != 1u)
        return;
    optics_gfx_device *d = s->device;
    /* The bindless slot may still be referenced by batches in flight on
     * the graphics queue (push constants recorded before this release).
     * Recycling the slot inline can reassign it to a new sampler while an
     * in-flight batch still samples through it, and destroying the
     * VkSampler mid-batch is a VUID-vkDestroySampler-sampler-01070
     * violation. Park both on the device retire queue, same contract as
     * optics_gfx_image_release / optics_gfx_buffer_release. */
    optics_gfx_device_retire_sampler(d, s->sampler, s->bindless);
    OPTICS_GFX_OBSERVE_RELEASE(d, OPTICS_GFX_SCOPE_DEVICE, "vk_sampler", "gfx/core", 0);
    optics_gfx_internal_free(d, s);
    optics_gfx_device_release(d);
}

VkSampler optics_gfx_sampler_vk_sampler(const optics_gfx_sampler *s) {
    return s ? s->sampler : VK_NULL_HANDLE;
}

optics_gfx_bindless_handle optics_gfx_sampler_bindless_handle(const optics_gfx_sampler *s) {
    return s ? s->bindless : OPTICS_GFX_BINDLESS_INVALID;
}

optics_gfx_device *optics_gfx_sampler_owner(const optics_gfx_sampler *sampler) {
    return sampler ? sampler->device : nullptr;
}
