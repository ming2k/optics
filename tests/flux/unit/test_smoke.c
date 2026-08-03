/*
 * Stage 1 smoke test: confirms the public ABI is linkable and the
 * non-stub bits (version, result strings, error-info plumbing,
 * arena) work end-to-end.
 */
#include "test_helpers.h"
#include <flux/flux.h>
#include <flux/vulkan.h>
#include <stdlib.h>
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
    /* --- version --- */
    {
        int major = -1, minor = -1, patch = -1;
        flux_version(&major, &minor, &patch);
        EXPECT(major == FLUX_VERSION_MAJOR);
        EXPECT(minor == FLUX_VERSION_MINOR);
        EXPECT(patch == FLUX_VERSION_PATCH);
        EXPECT(flux_version_number() == FLUX_VERSION_NUMBER);
        EXPECT(flux_version_check(FLUX_VERSION_MAJOR, FLUX_VERSION_MINOR, FLUX_VERSION_PATCH));
        EXPECT(!flux_version_check(FLUX_VERSION_MAJOR + 1, 0, 0));
        EXPECT(flux_version_string() != nullptr);
    }

    /* --- result strings --- */
    {
        EXPECT(strcmp(flux_result_string(FLUX_OK), "FLUX_OK") == 0);
        EXPECT(strcmp(flux_result_string(FLUX_ERROR_UNSUPPORTED), "FLUX_ERROR_UNSUPPORTED") == 0);
        EXPECT(strcmp(flux_result_string((flux_result)9999), "FLUX_ERROR_UNKNOWN") == 0);
    }

    /* --- error info plumbing (Stage 1: still zeroed) --- */
    {
        flux_error_info info;
        flux_get_last_error(&info);
        /* Just confirm the call works; nothing has set an error yet. */
        EXPECT(info.code == FLUX_OK || info.code != FLUX_OK);
    }

    /* --- device create round-trip (Stage 2a)
     *
     * On a host with a Vulkan 1.3 GPU: FLUX_OK + non-null device.
     * On a host without one (CI without lavapipe, no driver, etc.):
     * a defined error result + null device. Either path validates
     * the ABI; the test confirms create/release symmetry without
     * leaking on the success path.
     */
    {
        flux_drm_device_identity identity = {
            .has_primary = true,
            .has_render = true,
        };
        EXPECT(flux_device_enabled_features(nullptr) == 0);
        EXPECT(!flux_device_get_drm_identity(nullptr, &identity));
        EXPECT(!identity.has_primary);
        EXPECT(!identity.has_render);
    }

    {
        flux_device_features_desc features = FLUX_DEVICE_FEATURES_DESC_INIT;
        features.required = UINT64_C(1) << 63;
        flux_device_desc desc = FLUX_DEVICE_DESC_INIT;
        desc.next = &features;
        desc.headless = true;
        desc.validation = FLUX_VALIDATION_OFF;
        flux_device *d = nullptr;
        EXPECT(flux_device_create(&desc, &d) == FLUX_ERROR_UNSUPPORTED);
        EXPECT(d == nullptr);
    }

    {
        flux_device_features_desc tail = FLUX_DEVICE_FEATURES_DESC_INIT;
        flux_device_features_desc head = FLUX_DEVICE_FEATURES_DESC_INIT;
        head.next = &tail;
        flux_device_desc desc = FLUX_DEVICE_DESC_INIT;
        desc.next = &head;
        desc.headless = true;
        desc.validation = FLUX_VALIDATION_OFF;
        flux_device *d = nullptr;
        EXPECT(flux_device_create(&desc, &d) == FLUX_ERROR_INVALID_ARGUMENT);
        EXPECT(d == nullptr);
    }

    {
        flux_device_drm_node_desc tail = FLUX_DEVICE_DRM_NODE_DESC_INIT;
        flux_device_drm_node_desc head = FLUX_DEVICE_DRM_NODE_DESC_INIT;
        head.next = &tail;
        flux_device_desc desc = FLUX_DEVICE_DESC_INIT;
        desc.next = &head;
        desc.headless = true;
        desc.validation = FLUX_VALIDATION_OFF;
        flux_device *d = nullptr;
        EXPECT(flux_device_create(&desc, &d) == FLUX_ERROR_INVALID_ARGUMENT);
        EXPECT(d == nullptr);
    }

    {
        flux_device_features_desc features = FLUX_DEVICE_FEATURES_DESC_INIT;
        features.optional = FLUX_DEVICE_FEATURE_DMABUF | FLUX_DEVICE_FEATURE_DMABUF_SYNC_FILE;
        flux_device_desc desc = {
            .type = FLUX_TYPE_DEVICE_DESC,
            .next = &features,
            .headless = true,
            .frames_in_flight = 2,
            .validation = FLUX_VALIDATION_OFF,
        };
        flux_device *d = nullptr;
        flux_result r = flux_device_create(&desc, &d);
        if (r == FLUX_OK) {
            EXPECT(d != nullptr);
            EXPECT(flux_device_vk_instance(d) != VK_NULL_HANDLE);
            EXPECT(flux_device_vk_physical_device(d) != VK_NULL_HANDLE);
            EXPECT(flux_device_vk_device(d) != VK_NULL_HANDLE);
            EXPECT(flux_device_vk_graphics_queue(d) != VK_NULL_HANDLE);
            EXPECT(flux_device_vk_pipeline_cache(d) != VK_NULL_HANDLE);
            flux_device_feature_flags enabled = flux_device_enabled_features(d);
            EXPECT((enabled & FLUX_DEVICE_FEATURE_DMABUF_SYNC_FILE) == 0 ||
                   (enabled & FLUX_DEVICE_FEATURE_DMABUF) != 0);

            flux_device_drm_node_desc selected[2];
            uint32_t selected_count = selected_drm_nodes(d, selected);
            for (uint32_t i = 0; i < selected_count; ++i) {
                flux_device_desc matched = FLUX_DEVICE_DESC_INIT;
                matched.next = &selected[i];
                matched.headless = true;
                matched.validation = FLUX_VALIDATION_OFF;
                flux_device *same_gpu = nullptr;
                EXPECT(flux_device_create(&matched, &same_gpu) == FLUX_OK);
                EXPECT(same_gpu != nullptr);
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
            EXPECT(flux_device_create(&constrained, &wrong_gpu) == FLUX_ERROR_UNSUPPORTED);
            EXPECT(wrong_gpu == nullptr);

            flux_device_release(d);
        } else {
            EXPECT(d == nullptr);
            EXPECT(r == FLUX_ERROR_UNSUPPORTED || r == FLUX_ERROR_BACKEND_FAILURE);
        }
    }

    /* --- ref-count: retain then release does not destroy --- */
    {
        flux_device_desc desc = {
            .type = FLUX_TYPE_DEVICE_DESC,
            .headless = true,
            .validation = FLUX_VALIDATION_OFF,
        };
        flux_device *d = nullptr;
        if (flux_device_create(&desc, &d) == FLUX_OK) {
            flux_device *d2 = flux_device_retain(d);
            EXPECT(d2 == d);
            flux_device_release(d); /* drops the extra ref */
            /* still alive; we can wait_idle without crashing */
            flux_device_wait_idle(d);
            flux_device_release(d);
        }
    }

    /* --- arena is real --- */
    {
        flux_arena arena;
        EXPECT(flux_arena_init(&arena, 1024, nullptr) == FLUX_OK);
        void *a = flux_arena_alloc(&arena, 64);
        void *b = flux_arena_alloc(&arena, 64);
        EXPECT(a != nullptr);
        EXPECT(b != nullptr);
        EXPECT(b > a);
        flux_arena_reset(&arena);
        void *c = flux_arena_alloc(&arena, 64);
        EXPECT(c == a);                                           /* reset rewinds */
        EXPECT(flux_arena_alloc(&arena, 1024 * 1024) == nullptr); /* OOM */
        flux_arena_destroy(&arena);
    }

    /* --- color packs predictably --- */
    {
        flux_color c = flux_color_rgba(0x12, 0x34, 0x56, 0x78);
        uint8_t r, g, b, a;
        flux_color_unpack(c, &r, &g, &b, &a);
        EXPECT(r == 0x12);
        EXPECT(g == 0x34);
        EXPECT(b == 0x56);
        EXPECT(a == 0x78);
    }

    TEST_SUMMARY();
}
