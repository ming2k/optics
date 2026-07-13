/*
 * Offscreen surface (ADR-0013): render a canvas frame without a
 * window and assert the read-back pixels.
 *
 * This is the first GPU integration test that exercises the canvas
 * drawing path end-to-end (pipeline, transient ring, vertex pulling)
 * — everything before it stubbed the submit. Layout under test:
 *
 *   clear (20, 40, 60, 255), then an opaque white fill_rect covering
 *   the centre quarter and a blue swatch near the top-right. Corner
 *   pixels keep the clear colour; centre pixels are white; the swatch
 *   verifies that readback returns RGBA bytes even when the offscreen
 *   attachment uses a native BGRA format.
 *
 * Also covers: validation (windowed-only API misuse, short buffer,
 * read-before-submit), present-loop compatibility (begin → submit →
 * present runs unchanged), resize invalidation, multi-frame reuse.
 */
#include "test_helpers.h"
#include <flux/flux.h>
#include <flux/vulkan.h>

#include <stdlib.h>
#include <string.h>

#define W 64u
#define H 64u
#define BYTES (W * H * 4u)

static const uint8_t CLEAR_R = 20, CLEAR_G = 40, CLEAR_B = 60;

/* One pixel from a tightly packed RGBA8 readback. */
static const uint8_t *px_at(const uint8_t *px, uint32_t x, uint32_t y) {
    return px + (y * W + x) * 4u;
}

static bool near8(uint8_t got, uint8_t want) {
    int d = (int)got - (int)want;
    return d >= -2 && d <= 2; /* UNORM round-trip tolerance */
}

/* Render one frame: clear, then white centre-quarter rect. */
static flux_result render_frame(flux_surface *s, flux_canvas *canvas) {
    flux_frame *frame = nullptr;
    flux_result r = flux_surface_begin_frame(s, nullptr, &frame);
    if (r != FLUX_OK)
        return r;

    flux_color clear = flux_color_rgba(CLEAR_R, CLEAR_G, CLEAR_B, 255);
    r = flux_canvas_begin(canvas, frame, &clear);
    if (r != FLUX_OK)
        return r;

    flux_canvas_fill_rect_color(canvas, (flux_rect){W / 4.0f, H / 4.0f, W / 2.0f, H / 2.0f},
                                flux_color_rgba(255, 255, 255, 255));
    flux_canvas_fill_rect_color(canvas, (flux_rect){W - 12.0f, 4.0f, 8.0f, 8.0f},
                                flux_color_rgba(0, 64, 255, 255));
    flux_canvas_end(canvas);

    r = flux_frame_submit(frame);
    if (r != FLUX_OK)
        return r;
    return flux_frame_present(frame);
}

int main(void) {
    flux_device *d = test_helpers_make_headless_device();
    if (!d) {
        fprintf(stderr, "test_offscreen: no Vulkan device; skipping\n");
        TEST_SUMMARY();
    }

    /* --- creation validation --- */
    {
        flux_surface_desc sd = FLUX_SURFACE_DESC_INIT; /* no vk_surface_khr */
        flux_surface *s = nullptr;
        /* zero extent must be rejected, not deferred like a minimised window */
        EXPECT(flux_surface_create(d, &sd, &s) == FLUX_ERROR_INVALID_ARGUMENT);
        EXPECT(s == nullptr);
    }

    flux_surface *s = nullptr;
    {
        flux_surface_desc sd = FLUX_SURFACE_DESC_INIT;
        sd.width = W;
        sd.height = H;
        EXPECT(flux_surface_create(d, &sd, &s) == FLUX_OK);
        EXPECT(s != nullptr);

        flux_surface_info info;
        flux_surface_get_info(s, &info);
        EXPECT(info.width == W && info.height == H);
        EXPECT(!info.hdr);
        EXPECT(flux_surface_vk_swapchain(s) == VK_NULL_HANDLE);
    }

    static uint8_t px[BYTES];

    /* --- readback validation --- */
    {
        EXPECT(flux_surface_read_pixels(nullptr, px, BYTES) == FLUX_ERROR_INVALID_ARGUMENT);
        EXPECT(flux_surface_read_pixels(s, nullptr, BYTES) == FLUX_ERROR_INVALID_ARGUMENT);
        /* nothing submitted yet */
        EXPECT(flux_surface_read_pixels(s, px, BYTES) == FLUX_ERROR_INVALID_STATE);
    }

    flux_canvas *canvas = nullptr;
    {
        flux_canvas_desc cd = FLUX_CANVAS_DESC_INIT;
        cd.surface = s;
        EXPECT(flux_canvas_create(&cd, &canvas) == FLUX_OK);
    }

    /* --- frame state machine: recording -> submitted -> presented --- */
    {
        flux_frame *frame = nullptr;
        EXPECT(flux_surface_begin_frame(s, nullptr, &frame) == FLUX_OK);
        flux_transient tr = {0};
        EXPECT(flux_frame_alloc_transient(frame, 16, 3, &tr) == FLUX_ERROR_INVALID_ARGUMENT);
        EXPECT(tr.cpu == nullptr);
        EXPECT(flux_frame_alloc_transient(frame, 16, 512, &tr) == FLUX_ERROR_INVALID_ARGUMENT);
        EXPECT(flux_frame_alloc_transient(frame, 16, 16, &tr) == FLUX_OK);
        EXPECT(tr.cpu != nullptr && tr.alignment == 16);
        EXPECT(flux_frame_present(frame) == FLUX_ERROR_INVALID_STATE);
        EXPECT(flux_frame_submit(frame) == FLUX_OK);
        EXPECT(flux_frame_submit(frame) == FLUX_ERROR_INVALID_STATE);
        EXPECT(flux_frame_present(frame) == FLUX_OK);
        EXPECT(flux_frame_present(frame) == FLUX_ERROR_INVALID_STATE);
    }

    /* --- first frame: clear + centre rect --- */
    {
        EXPECT(render_frame(s, canvas) == FLUX_OK);

        /* short buffer rejected after a submit too */
        EXPECT(flux_surface_read_pixels(s, px, BYTES - 1) == FLUX_ERROR_INVALID_ARGUMENT);

        memset(px, 0xCD, BYTES);
        EXPECT(flux_surface_read_pixels(s, px, BYTES) == FLUX_OK);

        const uint8_t *corner = px_at(px, 1, 1);
        EXPECT(near8(corner[0], CLEAR_R));
        EXPECT(near8(corner[1], CLEAR_G));
        EXPECT(near8(corner[2], CLEAR_B));
        EXPECT(corner[3] == 255);

        const uint8_t *centre = px_at(px, W / 2, H / 2);
        EXPECT(centre[0] == 255 && centre[1] == 255 && centre[2] == 255);

        const uint8_t *blue = px_at(px, W - 8, 8);
        EXPECT(blue[0] < 5);
        EXPECT(near8(blue[1], 64));
        EXPECT(blue[2] > 250);
        EXPECT(blue[3] == 255);

        /* just inside each rect edge is white; just outside is clear */
        EXPECT(px_at(px, W / 4 + 1, H / 2)[0] == 255);
        EXPECT(near8(px_at(px, W / 4 - 2, H / 2)[0], CLEAR_R));
        EXPECT(px_at(px, W / 2, H / 4 + 1)[0] == 255);
        EXPECT(near8(px_at(px, W / 2, H / 4 - 2)[0], CLEAR_R));
    }

    /* --- Canvas LOAD overlays without discarding the selected image --- */
    {
        flux_surface_info info;
        flux_surface_get_info(s, &info);
        /* Offscreen surfaces have one image per frame slot. Seed every image
         * before wrapping around to the one selected by the LOAD frame. */
        for (uint32_t i = 0; i < info.image_count; ++i)
            EXPECT(render_frame(s, canvas) == FLUX_OK);

        flux_frame *frame = nullptr;
        EXPECT(flux_surface_begin_frame(s, nullptr, &frame) == FLUX_OK);
        EXPECT(flux_canvas_begin(canvas, frame, nullptr) == FLUX_OK);
        flux_canvas_fill_rect_color(canvas, (flux_rect){2, 2, 8, 8},
                                    flux_color_rgba_premul(0, 255, 0, 255));
        flux_canvas_end(canvas);
        EXPECT(flux_frame_submit(frame) == FLUX_OK);
        EXPECT(flux_frame_present(frame) == FLUX_OK);
        memset(px, 0xCD, BYTES);
        EXPECT(flux_surface_read_pixels(s, px, BYTES) == FLUX_OK);
        const uint8_t *overlay = px_at(px, 5, 5);
        EXPECT(overlay[0] < 5 && overlay[1] > 250 && overlay[2] < 5);
        EXPECT(near8(px_at(px, 12, 12)[0], CLEAR_R));
        EXPECT(px_at(px, W / 2, H / 2)[0] == 255);
    }

    /* --- frame ring reuse: render several frames back-to-back --- */
    {
        for (int i = 0; i < 5; ++i)
            EXPECT(render_frame(s, canvas) == FLUX_OK);
        memset(px, 0xCD, BYTES);
        EXPECT(flux_surface_read_pixels(s, px, BYTES) == FLUX_OK);
        EXPECT(px_at(px, W / 2, H / 2)[0] == 255);
        EXPECT(near8(px_at(px, 1, 1)[0], CLEAR_R));
    }

    /* --- resize drops old contents, then renders at the new extent --- */
    {
        EXPECT(flux_surface_resize(s, W, H) == FLUX_OK);
        EXPECT(flux_surface_read_pixels(s, px, BYTES) == FLUX_ERROR_INVALID_STATE);
        EXPECT(render_frame(s, canvas) == FLUX_OK);
        EXPECT(flux_surface_read_pixels(s, px, BYTES) == FLUX_OK);
        EXPECT(px_at(px, W / 2, H / 2)[0] == 255);
    }

    flux_canvas_destroy(canvas);
    flux_surface_release(s);
    flux_device_release(d);
    TEST_SUMMARY();
}
