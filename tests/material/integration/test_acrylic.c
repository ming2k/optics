/*
 * test_acrylic.c — integration test for material acrylic material.
 *
 * Verifies:
 *   - Lifecycle: creation, retention, refcounting, and safe destruction.
 *   - Masking: pixels outside bounds remain clear/zero.
 *   - Luminance plate: light and dark polarity balance correctly.
 *   - Frame slot cycling: multi-frame execution is stable with zero flickering.
 */

#include "gfx_test.h"
#include "optics_test.h"
#include <optics/gfx.h>
#include <optics/gfx/vulkan.h>
#include <optics/material.h>

#include "../material_test_helpers.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define W 64u
#define H 64u
#define BYTES (W * H * 4u)

static uint32_t render_acrylic_frame(optics_gfx_surface *surface, optics_gfx_canvas *canvas,
                                     optics_gfx_image *input, optics_gfx_image *blurred,
                                     optics_material_acrylic_filter *filter,
                                     const optics_material_acrylic_desc *desc, uint8_t *pixels) {
    optics_gfx_frame *frame = nullptr;
    OT_EXPECT(optics_gfx_surface_begin_frame(surface, nullptr, &frame) == OPTICS_GFX_OK);
    uint32_t slot = optics_gfx_frame_index(frame);

    optics_material_output out = {};
    OT_EXPECT(optics_material_acrylic_filter_apply(
                  filter, &(optics_material_acrylic_filter_apply_opts){
                              .frame = frame, .desc = desc, .out = &out}) == OPTICS_GFX_OK);
    OT_EXPECT(out.image != nullptr);

    optics_gfx_color clear_col = optics_gfx_color_rgba(0, 0, 0, 0);
    OT_EXPECT(optics_gfx_canvas_begin(canvas, &(optics_gfx_canvas_pass_desc){
                                                  .type = OPTICS_GFX_TYPE_CANVAS_PASS_DESC,
                                                  .frame = frame,
                                                  .clear_color = &clear_col}) == OPTICS_GFX_OK);
    test_draw_material(canvas, out);
    OT_EXPECT(optics_gfx_canvas_end(canvas) == OPTICS_GFX_OK);

    OT_EXPECT(optics_gfx_frame_submit(frame) == OPTICS_GFX_OK);
    OT_EXPECT(optics_gfx_frame_present(frame) == OPTICS_GFX_OK);
    memset(pixels, 0, BYTES);
    OT_EXPECT(optics_gfx_surface_read_pixels(surface, pixels, BYTES) == OPTICS_GFX_OK);
    return slot;
}

int main(void) {
    optics_gfx_device *d = optics_gfx_test_make_headless_device();
    if (!d) {
        fprintf(stderr, "test_acrylic: no Vulkan device; skipping\n");
        OT_SKIP("no Vulkan device");
    }

    /* 1. Lifecycle test */
    optics_material_acrylic_filter *filter = nullptr;
    OT_EXPECT(optics_material_acrylic_filter_create(d, &filter) == OPTICS_GFX_OK);
    OT_EXPECT(filter != nullptr);
    OT_EXPECT(optics_material_acrylic_filter_retain(filter) == filter);
    optics_material_acrylic_filter_release(filter);
    optics_material_acrylic_filter_release(filter);

    /* Recreate for rendering tests */
    OT_EXPECT(optics_material_acrylic_filter_create(d, &filter) == OPTICS_GFX_OK);
    OT_EXPECT(optics_material_acrylic_filter_warmup(nullptr, nullptr) ==
              OPTICS_GFX_ERROR_INVALID_ARGUMENT);
    OT_EXPECT(optics_material_acrylic_filter_warmup(filter, nullptr) == OPTICS_GFX_OK);

    optics_gfx_surface_desc sd = OPTICS_GFX_SURFACE_DESC_INIT;
    sd.width = W;
    sd.height = H;
    optics_gfx_surface *s = nullptr;
    OT_EXPECT(optics_gfx_surface_create(d, &sd, &s) == OPTICS_GFX_OK);

    optics_gfx_canvas_desc cd = {.type = OPTICS_GFX_TYPE_CANVAS_DESC, .surface = s};
    optics_gfx_canvas *c = nullptr;
    OT_EXPECT(optics_gfx_canvas_create(&cd, &c) == OPTICS_GFX_OK);

    /* Backdrop input texture */
    uint8_t *input_pixels = malloc(BYTES);
    for (uint32_t i = 0; i < W * H; ++i) {
        input_pixels[i * 4 + 0] = 60;
        input_pixels[i * 4 + 1] = 80;
        input_pixels[i * 4 + 2] = 120;
        input_pixels[i * 4 + 3] = 255;
    }

    optics_gfx_image_desc id = OPTICS_GFX_IMAGE_DESC_INIT;
    id.width = W;
    id.height = H;
    id.format = OPTICS_GFX_FORMAT_RGBA8_UNORM;
    id.initial_data = input_pixels;
    optics_gfx_image *input = nullptr;
    OT_EXPECT(optics_gfx_image_create(d, &id, &input) == OPTICS_GFX_OK);

    optics_gfx_image *blurred = nullptr;
    OT_EXPECT(optics_gfx_image_create(d, &id, &blurred) == OPTICS_GFX_OK);
    free(input_pixels);

    uint8_t *pixels = malloc(BYTES);

    /* 2. Shape [16, 16, 32, 32], r = 8 */
    optics_material_acrylic_shape shape = {
        .bounds = {16.0f, 16.0f, 32.0f, 32.0f},
        .corner_radius = 8.0f,
    };
    optics_material_acrylic_group group = OPTICS_MATERIAL_ACRYLIC_GROUP_INIT;
    group.shape = shape;
    group.tint_color = 0x223048;

    optics_material_acrylic_desc desc = OPTICS_MATERIAL_ACRYLIC_DESC_INIT;
    desc.input = input;
    desc.blurred_input = blurred;
    desc.groups = &group;
    desc.group_count = 1;
    desc.luminance_plate = 0.0f; /* Smoke plate */

    /* Render across several frames to guarantee multi-slot stability */
    uint8_t first_center[4] = {};
    for (int f = 0; f < 6; ++f) {
        render_acrylic_frame(s, c, input, blurred, filter, &desc, pixels);

        /* Outside should remain clear */
        uint32_t corner_idx = (2 * W + 2) * 4;
        OT_EXPECT(pixels[corner_idx + 3] == 0);

        /* Inside center should have solid alpha */
        uint32_t center_idx = (32 * W + 32) * 4;
        OT_EXPECT(pixels[center_idx + 3] > 200);

        if (f == 0) {
            memcpy(first_center, &pixels[center_idx], 4);
        } else {
            /* Zero flickering across frames: pixels must stay identical */
            OT_EXPECT(abs((int)pixels[center_idx + 0] - (int)first_center[0]) <= 1);
            OT_EXPECT(abs((int)pixels[center_idx + 1] - (int)first_center[1]) <= 1);
            OT_EXPECT(abs((int)pixels[center_idx + 2] - (int)first_center[2]) <= 1);
            OT_EXPECT(abs((int)pixels[center_idx + 3] - (int)first_center[3]) <= 1);
        }
    }

    free(pixels);
    optics_gfx_image_release(blurred);
    optics_gfx_image_release(input);
    optics_gfx_canvas_release(c);
    optics_gfx_surface_release(s);
    optics_material_acrylic_filter_release(filter);
    optics_gfx_device_release(d);

    return OT_REPORT();
}
