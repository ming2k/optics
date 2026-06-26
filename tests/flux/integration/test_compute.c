/*
 * Test compute pipeline create/release: input validation and a
 * minimal happy path. Validation paths run without a device (NULL d
 * short-circuits to FLUX_ERROR_INVALID_ARGUMENT). The
 * push-bytes-too-large path needs a real device for d->props; we
 * skip when no Vulkan is available.
 */
#include "test_helpers.h"
#include <flux/compute.h>
#include <flux/flux.h>

/* Trivial SPIR-V is not embeddable inline; the real-shader path is
 * exercised by examples/compute_fill. This unit test covers only the
 * input-validation rejections that don't reach the driver. */

int main(void) {
    /* --- NULL device / desc / out reject without touching anything --- */
    {
        flux_compute_pipeline *p = nullptr;
        EXPECT(flux_compute_pipeline_create(nullptr, nullptr, &p) == FLUX_ERROR_INVALID_ARGUMENT);
        EXPECT(p == nullptr);

        flux_compute_pipeline_desc desc = {
            .type = FLUX_TYPE_COMPUTE_PIPELINE_DESC,
            .spirv_word_count = 4,
            .push_constant_bytes = 0,
        };
        /* d=NULL → INVALID_ARGUMENT, regardless of desc contents. */
        EXPECT(flux_compute_pipeline_create(nullptr, &desc, &p) == FLUX_ERROR_INVALID_ARGUMENT);
    }

    flux_device *d = test_helpers_make_headless_device();
    if (!d) {
        fprintf(stderr, "test_compute: no Vulkan device available; skipping device-bound checks\n");
        TEST_SUMMARY();
    }

    /* --- wrong tagged-struct type --- */
    {
        flux_compute_pipeline *p = nullptr;
        uint32_t spv[4] = {0};
        flux_compute_pipeline_desc desc = {
            .type = FLUX_TYPE_UNKNOWN, /* deliberately wrong */
            .spirv = spv,
            .spirv_word_count = 4,
        };
        EXPECT(flux_compute_pipeline_create(d, &desc, &p) == FLUX_ERROR_INVALID_ARGUMENT);
        EXPECT(p == nullptr);

        flux_error_info info;
        flux_get_last_error(&info);
        EXPECT(info.code == FLUX_ERROR_INVALID_ARGUMENT);
    }

    /* --- missing SPIR-V --- */
    {
        flux_compute_pipeline *p = nullptr;
        flux_compute_pipeline_desc desc = {
            .type = FLUX_TYPE_COMPUTE_PIPELINE_DESC,
            .spirv = nullptr,
            .spirv_word_count = 0,
        };
        EXPECT(flux_compute_pipeline_create(d, &desc, &p) == FLUX_ERROR_INVALID_ARGUMENT);
        EXPECT(p == nullptr);
    }

    /* --- push constants larger than device-reported max --- */
    {
        flux_compute_pipeline *p = nullptr;
        uint32_t spv[4] = {0};
        flux_compute_pipeline_desc desc = {
            .type = FLUX_TYPE_COMPUTE_PIPELINE_DESC,
            .spirv = spv,
            .spirv_word_count = 4,
            .push_constant_bytes = 1u << 20, /* 1 MiB — no driver allows this */
        };
        EXPECT(flux_compute_pipeline_create(d, &desc, &p) == FLUX_ERROR_INVALID_ARGUMENT);
        EXPECT(p == nullptr);
    }

    /* --- release-NULL is a no-op --- */
    flux_compute_pipeline_release(nullptr);

    /* --- vk accessors on NULL return VK_NULL_HANDLE --- */
    EXPECT(flux_compute_pipeline_vk_pipeline(nullptr) == VK_NULL_HANDLE);
    EXPECT(flux_compute_pipeline_vk_layout(nullptr) == VK_NULL_HANDLE);

    flux_device_release(d);
    TEST_SUMMARY();
}
