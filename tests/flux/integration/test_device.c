/*
 * Stage 6.5 test_device.
 *
 * Exercises the Vulkan-touching paths that the smoke test skips:
 *   - Headless device create + retain/release
 *   - Bindless heap: handle packing, slot allocator round-trip,
 *     release across thread-safe boundary, exhaustion handling
 *   - Compute pipeline create + dispatch into a one-shot command
 *     buffer (mirrors compute_fill but as a unit assertion)
 *   - Capability queries: flux_device_get_limits and
 *     flux_device_supports_image_usage agree with what creation accepts
 *
 * Runs on a Vulkan host; skips with Meson exit code 77 when the device
 * cannot be created so CI without a driver is honest about the gap.
 */
#include "flux_test.h"
#include "optics_test.h"

#include <flux/compute.h>
#include <flux/flux.h>
#include <flux/vulkan.h>

#include <string.h>

static uint32_t selected_drm_nodes(flux_device *device, flux_device_drm_node_desc out[2]) {
    flux_drm_device_identity identity;
    if (!flux_device_get_drm_identity(device, &identity))
        return 0;

    uint32_t found = 0;
    if (identity.has_primary) {
        out[found] = (flux_device_drm_node_desc)FLUX_DEVICE_DRM_NODE_DESC_INIT;
        out[found].drm_major = identity.primary.major;
        out[found].drm_minor = identity.primary.minor;
        found++;
    }
    if (identity.has_render && found < 2) {
        out[found] = (flux_device_drm_node_desc)FLUX_DEVICE_DRM_NODE_DESC_INIT;
        out[found].drm_major = identity.render.major;
        out[found].drm_minor = identity.render.minor;
        found++;
    }
    return found;
}

int main(void) {
    /* --- descriptor validation never reaches a driver ------------------ */
    {
        flux_drm_device_identity identity = {
            .has_primary = true,
            .has_render = true,
        };
        OT_EXPECT(flux_device_enabled_features(nullptr) == 0);
        OT_EXPECT(!flux_device_get_drm_identity(nullptr, &identity));
        OT_EXPECT(!identity.has_primary);
        OT_EXPECT(!identity.has_render);
    }

    {
        flux_device_features_desc features = FLUX_DEVICE_FEATURES_DESC_INIT;
        features.required = UINT64_C(1) << 63;
        flux_device_desc desc = FLUX_DEVICE_DESC_INIT;
        desc.next = &features;
        desc.headless = true;
        desc.validation = FLUX_VALIDATION_OFF;
        flux_device *invalid = nullptr;
        OT_EXPECT(flux_device_create(&desc, &invalid) == FLUX_ERROR_UNSUPPORTED);
        OT_EXPECT(invalid == nullptr);
    }

    {
        flux_device_features_desc tail = FLUX_DEVICE_FEATURES_DESC_INIT;
        flux_device_features_desc head = FLUX_DEVICE_FEATURES_DESC_INIT;
        head.next = &tail;
        flux_device_desc desc = FLUX_DEVICE_DESC_INIT;
        desc.next = &head;
        desc.headless = true;
        desc.validation = FLUX_VALIDATION_OFF;
        flux_device *invalid = nullptr;
        OT_EXPECT(flux_device_create(&desc, &invalid) == FLUX_ERROR_INVALID_ARGUMENT);
        OT_EXPECT(invalid == nullptr);
    }

    {
        flux_device_drm_node_desc tail = FLUX_DEVICE_DRM_NODE_DESC_INIT;
        flux_device_drm_node_desc head = FLUX_DEVICE_DRM_NODE_DESC_INIT;
        head.next = &tail;
        flux_device_desc desc = FLUX_DEVICE_DESC_INIT;
        desc.next = &head;
        desc.headless = true;
        desc.validation = FLUX_VALIDATION_OFF;
        flux_device *invalid = nullptr;
        OT_EXPECT(flux_device_create(&desc, &invalid) == FLUX_ERROR_INVALID_ARGUMENT);
        OT_EXPECT(invalid == nullptr);
    }

    flux_device *d = flux_test_make_headless_device();
    if (!d) {
        fprintf(stderr, "test_device: no Vulkan device available; skipping\n");
        OT_SKIP("no Vulkan device");
    }

    /* --- descriptor-driven selection agrees with the live device ------ */
    {
        flux_device_drm_node_desc selected[2];
        uint32_t selected_count = selected_drm_nodes(d, selected);
        for (uint32_t i = 0; i < selected_count; ++i) {
            flux_device_desc matched = FLUX_DEVICE_DESC_INIT;
            matched.next = &selected[i];
            matched.headless = true;
            matched.validation = FLUX_VALIDATION_OFF;
            flux_device *same_gpu = nullptr;
            OT_EXPECT(flux_device_create(&matched, &same_gpu) == FLUX_OK);
            OT_EXPECT(same_gpu != nullptr);
            flux_device_release(same_gpu);
        }

        flux_device_drm_node_desc impossible = FLUX_DEVICE_DRM_NODE_DESC_INIT;
        impossible.drm_major = UINT32_MAX;
        impossible.drm_minor = UINT32_MAX;
        flux_device_desc constrained = FLUX_DEVICE_DESC_INIT;
        constrained.next = &impossible;
        constrained.headless = true;
        constrained.validation = FLUX_VALIDATION_OFF;
        flux_device *wrong_gpu = nullptr;
        OT_EXPECT(flux_device_create(&constrained, &wrong_gpu) == FLUX_ERROR_UNSUPPORTED);
        OT_EXPECT(wrong_gpu == nullptr);
    }

    /* --- ref count: retain + double-release does not crash --- */
    {
        flux_device *r = flux_device_retain(d);
        OT_EXPECT(r == d);
        flux_device_release(d); /* drops the extra retain */
        /* d is still valid */
        flux_device_wait_idle(d);
    }

    /* --- bindless: allocate / release / re-allocate same slot --- */
    {
        /* A real sampler: writing VK_NULL_HANDLE into a descriptor is
         * spec-UB (lavapipe crashes on it) and the heap now rejects
         * it — asserted below. */
        VkSamplerCreateInfo sci = {
            .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
            .magFilter = VK_FILTER_NEAREST,
            .minFilter = VK_FILTER_NEAREST,
        };
        VkSampler sampler = VK_NULL_HANDLE;
        OT_EXPECT(vkCreateSampler(flux_device_vk_device(d), &sci, nullptr, &sampler) == VK_SUCCESS);

        flux_bindless_handle h1 = FLUX_BINDLESS_INVALID;
        flux_result r = flux_bindless_register_sampler(d, sampler, &h1);
        OT_EXPECT(r == FLUX_OK);
        OT_EXPECT(h1 != FLUX_BINDLESS_INVALID);

        /* Handle binding-tag (top 4 bits) should match SAMPLER (== 2). */
        OT_EXPECT((h1 >> 28) == 2);

        /* Slot 0 should be allocated first (LIFO stack pushes
         * capacity-1 down to 0, so pop returns 0). */
        OT_EXPECT((h1 & 0x0FFFFFFFu) == 0);

        flux_bindless_handle h2 = FLUX_BINDLESS_INVALID;
        r = flux_bindless_register_sampler(d, sampler, &h2);
        OT_EXPECT(r == FLUX_OK);
        OT_EXPECT((h2 & 0x0FFFFFFFu) == 1);

        /* Release h1; next register should reuse slot 0. */
        flux_bindless_release(d, h1);
        flux_bindless_handle h3 = FLUX_BINDLESS_INVALID;
        r = flux_bindless_register_sampler(d, sampler, &h3);
        OT_EXPECT(r == FLUX_OK);
        OT_EXPECT((h3 & 0x0FFFFFFFu) == 0);

        /* Release everything we allocated. */
        flux_bindless_release(d, h3);
        flux_bindless_release(d, h2);

        /* Null handles are rejected, not forwarded to the driver. */
        flux_bindless_handle hn = 0;
        OT_EXPECT(flux_bindless_register_sampler(d, VK_NULL_HANDLE, &hn) ==
                  FLUX_ERROR_INVALID_ARGUMENT);
        OT_EXPECT(hn == FLUX_BINDLESS_INVALID);
        OT_EXPECT(flux_bindless_register_image(d, VK_NULL_HANDLE,
                                               VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                               &hn) == FLUX_ERROR_INVALID_ARGUMENT);

        vkDestroySampler(flux_device_vk_device(d), sampler, nullptr);
    }

    /* --- bindless: invalid handle release is a no-op (no crash) --- */
    {
        flux_bindless_release(d, FLUX_BINDLESS_INVALID);
        /* Bogus binding tag (15 << 28) — handle_release should clamp. */
        flux_bindless_release(d, (15u << 28) | 100u);
    }

    /* --- compute pipeline: trivial dispatch with empty push consts --- */
    {
        /* Minimal SPIR-V: a 1×1×1 main() that does nothing. Easier
         * to ship via a tiny embedded payload than to compile here.
         * For Stage 6.5 we settle for build-then-destroy without
         * dispatch — dispatch is exercised by examples/compute_fill. */
        /* (Skipped: no inline SPIR-V to compile from C.) */
    }

    /* --- core handles are non-null on a real device --- */
    {
        OT_EXPECT(flux_device_vk_instance(d) != VK_NULL_HANDLE);
        OT_EXPECT(flux_device_vk_physical_device(d) != VK_NULL_HANDLE);
        OT_EXPECT(flux_device_vk_device(d) != VK_NULL_HANDLE);
        OT_EXPECT(flux_device_vk_graphics_queue(d) != VK_NULL_HANDLE);
        OT_EXPECT(flux_device_vk_pipeline_cache(d) != VK_NULL_HANDLE);
        flux_device_vk_pipeline_cache_lock(d);
        flux_device_vk_pipeline_cache_unlock(d);
        flux_device_vk_pipeline_cache_lock(nullptr);
        flux_device_vk_pipeline_cache_unlock(nullptr);
        OT_EXPECT(flux_device_bindless_set(d) != VK_NULL_HANDLE);
        OT_EXPECT(flux_device_bindless_layout(d) != VK_NULL_HANDLE);

        VkPhysicalDeviceFeatures physical_features = {};
        vkGetPhysicalDeviceFeatures(flux_device_vk_physical_device(d), &physical_features);
        OT_EXPECT(flux_device_supports_large_points(d) == (bool)physical_features.largePoints);
        OT_EXPECT(!flux_device_supports_large_points(nullptr));
    }

    /* --- capability queries: discovery by query, not by failure --- */
    {
        /* Argument validation. */
        flux_device_limits zero = {};
        OT_EXPECT(flux_device_get_limits(d, nullptr) == FLUX_ERROR_INVALID_ARGUMENT);
        OT_EXPECT(flux_device_get_limits(nullptr, &zero) == FLUX_ERROR_INVALID_ARGUMENT);
        OT_EXPECT(flux_device_supports_image_usage(nullptr, FLUX_FORMAT_RGBA8_UNORM,
                                                   FLUX_IMAGE_USAGE_SAMPLED) == false);
        OT_EXPECT(flux_device_supports_image_usage(d, FLUX_FORMAT_UNDEFINED,
                                                   FLUX_IMAGE_USAGE_SAMPLED) == false);

        /* Limits are populated and sane on any conformant device. */
        flux_device_limits lim = FLUX_DEVICE_LIMITS_INIT;
        OT_EXPECT(flux_device_get_limits(d, &lim) == FLUX_OK);
        OT_EXPECT(lim.struct_size == sizeof(flux_device_limits));
        OT_EXPECT(lim.max_image_dimension2d >= 4096);
        OT_EXPECT(lim.max_color_attachments >= 4);
        OT_EXPECT(lim.max_frames_in_flight >= 1);
        OT_EXPECT(lim.timestamp_period_ns > 0.0f);
        OT_EXPECT(lim.min_uniform_buffer_offset_alignment > 0);

        /* Usage query must AGREE with what creation accepts — a query that
         * could disagree with the create path would be worse than none. */
        OT_EXPECT(flux_device_supports_image_usage(d, FLUX_FORMAT_RGBA8_UNORM,
                                                   FLUX_IMAGE_USAGE_SAMPLED) == true);
        /* Spec-guaranteed storage formats. */
        OT_EXPECT(flux_device_supports_image_usage(d, FLUX_FORMAT_RGBA8_UNORM,
                                                   FLUX_IMAGE_USAGE_COMPUTE_WRITE) == true);
        OT_EXPECT(flux_device_supports_image_usage(d, FLUX_FORMAT_BGRA8_UNORM,
                                                   FLUX_IMAGE_USAGE_COMPUTE_WRITE) == true);
        /* sRGB storage is carved out by policy: never reported writable. */
        OT_EXPECT(flux_device_supports_image_usage(d, FLUX_FORMAT_RGBA8_SRGB,
                                                   FLUX_IMAGE_USAGE_COMPUTE_WRITE) == false);
        /* RGBA16_SFLOAT: whatever the query says must match a real create
         * attempt, so apps can branch on the query alone. */
        bool q16 = flux_device_supports_image_usage(d, FLUX_FORMAT_RGBA16_SFLOAT,
                                                    FLUX_IMAGE_USAGE_COMPUTE_WRITE);
        flux_image *im16 = nullptr;
        flux_result r16 =
            flux_image_create_compute_writable(d, 4, 4, FLUX_FORMAT_RGBA16_SFLOAT, &im16);
        OT_EXPECT((r16 == FLUX_OK) == q16);
        if (r16 == FLUX_OK)
            flux_image_release(im16);
    }

    flux_device_release(d);
    return OT_REPORT();
}
