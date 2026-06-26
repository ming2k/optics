/*
 * flux_image_update_region: bounds checks + happy-path upload of a
 * sub-region into an existing image. Skips if no Vulkan device.
 */
#include "test_helpers.h"
#include <flux/flux.h>
#include <flux/vulkan.h>

#include <string.h>

#define IMG_W 16u
#define IMG_H 16u

static flux_image *make_image(flux_device *d, uint32_t *initial) {
    flux_image_desc desc = FLUX_IMAGE_DESC_INIT;
    desc.width = IMG_W;
    desc.height = IMG_H;
    desc.format = FLUX_FORMAT_RGBA8_UNORM;
    desc.initial_data = initial;
    flux_image *out = nullptr;
    return flux_image_create(d, &desc, &out) == FLUX_OK ? out : nullptr;
}

int main(void) {
    /* --- NULL safety --- */
    EXPECT(flux_image_update_region(nullptr, 0, 0, 1, 1, "x", 4) == FLUX_ERROR_INVALID_ARGUMENT);

    flux_device *d = test_helpers_make_headless_device();
    if (!d) {
        fprintf(stderr, "test_image_update: no Vulkan device; skipping\n");
        TEST_SUMMARY();
    }

    /* Fill 16x16 with a flat colour as initial data. */
    uint32_t pixels[IMG_W * IMG_H];
    for (uint32_t i = 0; i < IMG_W * IMG_H; ++i)
        pixels[i] = 0xFF000080u;
    flux_image *img = make_image(d, pixels);
    EXPECT(img != nullptr);

    /* --- happy path: 8x8 region at (4,4) --- */
    {
        uint32_t patch[8 * 8];
        for (uint32_t i = 0; i < 8 * 8; ++i)
            patch[i] = 0xFF00FF00u;
        EXPECT(flux_image_update_region(img, 4, 4, 8, 8, patch, sizeof(patch)) == FLUX_OK);
    }

    /* --- happy path: 1x1 corner --- */
    {
        uint32_t p = 0xFFFFFFFFu;
        EXPECT(flux_image_update_region(img, IMG_W - 1, IMG_H - 1, 1, 1, &p, sizeof(p)) == FLUX_OK);
    }

    /* --- happy path: full image (x=0, y=0, w=W, h=H) --- */
    {
        EXPECT(flux_image_update_region(img, 0, 0, IMG_W, IMG_H, pixels, sizeof(pixels)) ==
               FLUX_OK);
    }

    /* --- out of bounds: x+w exceeds width --- */
    {
        uint32_t p = 0;
        EXPECT(flux_image_update_region(img, IMG_W - 1, 0, 2, 1, &p, 8) == FLUX_ERROR_OUT_OF_RANGE);
    }

    /* --- out of bounds: y+h exceeds height --- */
    {
        uint32_t p = 0;
        EXPECT(flux_image_update_region(img, 0, IMG_H, 1, 1, &p, 4) == FLUX_ERROR_OUT_OF_RANGE);
    }

    /* --- zero extent rejected --- */
    {
        uint32_t p = 0;
        EXPECT(flux_image_update_region(img, 0, 0, 0, 1, &p, 4) == FLUX_ERROR_INVALID_ARGUMENT);
        EXPECT(flux_image_update_region(img, 0, 0, 1, 0, &p, 4) == FLUX_ERROR_INVALID_ARGUMENT);
    }

    /* --- bytes too small for w*h*bpp --- */
    {
        uint8_t too_small[4];
        EXPECT(flux_image_update_region(img, 0, 0, 2, 2, too_small, sizeof(too_small)) ==
               FLUX_ERROR_INVALID_ARGUMENT);
    }

    /* --- NULL data rejected --- */
    EXPECT(flux_image_update_region(img, 0, 0, 1, 1, nullptr, 4) == FLUX_ERROR_INVALID_ARGUMENT);

    flux_image_release(img);
    flux_device_release(d);
    TEST_SUMMARY();
}
