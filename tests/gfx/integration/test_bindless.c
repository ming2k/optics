/*
 * Bindless heap stress: register/release N images, recreate, verify
 * slots are recycled (not leaked) and handles stay valid through the
 * cycle. Skips if no Vulkan device is available.
 */
#include "gfx_test.h"
#include "optics_test.h"
#include <optics/gfx.h>
#include <optics/gfx/vulkan.h>

#define N_IMAGES 64

static optics_gfx_image *make_image(optics_gfx_device *d) {
    optics_gfx_image_desc desc = OPTICS_GFX_IMAGE_DESC_INIT;
    desc.width = 4;
    desc.height = 4;
    desc.format = OPTICS_GFX_FORMAT_RGBA8_UNORM;
    optics_gfx_image *out = nullptr;
    return optics_gfx_image_create(d, &desc, &out) == OPTICS_GFX_OK ? out : nullptr;
}

int main(void) {
    optics_gfx_device *d = optics_gfx_test_make_headless_device();
    if (!d) {
        fprintf(stderr, "test_bindless: no Vulkan device; skipping\n");
        OT_SKIP("no Vulkan device");
    }

    /* Round 1: create N, retain/release each, then release the round. */
    optics_gfx_image *images[N_IMAGES] = {};
    for (uint32_t i = 0; i < N_IMAGES; ++i) {
        images[i] = make_image(d);
        OT_EXPECT(images[i] != nullptr);
    }
    for (uint32_t i = 0; i < N_IMAGES; ++i) {
        OT_EXPECT(optics_gfx_image_retain(images[i]) == images[i]);
        optics_gfx_image_release(images[i]); /* drop the extra ref */
    }
    for (uint32_t i = 0; i < N_IMAGES; ++i)
        optics_gfx_image_release(images[i]);

    /* Round 2: re-create the same count. If round 1 leaked slots,
     * round 2 would fail at some i < N. */
    for (uint32_t i = 0; i < N_IMAGES; ++i) {
        images[i] = make_image(d);
        OT_EXPECT(images[i] != nullptr);
    }
    for (uint32_t i = 0; i < N_IMAGES; ++i)
        optics_gfx_image_release(images[i]);

    /* Round 3: NULL-safe release. */
    optics_gfx_image_release(nullptr);

    /* Round 4: retain-on-NULL returns NULL. */
    OT_EXPECT(optics_gfx_image_retain(nullptr) == nullptr);

    optics_gfx_device_release(d);
    return OT_REPORT();
}
