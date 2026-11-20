/*
 * Test compute pipeline create/release: input validation and a
 * minimal happy path. Validation paths run without a device (NULL d
 * short-circuits to FLUX_ERROR_INVALID_ARGUMENT). The
 * push-bytes-too-large path needs a real device for d->props; we
 * skip when no Vulkan is available.
 */
#include "gfx_test.h"
#include "optics_test.h"
#include <optics/gfx.h>
#include <optics/gfx/compute.h>

/* Trivial SPIR-V is not embeddable inline; the real-shader path is
 * exercised by examples/compute_fill. This unit test covers only the
 * input-validation rejections that don't reach the driver. */

int main(void) {
    /* --- NULL device / desc / out reject without touching anything --- */
    {
        optics_gfx_compute_pipeline *p = nullptr;
        OT_EXPECT(optics_gfx_compute_pipeline_create(nullptr, nullptr, &p) ==
                  OPTICS_GFX_ERROR_INVALID_ARGUMENT);
        OT_EXPECT(p == nullptr);

        optics_gfx_compute_pipeline_desc desc = {
            .type = OPTICS_GFX_TYPE_COMPUTE_PIPELINE_DESC,
            .spirv_word_count = 4,
            .push_constant_bytes = 0,
        };
        /* d=NULL → INVALID_ARGUMENT, regardless of desc contents. */
        OT_EXPECT(optics_gfx_compute_pipeline_create(nullptr, &desc, &p) ==
                  OPTICS_GFX_ERROR_INVALID_ARGUMENT);
    }

    optics_gfx_device *d = optics_gfx_test_make_headless_device();
    if (!d) {
        fprintf(stderr, "test_compute: no Vulkan device available; skipping device-bound checks\n");
        OT_SKIP("no Vulkan device");
    }

    /* --- wrong tagged-struct type --- */
    {
        optics_gfx_compute_pipeline *p = nullptr;
        uint32_t spv[4] = {};
        optics_gfx_compute_pipeline_desc desc = {
            .type = OPTICS_GFX_TYPE_UNKNOWN, /* deliberately wrong */
            .spirv = spv,
            .spirv_word_count = 4,
        };
        OT_EXPECT(optics_gfx_compute_pipeline_create(d, &desc, &p) ==
                  OPTICS_GFX_ERROR_INVALID_ARGUMENT);
        OT_EXPECT(p == nullptr);

        optics_gfx_error_info info;
        optics_gfx_get_last_error(&info);
        OT_EXPECT(info.code == OPTICS_GFX_ERROR_INVALID_ARGUMENT);
    }

    /* --- missing SPIR-V --- */
    {
        optics_gfx_compute_pipeline *p = nullptr;
        optics_gfx_compute_pipeline_desc desc = {
            .type = OPTICS_GFX_TYPE_COMPUTE_PIPELINE_DESC,
            .spirv = nullptr,
            .spirv_word_count = 0,
        };
        OT_EXPECT(optics_gfx_compute_pipeline_create(d, &desc, &p) ==
                  OPTICS_GFX_ERROR_INVALID_ARGUMENT);
        OT_EXPECT(p == nullptr);
    }

    /* --- push constants larger than device-reported max --- */
    {
        optics_gfx_compute_pipeline *p = nullptr;
        uint32_t spv[4] = {};
        optics_gfx_compute_pipeline_desc desc = {
            .type = OPTICS_GFX_TYPE_COMPUTE_PIPELINE_DESC,
            .spirv = spv,
            .spirv_word_count = 4,
            .push_constant_bytes = 1u << 20, /* 1 MiB — no driver allows this */
        };
        OT_EXPECT(optics_gfx_compute_pipeline_create(d, &desc, &p) ==
                  OPTICS_GFX_ERROR_INVALID_ARGUMENT);
        OT_EXPECT(p == nullptr);
    }

    /* --- release-NULL is a no-op --- */
    optics_gfx_compute_pipeline_release(nullptr);

    /* --- vk accessors on NULL return VK_NULL_HANDLE --- */
    OT_EXPECT(optics_gfx_compute_pipeline_vk_pipeline(nullptr) == VK_NULL_HANDLE);
    OT_EXPECT(optics_gfx_compute_pipeline_vk_layout(nullptr) == VK_NULL_HANDLE);

    optics_gfx_device_release(d);
    return OT_REPORT();
}
