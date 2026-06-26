/*
 * Bindless heap stress: register/release N images, recreate, verify
 * slots are recycled (not leaked) and handles stay valid through the
 * cycle. Skips if no Vulkan device is available.
 */
#include "test_helpers.h"
#include <flux/flux.h>
#include <flux/vulkan.h>

#define N_IMAGES 64

static flux_image *make_image(flux_device *d) {
    flux_image_desc desc = FLUX_IMAGE_DESC_INIT;
    desc.width = 4;
    desc.height = 4;
    desc.format = FLUX_FORMAT_RGBA8_UNORM;
    flux_image *out = nullptr;
    return flux_image_create(d, &desc, &out) == FLUX_OK ? out : nullptr;
}

int main(void) {
    flux_device *d = test_helpers_make_headless_device();
    if (!d) {
        fprintf(stderr, "test_bindless: no Vulkan device; skipping\n");
        TEST_SUMMARY();
    }

    /* Round 1: create N, retain/release each, then release the round. */
    flux_image *images[N_IMAGES] = {0};
    for (uint32_t i = 0; i < N_IMAGES; ++i) {
        images[i] = make_image(d);
        EXPECT(images[i] != nullptr);
    }
    for (uint32_t i = 0; i < N_IMAGES; ++i) {
        EXPECT(flux_image_retain(images[i]) == images[i]);
        flux_image_release(images[i]); /* drop the extra ref */
    }
    for (uint32_t i = 0; i < N_IMAGES; ++i)
        flux_image_release(images[i]);

    /* Round 2: re-create the same count. If round 1 leaked slots,
     * round 2 would fail at some i < N. */
    for (uint32_t i = 0; i < N_IMAGES; ++i) {
        images[i] = make_image(d);
        EXPECT(images[i] != nullptr);
    }
    for (uint32_t i = 0; i < N_IMAGES; ++i)
        flux_image_release(images[i]);

    /* Round 3: NULL-safe release. */
    flux_image_release(nullptr);

    /* Round 4: retain-on-NULL returns NULL. */
    EXPECT(flux_image_retain(nullptr) == nullptr);

    flux_device_release(d);
    TEST_SUMMARY();
}
