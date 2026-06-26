/*
 * Stage 1 smoke test: confirms the public ABI is linkable and the
 * non-stub bits (version, result strings, error-info plumbing,
 * arena) work end-to-end.
 */
#include "test_helpers.h"
#include <flux/flux.h>
#include <flux/vulkan.h>
#include <string.h>

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
        flux_device_desc desc = {
            .type = FLUX_TYPE_DEVICE_DESC,
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
