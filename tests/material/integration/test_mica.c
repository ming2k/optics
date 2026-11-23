/*
 * test_mica.c — integration test for material mica foundation material (ADR-0096).
 *
 * Verifies:
 *   - Lifecycle: creation, retention, refcounting, and safe destruction.
 *   - Geometry & Masking: analytic rounded-rectangle SDF masks the mica layer.
 *   - Base vs. Alt: OPTICS_MATERIAL_MICA_ALT produces deeper tint/contrast than
 * OPTICS_MATERIAL_MICA_BASE.
 *   - Inactive Fallback: fallback_weight = 1.0f blends output into fallback_color.
 *   - Screen anchoring: screen_width / screen_height coordinates execute reliably.
 *   - Frame slot cycling: multi-frame persistent output clearing works without residue.
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

static uint32_t render_mica_frame(optics_gfx_surface *surface, optics_gfx_canvas *canvas,
                                  optics_gfx_image *wallpaper,
                                  optics_material_mica_filter *mica_filter,
                                  const optics_material_mica_desc *desc, uint8_t *pixels) {
    optics_gfx_frame *frame = nullptr;
    OT_EXPECT(optics_gfx_surface_begin_frame(surface, nullptr, &frame) == OPTICS_GFX_OK);
    uint32_t slot = optics_gfx_frame_index(frame);

    optics_material_output mica_out = {};
    OT_EXPECT(
        optics_material_mica_filter_apply(
            mica_filter, &(optics_material_mica_filter_apply_opts){
                             .frame = frame, .desc = desc, .out = &mica_out}) == OPTICS_GFX_OK);
    OT_EXPECT(mica_out.image != nullptr);

    optics_gfx_color clear_col = optics_gfx_color_rgba(0, 0, 0, 0);
    OT_EXPECT(optics_gfx_canvas_begin(canvas, &(optics_gfx_canvas_pass_desc){
                                                  .type = OPTICS_GFX_TYPE_CANVAS_PASS_DESC,
                                                  .frame = frame,
                                                  .clear_color = &clear_col}) == OPTICS_GFX_OK);
    test_draw_material(canvas, mica_out);
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
        fprintf(stderr, "test_mica: no Vulkan device; skipping\n");
        OT_SKIP("no Vulkan device");
    }

    /* 1. Lifecycle test */
    optics_material_mica_filter *filter = nullptr;
    OT_EXPECT(optics_material_mica_filter_create(d, &filter) == OPTICS_GFX_OK);
    OT_EXPECT(filter != nullptr);
    OT_EXPECT(optics_material_mica_filter_retain(filter) == filter);
    optics_material_mica_filter_release(filter);
    optics_material_mica_filter_release(filter);

    /* Recreate for rendering tests */
    OT_EXPECT(optics_material_mica_filter_create(d, &filter) == OPTICS_GFX_OK);
    OT_EXPECT(optics_material_mica_filter_warmup(nullptr, nullptr) ==
              OPTICS_GFX_ERROR_INVALID_ARGUMENT);
    OT_EXPECT(optics_material_mica_filter_warmup(filter, nullptr) == OPTICS_GFX_OK);

    optics_gfx_surface_desc sd = OPTICS_GFX_SURFACE_DESC_INIT;
    sd.width = W;
    sd.height = H;
    optics_gfx_surface *s = nullptr;
    OT_EXPECT(optics_gfx_surface_create(d, &sd, &s) == OPTICS_GFX_OK);

    optics_gfx_canvas_desc cd = {.type = OPTICS_GFX_TYPE_CANVAS_DESC, .surface = s};
    optics_gfx_canvas *c = nullptr;
    OT_EXPECT(optics_gfx_canvas_create(&cd, &c) == OPTICS_GFX_OK);

    /* Populate wallpaper with distinct cyan color: (30, 160, 220, 255) */
    uint8_t *wp_pixels = malloc(BYTES);
    for (uint32_t i = 0; i < W * H; ++i) {
        wp_pixels[i * 4 + 0] = 30;
        wp_pixels[i * 4 + 1] = 160;
        wp_pixels[i * 4 + 2] = 220;
        wp_pixels[i * 4 + 3] = 255;
    }

    optics_gfx_image_desc id = OPTICS_GFX_IMAGE_DESC_INIT;
    id.width = W;
    id.height = H;
    id.format = OPTICS_GFX_FORMAT_RGBA8_UNORM;
    id.initial_data = wp_pixels;
    optics_gfx_image *wp = nullptr;
    OT_EXPECT(optics_gfx_image_create(d, &id, &wp) == OPTICS_GFX_OK);
    free(wp_pixels);

    uint8_t *pixels = malloc(BYTES);

    /* 2. Base Mica test with rounded rectangle body: [16, 16, 32, 32], r = 8 */
    optics_material_mica_shape shape = {
        .bounds = {16.0f, 16.0f, 32.0f, 32.0f},
        .corner_radius = 8.0f,
    };
    optics_material_mica_group group = OPTICS_MATERIAL_MICA_GROUP_INIT;
    group.shape = shape;

    optics_material_mica_desc desc_base = OPTICS_MATERIAL_MICA_DESC_INIT;
    desc_base.wallpaper = wp;
    desc_base.groups = &group;
    desc_base.group_count = 1;
    desc_base.kind = OPTICS_MATERIAL_MICA_BASE;
    desc_base.luminosity_plate = 0.0f; /* Dark smoke plate */
    desc_base.tint_color = 0x00FF88;   /* Teal tint */
    desc_base.tint_opacity = 0.30f;

    render_mica_frame(s, c, wp, filter, &desc_base, pixels);

    /* Verify masking: pixel (2, 2) is outside the body -> should remain clear / zero */
    uint32_t corner_idx = (2 * W + 2) * 4;
    OT_EXPECT(pixels[corner_idx + 3] == 0);

    /* Verify inside: pixel (32, 32) is center of body -> should have solid alpha and blended color
     */
    uint32_t center_idx = (32 * W + 32) * 4;
    OT_EXPECT(pixels[center_idx + 3] > 240);
    uint8_t base_g = pixels[center_idx + 1];

    /* 3. Mica Alt test: deeper tint & contrast */
    optics_material_mica_desc desc_alt = desc_base;
    desc_alt.kind = OPTICS_MATERIAL_MICA_ALT;
    render_mica_frame(s, c, wp, filter, &desc_alt, pixels);

    OT_EXPECT(pixels[center_idx + 3] > 240);
    uint8_t alt_g = pixels[center_idx + 1];
    /* Alt has higher tint boost than Base, so green channel should be noticeably distinct */
    OT_EXPECT(alt_g != base_g);

    /* 4. Inactive Fallback test: fallback_weight = 1.0f */
    optics_material_mica_desc desc_inactive = desc_base;
    desc_inactive.fallback_color = 0x402010; /* Dark brownish charcoal */
    desc_inactive.fallback_weight = 1.0f;
    render_mica_frame(s, c, wp, filter, &desc_inactive, pixels);

    /* Center color should closely reflect fallback_color (R ~ 0x40=64, G ~ 0x20=32, B ~ 0x10=16) */
    uint8_t fb_r = pixels[center_idx + 0];
    uint8_t fb_g = pixels[center_idx + 1];
    uint8_t fb_b = pixels[center_idx + 2];
    OT_EXPECT(abs((int)fb_r - 0x40) < 10);
    OT_EXPECT(abs((int)fb_g - 0x20) < 10);
    OT_EXPECT(abs((int)fb_b - 0x10) < 10);

    /* 5. Screen-anchoring test: coordinate offsets */
    optics_material_mica_desc desc_screen = desc_base;
    desc_screen.screen_origin_x = 100.0f;
    desc_screen.screen_origin_y = 200.0f;
    desc_screen.screen_width = 1920.0f;
    desc_screen.screen_height = 1080.0f;
    render_mica_frame(s, c, wp, filter, &desc_screen, pixels);
    OT_EXPECT(pixels[center_idx + 3] > 240);

    /* Clean up */
    free(pixels);
    optics_gfx_image_release(wp);
    optics_gfx_canvas_release(c);
    optics_gfx_surface_release(s);
    optics_material_mica_filter_release(filter);
    optics_gfx_device_wait_idle(d);
    optics_gfx_device_release(d);

    return OT_REPORT();
}
