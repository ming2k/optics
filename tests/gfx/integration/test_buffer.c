/*
 * flux_buffer: HOST_VISIBLE map round-trip, GPU_LOCAL with initial
 * upload, device-address request, rejection paths.
 */
#include "gfx_test.h"
#include "optics_test.h"
#include <optics/gfx.h>
#include <optics/gfx/vulkan.h>

#include <string.h>

int main(void) {
    /* --- NULL / wrong-tag rejection (no device) --- */
    {
        optics_gfx_buffer *b = nullptr;
        OT_EXPECT(optics_gfx_buffer_create(nullptr, nullptr, &b) ==
                  OPTICS_GFX_ERROR_INVALID_ARGUMENT);
    }

    optics_gfx_device *d = optics_gfx_test_make_headless_device();
    if (!d) {
        fprintf(stderr, "test_buffer: no Vulkan device; skipping\n");
        OT_SKIP("no Vulkan device");
    }

    /* --- HOST_VISIBLE: map round-trip --- */
    {
        const size_t N = 64;
        float seed[N];
        for (size_t i = 0; i < N; ++i)
            seed[i] = (float)i * 0.5f;

        optics_gfx_buffer_desc bd = OPTICS_GFX_BUFFER_DESC_INIT;
        bd.size = N * sizeof(float);
        bd.usage = OPTICS_GFX_BUFFER_USAGE_UNIFORM;
        bd.location = OPTICS_GFX_BUFFER_HOST_VISIBLE;
        bd.initial_data = seed;

        optics_gfx_buffer *b = nullptr;
        OT_EXPECT(optics_gfx_buffer_create(d, &bd, &b) == OPTICS_GFX_OK);
        OT_EXPECT(b != nullptr);

        OT_EXPECT(optics_gfx_buffer_size(b) == bd.size);
        OT_EXPECT(optics_gfx_buffer_vk_buffer(b) != VK_NULL_HANDLE);

        float *mapped = optics_gfx_buffer_mapped(b);
        OT_EXPECT(mapped != nullptr);
        for (size_t i = 0; i < N; ++i) {
            OT_EXPECT_NEAR(mapped[i], (float)i * 0.5f, 1e-6f);
        }

        /* Write a new pattern; subsequent map reads should see it. */
        for (size_t i = 0; i < N; ++i)
            mapped[i] = -(float)i;
        for (size_t i = 0; i < N; ++i) {
            OT_EXPECT_NEAR(mapped[i], -(float)i, 1e-6f);
        }

        OT_EXPECT(optics_gfx_buffer_retain(b) == b);
        optics_gfx_buffer_release(b);
        optics_gfx_buffer_release(b);
    }

    /* --- GPU_LOCAL with initial_data: staging upload --- */
    {
        const size_t bytes = 4096;
        uint8_t seed[4096];
        memset(seed, 0xAB, sizeof(seed));

        optics_gfx_buffer_desc bd = OPTICS_GFX_BUFFER_DESC_INIT;
        bd.size = bytes;
        bd.usage = OPTICS_GFX_BUFFER_USAGE_VERTEX | OPTICS_GFX_BUFFER_USAGE_TRANSFER_DST;
        bd.location = OPTICS_GFX_BUFFER_GPU_LOCAL;
        bd.initial_data = seed;

        optics_gfx_buffer *b = nullptr;
        OT_EXPECT(optics_gfx_buffer_create(d, &bd, &b) == OPTICS_GFX_OK);
        OT_EXPECT(b != nullptr);
        OT_EXPECT(optics_gfx_buffer_mapped(b) == nullptr); /* GPU_LOCAL is not mappable */
        OT_EXPECT(optics_gfx_buffer_vk_buffer(b) != VK_NULL_HANDLE);
        optics_gfx_buffer_release(b);
    }

    /* --- device_address: HOST_VISIBLE storage buffer with BDA --- */
    {
        optics_gfx_buffer_desc bd = OPTICS_GFX_BUFFER_DESC_INIT;
        bd.size = 1024;
        bd.usage = OPTICS_GFX_BUFFER_USAGE_STORAGE;
        bd.location = OPTICS_GFX_BUFFER_HOST_VISIBLE;
        bd.device_address = true;

        optics_gfx_buffer *b = nullptr;
        OT_EXPECT(optics_gfx_buffer_create(d, &bd, &b) == OPTICS_GFX_OK);
        OT_EXPECT(optics_gfx_buffer_device_address(b) != 0);
        optics_gfx_buffer_release(b);
    }

    /* --- size = 0 rejected --- */
    {
        optics_gfx_buffer_desc bd = OPTICS_GFX_BUFFER_DESC_INIT;
        bd.usage = OPTICS_GFX_BUFFER_USAGE_UNIFORM;
        bd.location = OPTICS_GFX_BUFFER_HOST_VISIBLE;

        optics_gfx_buffer *b = nullptr;
        OT_EXPECT(optics_gfx_buffer_create(d, &bd, &b) == OPTICS_GFX_ERROR_INVALID_ARGUMENT);
    }

    /* --- wrong sType rejected --- */
    {
        optics_gfx_buffer_desc bd = {.type = OPTICS_GFX_TYPE_UNKNOWN,
                                     .size = 64,
                                     .usage = OPTICS_GFX_BUFFER_USAGE_UNIFORM,
                                     .location = OPTICS_GFX_BUFFER_HOST_VISIBLE};
        optics_gfx_buffer *b = nullptr;
        OT_EXPECT(optics_gfx_buffer_create(d, &bd, &b) == OPTICS_GFX_ERROR_INVALID_ARGUMENT);
    }

    /* --- NULL-safe accessors --- */
    OT_EXPECT(optics_gfx_buffer_mapped(nullptr) == nullptr);
    OT_EXPECT(optics_gfx_buffer_size(nullptr) == 0);
    OT_EXPECT(optics_gfx_buffer_vk_buffer(nullptr) == VK_NULL_HANDLE);
    OT_EXPECT(optics_gfx_buffer_device_address(nullptr) == 0);
    OT_EXPECT(optics_gfx_buffer_retain(nullptr) == nullptr);
    optics_gfx_buffer_release(nullptr);

    optics_gfx_device_release(d);
    return OT_REPORT();
}
