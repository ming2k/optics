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
 * read-before-submit, cross-surface frame ownership), present-loop
 * compatibility (begin → submit → present runs unchanged), resize
 * invalidation, multi-frame reuse.
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

static const uint8_t *px_at_width(const uint8_t *px, uint32_t width, uint32_t x, uint32_t y) {
    return px + (y * width + x) * 4u;
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

static flux_result render_solid_frame(flux_surface *s, flux_canvas *canvas, flux_color color,
                                      bool capture) {
    flux_frame *frame = nullptr;
    flux_result r = flux_surface_begin_frame(s, nullptr, &frame);
    if (r != FLUX_OK)
        return r;
    r = flux_canvas_begin(canvas, frame, &color);
    if (r != FLUX_OK)
        return r;
    flux_canvas_end(canvas);
    if (capture) {
        r = flux_frame_request_readback(frame);
        if (r != FLUX_OK)
            return r;
    }
    r = flux_frame_submit(frame);
    if (r != FLUX_OK)
        return r;
    return flux_frame_present(frame);
}

static flux_result render_pattern_region(flux_surface *s, flux_canvas *canvas,
                                         const flux_readback_region *region) {
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
    const flux_readback_region empty = {.x = 1, .y = 1, .width = 0, .height = 1};
    const flux_readback_region outside = {.x = W, .y = 0, .width = 1, .height = 1};
    EXPECT(flux_frame_request_readback_region(frame, nullptr) == FLUX_ERROR_INVALID_ARGUMENT);
    EXPECT(flux_frame_request_readback_region(frame, &empty) == FLUX_ERROR_INVALID_ARGUMENT);
    EXPECT(flux_frame_request_readback_region(frame, &outside) == FLUX_ERROR_OUT_OF_RANGE);
    r = flux_frame_request_readback_region(frame, region);
    if (r != FLUX_OK)
        return r;
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
        bool ready = false;
        EXPECT(flux_surface_read_pixels(nullptr, px, BYTES) == FLUX_ERROR_INVALID_ARGUMENT);
        EXPECT(flux_surface_read_pixels(s, nullptr, BYTES) == FLUX_ERROR_INVALID_ARGUMENT);
        EXPECT(flux_surface_read_pixels_ready(s, &ready) == FLUX_ERROR_UNSUPPORTED);
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

    /* --- cross-surface frame ownership: a canvas refuses a frame from a
     * different surface (the command buffer belongs to that surface's
     * per-slot pool) --- */
    {
        flux_surface *s2 = nullptr;
        flux_surface_desc sd = FLUX_SURFACE_DESC_INIT;
        sd.width = W;
        sd.height = H;
        EXPECT(flux_surface_create(d, &sd, &s2) == FLUX_OK);

        flux_frame *foreign = nullptr;
        EXPECT(flux_surface_begin_frame(s2, nullptr, &foreign) == FLUX_OK);
        EXPECT(flux_canvas_begin(canvas, foreign, nullptr) == FLUX_ERROR_INVALID_ARGUMENT);
        /* The canvas must be untouched by the refused begin: a same-surface
         * frame works immediately after. */
        flux_frame *own = nullptr;
        EXPECT(flux_surface_begin_frame(s, nullptr, &own) == FLUX_OK);
        EXPECT(flux_canvas_begin(canvas, own, nullptr) == FLUX_OK);
        flux_canvas_end(canvas);
        EXPECT(flux_frame_submit(own) == FLUX_OK);
        EXPECT(flux_frame_present(own) == FLUX_OK);
        /* Finish the foreign frame on its own surface. */
        EXPECT(flux_frame_submit(foreign) == FLUX_OK);
        EXPECT(flux_frame_present(foreign) == FLUX_OK);
        flux_surface_release(s2);
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

    /* --- on-demand snapshot is the requested frame, not later frames --- */
    {
        flux_surface_desc sd = FLUX_SURFACE_DESC_INIT;
        sd.width = W;
        sd.height = H;
        flux_surface *snapshot_surface = nullptr;
        EXPECT(flux_surface_create(d, &sd, &snapshot_surface) == FLUX_OK);
        flux_canvas *snapshot_canvas = nullptr;
        flux_canvas_desc cd = FLUX_CANVAS_DESC_INIT;
        cd.surface = snapshot_surface;
        EXPECT(flux_canvas_create(&cd, &snapshot_canvas) == FLUX_OK);

        bool ready = false;
        EXPECT(flux_surface_prepare_readback(nullptr) == FLUX_ERROR_INVALID_ARGUMENT);
        EXPECT(flux_surface_prepare_readback(snapshot_surface) == FLUX_OK);
        EXPECT(flux_surface_prepare_readback(snapshot_surface) == FLUX_OK);
        EXPECT(flux_frame_request_readback(nullptr) == FLUX_ERROR_INVALID_STATE);
        EXPECT(flux_surface_read_pixels_ready(snapshot_surface, &ready) ==
               FLUX_ERROR_INVALID_STATE);
        EXPECT(render_solid_frame(snapshot_surface, snapshot_canvas,
                                  flux_color_rgba(220, 10, 20, 255), true) == FLUX_OK);
        /* This newer green frame must not mutate the requested red snapshot. */
        EXPECT(render_solid_frame(snapshot_surface, snapshot_canvas,
                                  flux_color_rgba(10, 210, 20, 255), false) == FLUX_OK);
        flux_device_wait_idle(d);
        EXPECT(flux_surface_read_pixels_ready(snapshot_surface, &ready) == FLUX_OK);
        EXPECT(ready);
        flux_readback *snapshot = nullptr;
        EXPECT(flux_surface_take_readback(snapshot_surface, &snapshot) == FLUX_OK);
        EXPECT(snapshot != nullptr);
        flux_readback_region full_region = {0};
        flux_readback_get_region(snapshot, &full_region);
        EXPECT(full_region.x == 0 && full_region.y == 0);
        EXPECT(full_region.width == W && full_region.height == H);
        EXPECT(flux_surface_read_pixels_ready(snapshot_surface, &ready) ==
               FLUX_ERROR_UNSUPPORTED);
        memset(px, 0xCD, BYTES);
        EXPECT(flux_readback_read_pixels(snapshot, px, BYTES) == FLUX_OK);
        EXPECT(near8(px_at(px, 1, 1)[0], 220));
        EXPECT(near8(px_at(px, 1, 1)[1], 10));
        EXPECT(near8(px_at(px, 1, 1)[2], 20));
        EXPECT(px_at(px, 1, 1)[3] == 255);
        flux_readback_release(snapshot);

        flux_canvas_destroy(snapshot_canvas);
        flux_surface_release(snapshot_surface);
    }

    /* --- region snapshot copies only the requested image offset/extent --- */
    {
        flux_surface_desc sd = FLUX_SURFACE_DESC_INIT;
        sd.width = W;
        sd.height = H;
        flux_surface *region_surface = nullptr;
        EXPECT(flux_surface_create(d, &sd, &region_surface) == FLUX_OK);
        flux_canvas *region_canvas = nullptr;
        flux_canvas_desc cd = FLUX_CANVAS_DESC_INIT;
        cd.surface = region_surface;
        EXPECT(flux_canvas_create(&cd, &region_canvas) == FLUX_OK);

        EXPECT(flux_surface_prepare_readback_region(nullptr, 12, 12) ==
               FLUX_ERROR_INVALID_ARGUMENT);
        EXPECT(flux_surface_prepare_readback_region(region_surface, 0, 12) ==
               FLUX_ERROR_INVALID_ARGUMENT);
        EXPECT(flux_surface_prepare_readback_region(region_surface, W + 1, 12) ==
               FLUX_ERROR_OUT_OF_RANGE);
        EXPECT(flux_surface_prepare_readback_region(region_surface, 12, 12) == FLUX_OK);

        const flux_readback_region wanted = {.x = W - 14, .y = 2, .width = 12, .height = 12};
        EXPECT(render_pattern_region(region_surface, region_canvas, &wanted) == FLUX_OK);
        flux_device_wait_idle(d);
        bool ready = false;
        EXPECT(flux_surface_read_pixels_ready(region_surface, &ready) == FLUX_OK);
        EXPECT(ready);

        flux_readback *snapshot = nullptr;
        EXPECT(flux_surface_take_readback(region_surface, &snapshot) == FLUX_OK);
        flux_readback_region got = {1, 1, 1, 1};
        flux_readback_get_region(nullptr, &got);
        EXPECT(got.x == 0 && got.y == 0 && got.width == 0 && got.height == 0);
        flux_readback_get_region(snapshot, &got);
        EXPECT(got.x == wanted.x && got.y == wanted.y);
        EXPECT(got.width == wanted.width && got.height == wanted.height);

        uint8_t crop[12u * 12u * 4u];
        EXPECT(flux_readback_read_pixels(snapshot, crop, sizeof(crop) - 1) ==
               FLUX_ERROR_INVALID_ARGUMENT);
        memset(crop, 0xCD, sizeof(crop));
        EXPECT(flux_readback_read_pixels(snapshot, crop, sizeof(crop)) == FLUX_OK);
        const uint8_t *clear = px_at_width(crop, wanted.width, 0, 0); /* surface 50,2 */
        EXPECT(near8(clear[0], CLEAR_R) && near8(clear[1], CLEAR_G) &&
               near8(clear[2], CLEAR_B));
        const uint8_t *blue = px_at_width(crop, wanted.width, 2, 2); /* surface 52,4 */
        EXPECT(blue[0] < 5 && near8(blue[1], 64) && blue[2] > 250 && blue[3] == 255);
        const uint8_t *outside = px_at_width(crop, wanted.width, 10, 10); /* surface 60,12 */
        EXPECT(near8(outside[0], CLEAR_R) && near8(outside[1], CLEAR_G) &&
               near8(outside[2], CLEAR_B));
        flux_readback_release(snapshot);

        flux_canvas_destroy(region_canvas);
        flux_surface_release(region_surface);
    }

    /* --- readback desc: pinned non-exportable, readback always works --- */
    {
        flux_surface_readback_desc rb = FLUX_SURFACE_READBACK_DESC_INIT;
        rb.require_readback = true;
        flux_surface_desc sd = FLUX_SURFACE_DESC_INIT;
        sd.next = &rb;
        sd.width = W;
        sd.height = H;
        flux_surface *rs = nullptr;
        EXPECT(flux_surface_create(d, &sd, &rs) == FLUX_OK);
        EXPECT(rs != nullptr);
        EXPECT(!flux_surface_exportable(rs));
        EXPECT(flux_surface_prepare_readback_region(rs, 12, 12) == FLUX_ERROR_UNSUPPORTED);
        bool ready = false;
        EXPECT(flux_surface_read_pixels_ready(rs, &ready) == FLUX_ERROR_INVALID_STATE);

        flux_canvas *rc = nullptr;
        flux_canvas_desc cd = FLUX_CANVAS_DESC_INIT;
        cd.surface = rs;
        EXPECT(flux_canvas_create(&cd, &rc) == FLUX_OK);
        EXPECT(render_frame(rs, rc) == FLUX_OK);
        EXPECT(flux_surface_read_pixels_ready(rs, &ready) == FLUX_OK);
        if (!ready) {
            flux_device_wait_idle(d);
            EXPECT(flux_surface_read_pixels_ready(rs, &ready) == FLUX_OK);
        }
        EXPECT(ready);
        memset(px, 0xCD, BYTES);
        EXPECT(flux_surface_read_pixels(rs, px, BYTES) == FLUX_OK);
        EXPECT(px_at(px, W / 2, H / 2)[0] == 255);
        EXPECT(near8(px_at(px, 1, 1)[0], CLEAR_R));
        /* Several queued frames safely share the persistent staging buffer;
         * the latest frame fence covers the last FIFO copy. */
        for (int i = 0; i < 5; ++i)
            EXPECT(render_frame(rs, rc) == FLUX_OK);
        flux_device_wait_idle(d);
        ready = false;
        EXPECT(flux_surface_read_pixels_ready(rs, &ready) == FLUX_OK);
        EXPECT(ready);
        EXPECT(flux_surface_read_pixels(rs, px, BYTES) == FLUX_OK);
        EXPECT(px_at(px, W / 2, H / 2)[0] == 255);
        /* Resize retires and recreates persistent staging with the images. */
        EXPECT(flux_surface_resize(rs, W, H) == FLUX_OK);
        EXPECT(flux_surface_read_pixels_ready(rs, &ready) == FLUX_ERROR_INVALID_STATE);
        EXPECT(render_frame(rs, rc) == FLUX_OK);
        EXPECT(flux_surface_read_pixels(rs, px, BYTES) == FLUX_OK);
        EXPECT(near8(px_at(px, 1, 1)[0], CLEAR_R));
        /* export still refused: the surface is deliberately not exportable */
        int fd = -1;
        EXPECT(flux_surface_export_dmabuf(rs, &fd) != FLUX_OK);

        flux_canvas_destroy(rc);
        flux_surface_release(rs);
    }

    /* --- readback + dma-buf extensions conflict --- */
    {
        uint64_t linear_modifier = 0; /* DRM_FORMAT_MOD_LINEAR */
        flux_surface_dmabuf_desc dm = FLUX_SURFACE_DMABUF_DESC_INIT;
        dm.modifiers = &linear_modifier;
        dm.modifier_count = 1;
        flux_surface_readback_desc rb = FLUX_SURFACE_READBACK_DESC_INIT;
        rb.require_readback = true;
        rb.next = &dm;
        flux_surface_desc sd = FLUX_SURFACE_DESC_INIT;
        sd.next = &rb;
        sd.width = W;
        sd.height = H;
        flux_surface *cs = nullptr;
        EXPECT(flux_surface_create(d, &sd, &cs) == FLUX_ERROR_INVALID_ARGUMENT);
        EXPECT(cs == nullptr);
    }

    flux_device_release(d);
    TEST_SUMMARY();
}
