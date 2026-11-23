/*
 * optics_gfx_image_update_region: bounds checks + happy-path upload of a
 * sub-region into an existing image. Skips if no Vulkan device.
 */
#include "gfx_test.h"
#include "optics_test.h"
#include <optics/gfx.h>
#include <optics/gfx/vulkan.h>

#include <string.h>

#define IMG_W 16u
#define IMG_H 16u

static optics_gfx_image *make_image(optics_gfx_device *d, uint32_t *initial) {
    optics_gfx_image_desc desc = OPTICS_GFX_IMAGE_DESC_INIT;
    desc.width = IMG_W;
    desc.height = IMG_H;
    desc.format = OPTICS_GFX_FORMAT_RGBA8_UNORM;
    desc.initial_data = initial;
    optics_gfx_image *out = nullptr;
    return optics_gfx_image_create(d, &desc, &out) == OPTICS_GFX_OK ? out : nullptr;
}

int main(void) {
    /* --- NULL safety --- */
    OT_EXPECT(optics_gfx_image_update_region(
                  nullptr,
                  &(optics_gfx_image_update_region_opts){
                      .x = 0, .y = 0, .width = 1, .height = 1, .data = "x", .bytes = 4}) ==
              OPTICS_GFX_ERROR_INVALID_ARGUMENT);

    optics_gfx_device *d = optics_gfx_test_make_headless_device();
    if (!d) {
        fprintf(stderr, "test_image_update: no Vulkan device; skipping\n");
        OT_SKIP("no Vulkan device");
    }

    /* Fill 16x16 with a flat colour as initial data. */
    uint32_t pixels[IMG_W * IMG_H];
    for (uint32_t i = 0; i < IMG_W * IMG_H; ++i)
        pixels[i] = 0xFF000080u;
    optics_gfx_image *img = make_image(d, pixels);
    OT_EXPECT(img != nullptr);

    /* --- happy path: 8x8 region at (4,4) --- */
    {
        uint32_t patch[8 * 8];
        for (uint32_t i = 0; i < 8 * 8; ++i)
            patch[i] = 0xFF00FF00u;
        OT_EXPECT(optics_gfx_image_update_region(
                      img, &(optics_gfx_image_update_region_opts){.x = 4,
                                                                  .y = 4,
                                                                  .width = 8,
                                                                  .height = 8,
                                                                  .data = patch,
                                                                  .bytes = sizeof(patch)}) ==
                  OPTICS_GFX_OK);
    }

    /* --- happy path: 1x1 corner --- */
    {
        uint32_t p = 0xFFFFFFFFu;
        OT_EXPECT(optics_gfx_image_update_region(img, &(optics_gfx_image_update_region_opts){
                                                          .x = IMG_W - 1,
                                                          .y = IMG_H - 1,
                                                          .width = 1,
                                                          .height = 1,
                                                          .data = &p,
                                                          .bytes = sizeof(p)}) == OPTICS_GFX_OK);
    }

    /* --- happy path: full image (x=0, y=0, w=W, h=H) --- */
    {
        OT_EXPECT(optics_gfx_image_update_region(
                      img, &(optics_gfx_image_update_region_opts){.x = 0,
                                                                  .y = 0,
                                                                  .width = IMG_W,
                                                                  .height = IMG_H,
                                                                  .data = pixels,
                                                                  .bytes = sizeof(pixels)}) ==
                  OPTICS_GFX_OK);
    }

    /* --- out of bounds: x+w exceeds width --- */
    {
        uint32_t p = 0;
        OT_EXPECT(
            optics_gfx_image_update_region(
                img,
                &(optics_gfx_image_update_region_opts){
                    .x = IMG_W - 1, .y = 0, .width = 2, .height = 1, .data = &p, .bytes = 8}) ==
            OPTICS_GFX_ERROR_OUT_OF_RANGE);
    }

    /* --- out of bounds: y+h exceeds height --- */
    {
        uint32_t p = 0;
        OT_EXPECT(optics_gfx_image_update_region(
                      img,
                      &(optics_gfx_image_update_region_opts){
                          .x = 0, .y = IMG_H, .width = 1, .height = 1, .data = &p, .bytes = 4}) ==
                  OPTICS_GFX_ERROR_OUT_OF_RANGE);
    }

    /* --- zero extent rejected --- */
    {
        uint32_t p = 0;
        OT_EXPECT(optics_gfx_image_update_region(
                      img,
                      &(optics_gfx_image_update_region_opts){
                          .x = 0, .y = 0, .width = 0, .height = 1, .data = &p, .bytes = 4}) ==
                  OPTICS_GFX_ERROR_INVALID_ARGUMENT);
        OT_EXPECT(optics_gfx_image_update_region(
                      img,
                      &(optics_gfx_image_update_region_opts){
                          .x = 0, .y = 0, .width = 1, .height = 0, .data = &p, .bytes = 4}) ==
                  OPTICS_GFX_ERROR_INVALID_ARGUMENT);
    }

    /* --- bytes too small for w*h*bpp --- */
    {
        uint8_t too_small[4];
        OT_EXPECT(optics_gfx_image_update_region(
                      img, &(optics_gfx_image_update_region_opts){.x = 0,
                                                                  .y = 0,
                                                                  .width = 2,
                                                                  .height = 2,
                                                                  .data = too_small,
                                                                  .bytes = sizeof(too_small)}) ==
                  OPTICS_GFX_ERROR_INVALID_ARGUMENT);
    }

    /* --- NULL data rejected --- */
    OT_EXPECT(optics_gfx_image_update_region(
                  img,
                  &(optics_gfx_image_update_region_opts){
                      .x = 0, .y = 0, .width = 1, .height = 1, .data = nullptr, .bytes = 4}) ==
              OPTICS_GFX_ERROR_INVALID_ARGUMENT);

    optics_gfx_image_release(img);
    optics_gfx_device_release(d);
    return OT_REPORT();
}
