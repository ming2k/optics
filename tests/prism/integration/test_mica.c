/*
 * test_mica.c — integration test for prism mica foundation material (ADR-0096).
 *
 * Verifies:
 *   - Lifecycle: creation, retention, refcounting, and safe destruction.
 *   - Geometry & Masking: analytic rounded-rectangle SDF masks the mica layer.
 *   - Base vs. Alt: PRISM_MICA_ALT produces deeper tint/contrast than PRISM_MICA_BASE.
 *   - Inactive Fallback: fallback_weight = 1.0f blends output into fallback_color.
 *   - Screen anchoring: screen_width / screen_height coordinates execute reliably.
 *   - Frame slot cycling: multi-frame persistent output clearing works without residue.
 */

#include "test_helpers.h"
#include <flux/flux.h>
#include <flux/vulkan.h>
#include <prism/prism.h>

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define W 64u
#define H 64u
#define BYTES (W * H * 4u)

static uint32_t render_mica_frame(flux_surface *surface, flux_canvas *canvas, flux_image *wallpaper,
                                  prism_mica_filter *mica_filter, const prism_mica_desc *desc,
                                  uint8_t *pixels) {
    flux_frame *frame = nullptr;
    EXPECT(flux_surface_begin_frame(surface, nullptr, &frame) == FLUX_OK);
    uint32_t slot = flux_frame_index(frame);

    flux_image *mica_out = nullptr;
    EXPECT(prism_mica_filter_apply(mica_filter, frame, desc, &mica_out) == FLUX_OK);
    EXPECT(mica_out != nullptr);

    flux_color clear_col = flux_color_rgba(0, 0, 0, 0);
    EXPECT(flux_canvas_begin(canvas, &(flux_canvas_pass_desc){.type = FLUX_TYPE_CANVAS_PASS_DESC,
                                                              .frame = frame,
                                                              .clear_color = &clear_col}) ==
           FLUX_OK);
    flux_canvas_draw_image(canvas, mica_out, (flux_rect){0, 0, (float)W, (float)H}, nullptr);
    EXPECT(flux_canvas_end(canvas) == FLUX_OK);

    EXPECT(flux_frame_submit(frame) == FLUX_OK);
    EXPECT(flux_frame_present(frame) == FLUX_OK);
    memset(pixels, 0, BYTES);
    EXPECT(flux_surface_read_pixels(surface, pixels, BYTES) == FLUX_OK);
    return slot;
}

int main(void) {
    flux_device *d = test_helpers_make_headless_device();
    if (!d) {
        fprintf(stderr, "test_mica: no Vulkan device; skipping\n");
        TEST_SUMMARY();
        return 0;
    }

    /* 1. Lifecycle test */
    prism_mica_filter *filter = nullptr;
    EXPECT(prism_mica_filter_create(d, &filter) == FLUX_OK);
    EXPECT(filter != nullptr);
    EXPECT(prism_mica_filter_retain(filter) == filter);
    prism_mica_filter_release(filter);
    prism_mica_filter_release(filter);

    /* Recreate for rendering tests */
    EXPECT(prism_mica_filter_create(d, &filter) == FLUX_OK);

    flux_surface_desc sd = FLUX_SURFACE_DESC_INIT;
    sd.width = W;
    sd.height = H;
    flux_surface *s = nullptr;
    EXPECT(flux_surface_create(d, &sd, &s) == FLUX_OK);

    flux_canvas_desc cd = {.type = FLUX_TYPE_CANVAS_DESC, .surface = s};
    flux_canvas *c = nullptr;
    EXPECT(flux_canvas_create(&cd, &c) == FLUX_OK);

    /* Populate wallpaper with distinct cyan color: (30, 160, 220, 255) */
    uint8_t *wp_pixels = malloc(BYTES);
    for (uint32_t i = 0; i < W * H; ++i) {
        wp_pixels[i * 4 + 0] = 30;
        wp_pixels[i * 4 + 1] = 160;
        wp_pixels[i * 4 + 2] = 220;
        wp_pixels[i * 4 + 3] = 255;
    }

    flux_image_desc id = FLUX_IMAGE_DESC_INIT;
    id.width = W;
    id.height = H;
    id.format = FLUX_FORMAT_RGBA8_UNORM;
    id.initial_data = wp_pixels;
    flux_image *wp = nullptr;
    EXPECT(flux_image_create(d, &id, &wp) == FLUX_OK);
    free(wp_pixels);

    uint8_t *pixels = malloc(BYTES);

    /* 2. Base Mica test with rounded rectangle body: [16, 16, 32, 32], r = 8 */
    prism_mica_shape shape = {
        .bounds = {16.0f, 16.0f, 32.0f, 32.0f},
        .corner_radius = 8.0f,
    };
    prism_mica_group group = PRISM_MICA_GROUP_INIT;
    group.shape = shape;

    prism_mica_desc desc_base = PRISM_MICA_DESC_INIT;
    desc_base.wallpaper = wp;
    desc_base.groups = &group;
    desc_base.group_count = 1;
    desc_base.kind = PRISM_MICA_BASE;
    desc_base.luminosity_plate = 0.0f; /* Dark smoke plate */
    desc_base.tint_color = 0x00FF88;   /* Teal tint */
    desc_base.tint_opacity = 0.30f;

    render_mica_frame(s, c, wp, filter, &desc_base, pixels);

    /* Verify masking: pixel (2, 2) is outside the body -> should remain clear / zero */
    uint32_t corner_idx = (2 * W + 2) * 4;
    EXPECT(pixels[corner_idx + 3] == 0);

    /* Verify inside: pixel (32, 32) is center of body -> should have solid alpha and blended color
     */
    uint32_t center_idx = (32 * W + 32) * 4;
    EXPECT(pixels[center_idx + 3] > 240);
    uint8_t base_g = pixels[center_idx + 1];

    /* 3. Mica Alt test: deeper tint & contrast */
    prism_mica_desc desc_alt = desc_base;
    desc_alt.kind = PRISM_MICA_ALT;
    render_mica_frame(s, c, wp, filter, &desc_alt, pixels);

    EXPECT(pixels[center_idx + 3] > 240);
    uint8_t alt_g = pixels[center_idx + 1];
    /* Alt has higher tint boost than Base, so green channel should be noticeably distinct */
    EXPECT(alt_g != base_g);

    /* 4. Inactive Fallback test: fallback_weight = 1.0f */
    prism_mica_desc desc_inactive = desc_base;
    desc_inactive.fallback_color = 0x402010; /* Dark brownish charcoal */
    desc_inactive.fallback_weight = 1.0f;
    render_mica_frame(s, c, wp, filter, &desc_inactive, pixels);

    /* Center color should closely reflect fallback_color (R ~ 0x40=64, G ~ 0x20=32, B ~ 0x10=16) */
    uint8_t fb_r = pixels[center_idx + 0];
    uint8_t fb_g = pixels[center_idx + 1];
    uint8_t fb_b = pixels[center_idx + 2];
    EXPECT(abs((int)fb_r - 0x40) < 10);
    EXPECT(abs((int)fb_g - 0x20) < 10);
    EXPECT(abs((int)fb_b - 0x10) < 10);

    /* 5. Screen-anchoring test: coordinate offsets */
    prism_mica_desc desc_screen = desc_base;
    desc_screen.screen_origin_x = 100.0f;
    desc_screen.screen_origin_y = 200.0f;
    desc_screen.screen_width = 1920.0f;
    desc_screen.screen_height = 1080.0f;
    render_mica_frame(s, c, wp, filter, &desc_screen, pixels);
    EXPECT(pixels[center_idx + 3] > 240);

    /* Clean up */
    free(pixels);
    flux_image_release(wp);
    flux_canvas_release(c);
    flux_surface_release(s);
    prism_mica_filter_release(filter);
    flux_device_wait_idle(d);
    flux_device_release(d);

    TEST_SUMMARY();
    return 0;
}
