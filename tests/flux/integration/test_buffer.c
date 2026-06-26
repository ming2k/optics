/*
 * flux_buffer: HOST_VISIBLE map round-trip, GPU_LOCAL with initial
 * upload, device-address request, rejection paths.
 */
#include "test_helpers.h"
#include <flux/flux.h>
#include <flux/vulkan.h>

#include <string.h>

int main(void) {
    /* --- NULL / wrong-tag rejection (no device) --- */
    {
        flux_buffer *b = nullptr;
        EXPECT(flux_buffer_create(nullptr, nullptr, &b) == FLUX_ERROR_INVALID_ARGUMENT);
    }

    flux_device *d = test_helpers_make_headless_device();
    if (!d) {
        fprintf(stderr, "test_buffer: no Vulkan device; skipping\n");
        TEST_SUMMARY();
    }

    /* --- HOST_VISIBLE: map round-trip --- */
    {
        const size_t N = 64;
        float seed[N];
        for (size_t i = 0; i < N; ++i)
            seed[i] = (float)i * 0.5f;

        flux_buffer_desc bd = FLUX_BUFFER_DESC_INIT;
        bd.size = N * sizeof(float);
        bd.usage = FLUX_BUFFER_USAGE_UNIFORM;
        bd.location = FLUX_BUFFER_HOST_VISIBLE;
        bd.initial_data = seed;

        flux_buffer *b = nullptr;
        EXPECT(flux_buffer_create(d, &bd, &b) == FLUX_OK);
        EXPECT(b != nullptr);

        EXPECT(flux_buffer_size(b) == bd.size);
        EXPECT(flux_buffer_vk_buffer(b) != VK_NULL_HANDLE);

        float *mapped = flux_buffer_mapped(b);
        EXPECT(mapped != nullptr);
        for (size_t i = 0; i < N; ++i) {
            EXPECT_NEAR(mapped[i], (float)i * 0.5f, 1e-6f);
        }

        /* Write a new pattern; subsequent map reads should see it. */
        for (size_t i = 0; i < N; ++i)
            mapped[i] = -(float)i;
        for (size_t i = 0; i < N; ++i) {
            EXPECT_NEAR(mapped[i], -(float)i, 1e-6f);
        }

        EXPECT(flux_buffer_retain(b) == b);
        flux_buffer_release(b);
        flux_buffer_release(b);
    }

    /* --- GPU_LOCAL with initial_data: staging upload --- */
    {
        const size_t bytes = 4096;
        uint8_t seed[4096];
        memset(seed, 0xAB, sizeof(seed));

        flux_buffer_desc bd = FLUX_BUFFER_DESC_INIT;
        bd.size = bytes;
        bd.usage = FLUX_BUFFER_USAGE_VERTEX | FLUX_BUFFER_USAGE_TRANSFER_DST;
        bd.location = FLUX_BUFFER_GPU_LOCAL;
        bd.initial_data = seed;

        flux_buffer *b = nullptr;
        EXPECT(flux_buffer_create(d, &bd, &b) == FLUX_OK);
        EXPECT(b != nullptr);
        EXPECT(flux_buffer_mapped(b) == nullptr); /* GPU_LOCAL is not mappable */
        EXPECT(flux_buffer_vk_buffer(b) != VK_NULL_HANDLE);
        flux_buffer_release(b);
    }

    /* --- device_address: HOST_VISIBLE storage buffer with BDA --- */
    {
        flux_buffer_desc bd = FLUX_BUFFER_DESC_INIT;
        bd.size = 1024;
        bd.usage = FLUX_BUFFER_USAGE_STORAGE;
        bd.location = FLUX_BUFFER_HOST_VISIBLE;
        bd.device_address = true;

        flux_buffer *b = nullptr;
        EXPECT(flux_buffer_create(d, &bd, &b) == FLUX_OK);
        EXPECT(flux_buffer_device_address(b) != 0);
        flux_buffer_release(b);
    }

    /* --- size = 0 rejected --- */
    {
        flux_buffer_desc bd = FLUX_BUFFER_DESC_INIT;
        bd.usage = FLUX_BUFFER_USAGE_UNIFORM;
        bd.location = FLUX_BUFFER_HOST_VISIBLE;

        flux_buffer *b = nullptr;
        EXPECT(flux_buffer_create(d, &bd, &b) == FLUX_ERROR_INVALID_ARGUMENT);
    }

    /* --- wrong sType rejected --- */
    {
        flux_buffer_desc bd = {.type = FLUX_TYPE_UNKNOWN,
                               .size = 64,
                               .usage = FLUX_BUFFER_USAGE_UNIFORM,
                               .location = FLUX_BUFFER_HOST_VISIBLE};
        flux_buffer *b = nullptr;
        EXPECT(flux_buffer_create(d, &bd, &b) == FLUX_ERROR_INVALID_ARGUMENT);
    }

    /* --- NULL-safe accessors --- */
    EXPECT(flux_buffer_mapped(nullptr) == nullptr);
    EXPECT(flux_buffer_size(nullptr) == 0);
    EXPECT(flux_buffer_vk_buffer(nullptr) == VK_NULL_HANDLE);
    EXPECT(flux_buffer_device_address(nullptr) == 0);
    EXPECT(flux_buffer_retain(nullptr) == nullptr);
    flux_buffer_release(nullptr);

    flux_device_release(d);
    TEST_SUMMARY();
}
