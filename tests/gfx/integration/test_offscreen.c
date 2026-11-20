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
#include "gfx_test.h"
#include "optics_test.h"
#include <optics/gfx.h>
#include <optics/gfx/vulkan.h>

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
static optics_gfx_result render_frame(optics_gfx_surface *s, optics_gfx_canvas *canvas) {
    optics_gfx_frame *frame = nullptr;
    optics_gfx_result r = optics_gfx_surface_begin_frame(s, nullptr, &frame);
    if (r != OPTICS_GFX_OK)
        return r;

    optics_gfx_color clear = optics_gfx_color_rgba(CLEAR_R, CLEAR_G, CLEAR_B, 255);
    r = optics_gfx_canvas_begin(
        canvas, &(optics_gfx_canvas_pass_desc){.type = OPTICS_GFX_TYPE_CANVAS_PASS_DESC,
                                               .frame = frame,
                                               .clear_color = &clear});
    if (r != OPTICS_GFX_OK)
        return r;

    optics_gfx_canvas_fill_rect_color(canvas,
                                      (optics_gfx_rect){W / 4.0f, H / 4.0f, W / 2.0f, H / 2.0f},
                                      optics_gfx_color_rgba(255, 255, 255, 255));
    optics_gfx_canvas_fill_rect_color(canvas, (optics_gfx_rect){W - 12.0f, 4.0f, 8.0f, 8.0f},
                                      optics_gfx_color_rgba(0, 64, 255, 255));
    OT_EXPECT(optics_gfx_canvas_end(canvas) == OPTICS_GFX_OK);

    r = optics_gfx_frame_submit(frame);
    if (r != OPTICS_GFX_OK)
        return r;
    return optics_gfx_frame_present(frame);
}

static optics_gfx_result render_solid_frame(optics_gfx_surface *s, optics_gfx_canvas *canvas,
                                            optics_gfx_color color, bool capture) {
    optics_gfx_frame *frame = nullptr;
    optics_gfx_result r = optics_gfx_surface_begin_frame(s, nullptr, &frame);
    if (r != OPTICS_GFX_OK)
        return r;
    r = optics_gfx_canvas_begin(
        canvas, &(optics_gfx_canvas_pass_desc){.type = OPTICS_GFX_TYPE_CANVAS_PASS_DESC,
                                               .frame = frame,
                                               .clear_color = &color});
    if (r != OPTICS_GFX_OK)
        return r;
    OT_EXPECT(optics_gfx_canvas_end(canvas) == OPTICS_GFX_OK);
    if (capture) {
        r = optics_gfx_frame_request_readback(frame);
        if (r != OPTICS_GFX_OK)
            return r;
    }
    r = optics_gfx_frame_submit(frame);
    if (r != OPTICS_GFX_OK)
        return r;
    return optics_gfx_frame_present(frame);
}

static optics_gfx_result render_pattern_region(optics_gfx_surface *s, optics_gfx_canvas *canvas,
                                               const optics_gfx_readback_region *region) {
    optics_gfx_frame *frame = nullptr;
    optics_gfx_result r = optics_gfx_surface_begin_frame(s, nullptr, &frame);
    if (r != OPTICS_GFX_OK)
        return r;
    optics_gfx_color clear = optics_gfx_color_rgba(CLEAR_R, CLEAR_G, CLEAR_B, 255);
    r = optics_gfx_canvas_begin(
        canvas, &(optics_gfx_canvas_pass_desc){.type = OPTICS_GFX_TYPE_CANVAS_PASS_DESC,
                                               .frame = frame,
                                               .clear_color = &clear});
    if (r != OPTICS_GFX_OK)
        return r;
    optics_gfx_canvas_fill_rect_color(canvas,
                                      (optics_gfx_rect){W / 4.0f, H / 4.0f, W / 2.0f, H / 2.0f},
                                      optics_gfx_color_rgba(255, 255, 255, 255));
    optics_gfx_canvas_fill_rect_color(canvas, (optics_gfx_rect){W - 12.0f, 4.0f, 8.0f, 8.0f},
                                      optics_gfx_color_rgba(0, 64, 255, 255));
    OT_EXPECT(optics_gfx_canvas_end(canvas) == OPTICS_GFX_OK);
    const optics_gfx_readback_region empty = {.x = 1, .y = 1, .width = 0, .height = 1};
    const optics_gfx_readback_region outside = {.x = W, .y = 0, .width = 1, .height = 1};
    OT_EXPECT(optics_gfx_frame_request_readback_region(frame, nullptr) ==
              OPTICS_GFX_ERROR_INVALID_ARGUMENT);
    OT_EXPECT(optics_gfx_frame_request_readback_region(frame, &empty) ==
              OPTICS_GFX_ERROR_INVALID_ARGUMENT);
    OT_EXPECT(optics_gfx_frame_request_readback_region(frame, &outside) ==
              OPTICS_GFX_ERROR_OUT_OF_RANGE);
    r = optics_gfx_frame_request_readback_region(frame, region);
    if (r != OPTICS_GFX_OK)
        return r;
    r = optics_gfx_frame_submit(frame);
    if (r != OPTICS_GFX_OK)
        return r;
    return optics_gfx_frame_present(frame);
}

int main(void) {
    optics_gfx_device *d = optics_gfx_test_make_headless_device();
    if (!d) {
        fprintf(stderr, "test_offscreen: no Vulkan device; skipping\n");
        OT_SKIP("no Vulkan device");
    }

    /* --- creation validation --- */
    {
        optics_gfx_surface_desc sd = OPTICS_GFX_SURFACE_DESC_INIT; /* no vk_surface_khr */
        optics_gfx_surface *s = nullptr;
        /* zero extent must be rejected, not deferred like a minimised window */
        OT_EXPECT(optics_gfx_surface_create(d, &sd, &s) == OPTICS_GFX_ERROR_INVALID_ARGUMENT);
        OT_EXPECT(s == nullptr);
    }

    optics_gfx_surface *s = nullptr;
    {
        optics_gfx_surface_desc sd = OPTICS_GFX_SURFACE_DESC_INIT;
        sd.width = W;
        sd.height = H;
        OT_EXPECT(optics_gfx_surface_create(d, &sd, &s) == OPTICS_GFX_OK);
        OT_EXPECT(s != nullptr);

        optics_gfx_surface_info info;
        optics_gfx_surface_get_info(s, &info);
        OT_EXPECT(info.width == W && info.height == H);
        OT_EXPECT(!info.hdr);
        OT_EXPECT(optics_gfx_surface_vk_swapchain(s) == VK_NULL_HANDLE);
    }

    static uint8_t px[BYTES];

    /* --- readback validation --- */
    {
        bool ready = false;
        OT_EXPECT(optics_gfx_surface_read_pixels(nullptr, px, BYTES) ==
                  OPTICS_GFX_ERROR_INVALID_ARGUMENT);
        OT_EXPECT(optics_gfx_surface_read_pixels(s, nullptr, BYTES) ==
                  OPTICS_GFX_ERROR_INVALID_ARGUMENT);
        OT_EXPECT(optics_gfx_surface_read_pixels_ready(s, &ready) == OPTICS_GFX_ERROR_UNSUPPORTED);
        /* nothing submitted yet */
        OT_EXPECT(optics_gfx_surface_read_pixels(s, px, BYTES) == OPTICS_GFX_ERROR_INVALID_STATE);
    }

    optics_gfx_canvas *canvas = nullptr;
    {
        optics_gfx_canvas_desc cd = OPTICS_GFX_CANVAS_DESC_INIT;
        cd.surface = s;
        OT_EXPECT(optics_gfx_canvas_create(&cd, &canvas) == OPTICS_GFX_OK);
    }

    /* --- frame state machine: recording -> submitted -> presented --- */
    {
        optics_gfx_frame *frame = nullptr;
        OT_EXPECT(optics_gfx_surface_begin_frame(s, nullptr, &frame) == OPTICS_GFX_OK);
        optics_gfx_transient tr = {};
        OT_EXPECT(optics_gfx_frame_alloc_transient(frame,
                                                   &(optics_gfx_frame_alloc_transient_opts){
                                                       .bytes = 16, .alignment = 3, .out = &tr}) ==
                  OPTICS_GFX_ERROR_INVALID_ARGUMENT);
        OT_EXPECT(tr.cpu == nullptr);
        OT_EXPECT(
            optics_gfx_frame_alloc_transient(frame,
                                             &(optics_gfx_frame_alloc_transient_opts){
                                                 .bytes = 16, .alignment = 512, .out = &tr}) ==
            OPTICS_GFX_ERROR_INVALID_ARGUMENT);
        OT_EXPECT(optics_gfx_frame_alloc_transient(
                      frame, &(optics_gfx_frame_alloc_transient_opts){
                                 .bytes = 16, .alignment = 16, .out = &tr}) == OPTICS_GFX_OK);
        OT_EXPECT(tr.cpu != nullptr && tr.alignment == 16);
        OT_EXPECT(optics_gfx_frame_present(frame) == OPTICS_GFX_ERROR_INVALID_STATE);
        OT_EXPECT(optics_gfx_frame_submit(frame) == OPTICS_GFX_OK);
        OT_EXPECT(optics_gfx_frame_submit(frame) == OPTICS_GFX_ERROR_INVALID_STATE);
        OT_EXPECT(optics_gfx_frame_present(frame) == OPTICS_GFX_OK);
        OT_EXPECT(optics_gfx_frame_present(frame) == OPTICS_GFX_ERROR_INVALID_STATE);
    }

    /* --- frame state machine: abandon releases the slot without ring
     * advance (ADR-0110) --- */
    {
        /* A null frame and a recording frame are both no-ops: abandon only
         * consumes a *submitted* frame's borrow. */
        optics_gfx_frame_abandon(nullptr);

        optics_gfx_frame *frame = nullptr;
        OT_EXPECT(optics_gfx_surface_begin_frame(s, nullptr, &frame) == OPTICS_GFX_OK);
        optics_gfx_frame_abandon(frame);
        OT_EXPECT(optics_gfx_canvas_begin(canvas, &(optics_gfx_canvas_pass_desc){
                                                      .type = OPTICS_GFX_TYPE_CANVAS_PASS_DESC,
                                                      .frame = frame,
                                                      .clear_color = nullptr}) == OPTICS_GFX_OK);
        OT_EXPECT(optics_gfx_canvas_end(canvas) == OPTICS_GFX_OK);
        OT_EXPECT(optics_gfx_frame_submit(frame) == OPTICS_GFX_OK);

        /* Abandon consumes the submitted frame: both later transitions are
         * single-use and must reject. */
        optics_gfx_frame_abandon(frame);
        OT_EXPECT(optics_gfx_frame_present(frame) == OPTICS_GFX_ERROR_INVALID_STATE);
        OT_EXPECT(optics_gfx_frame_submit(frame) == OPTICS_GFX_ERROR_INVALID_STATE);
        optics_gfx_frame_abandon(frame);

        /* The abandoned slot is immediately reusable and still renders the
         * same content: the surface never wedges. */
        OT_EXPECT(render_frame(s, canvas) == OPTICS_GFX_OK);
        OT_EXPECT(optics_gfx_surface_read_pixels(s, px, BYTES) == OPTICS_GFX_OK);
        OT_EXPECT(near8(px_at(px, 1, 1)[0], CLEAR_R));
        OT_EXPECT(px_at(px, W / 2, H / 2)[0] == 255);
    }

    /* --- cross-surface frame ownership: a canvas refuses a frame from a
     * different surface (the command buffer belongs to that surface's
     * per-slot pool) --- */
    {
        optics_gfx_surface *s2 = nullptr;
        optics_gfx_surface_desc sd = OPTICS_GFX_SURFACE_DESC_INIT;
        sd.width = W;
        sd.height = H;
        OT_EXPECT(optics_gfx_surface_create(d, &sd, &s2) == OPTICS_GFX_OK);

        optics_gfx_frame *foreign = nullptr;
        OT_EXPECT(optics_gfx_surface_begin_frame(s2, nullptr, &foreign) == OPTICS_GFX_OK);
        OT_EXPECT(optics_gfx_canvas_begin(
                      canvas, &(optics_gfx_canvas_pass_desc){
                                  .type = OPTICS_GFX_TYPE_CANVAS_PASS_DESC,
                                  .frame = foreign,
                                  .clear_color = nullptr}) == OPTICS_GFX_ERROR_INVALID_ARGUMENT);
        /* The canvas must be untouched by the refused begin: a same-surface
         * frame works immediately after. */
        optics_gfx_frame *own = nullptr;
        OT_EXPECT(optics_gfx_surface_begin_frame(s, nullptr, &own) == OPTICS_GFX_OK);
        OT_EXPECT(optics_gfx_canvas_begin(canvas, &(optics_gfx_canvas_pass_desc){
                                                      .type = OPTICS_GFX_TYPE_CANVAS_PASS_DESC,
                                                      .frame = own,
                                                      .clear_color = nullptr}) == OPTICS_GFX_OK);
        OT_EXPECT(optics_gfx_canvas_end(canvas) == OPTICS_GFX_OK);
        OT_EXPECT(optics_gfx_frame_submit(own) == OPTICS_GFX_OK);
        OT_EXPECT(optics_gfx_frame_present(own) == OPTICS_GFX_OK);
        /* Finish the foreign frame on its own surface. */
        OT_EXPECT(optics_gfx_frame_submit(foreign) == OPTICS_GFX_OK);
        OT_EXPECT(optics_gfx_frame_present(foreign) == OPTICS_GFX_OK);
        optics_gfx_surface_release(s2);
    }

    /* --- first frame: clear + centre rect --- */
    {
        OT_EXPECT(render_frame(s, canvas) == OPTICS_GFX_OK);

        /* short buffer rejected after a submit too */
        OT_EXPECT(optics_gfx_surface_read_pixels(s, px, BYTES - 1) ==
                  OPTICS_GFX_ERROR_INVALID_ARGUMENT);

        memset(px, 0xCD, BYTES);
        OT_EXPECT(optics_gfx_surface_read_pixels(s, px, BYTES) == OPTICS_GFX_OK);

        const uint8_t *corner = px_at(px, 1, 1);
        OT_EXPECT(near8(corner[0], CLEAR_R));
        OT_EXPECT(near8(corner[1], CLEAR_G));
        OT_EXPECT(near8(corner[2], CLEAR_B));
        OT_EXPECT(corner[3] == 255);

        const uint8_t *centre = px_at(px, W / 2, H / 2);
        OT_EXPECT(centre[0] == 255 && centre[1] == 255 && centre[2] == 255);

        const uint8_t *blue = px_at(px, W - 8, 8);
        OT_EXPECT(blue[0] < 5);
        OT_EXPECT(near8(blue[1], 64));
        OT_EXPECT(blue[2] > 250);
        OT_EXPECT(blue[3] == 255);

        /* just inside each rect edge is white; just outside is clear */
        OT_EXPECT(px_at(px, W / 4 + 1, H / 2)[0] == 255);
        OT_EXPECT(near8(px_at(px, W / 4 - 2, H / 2)[0], CLEAR_R));
        OT_EXPECT(px_at(px, W / 2, H / 4 + 1)[0] == 255);
        OT_EXPECT(near8(px_at(px, W / 2, H / 4 - 2)[0], CLEAR_R));
    }

    /* --- Canvas LOAD overlays without discarding the selected image --- */
    {
        optics_gfx_surface_info info;
        optics_gfx_surface_get_info(s, &info);
        /* Offscreen surfaces have one image per frame slot. Seed every image
         * before wrapping around to the one selected by the LOAD frame. */
        for (uint32_t i = 0; i < info.image_count; ++i)
            OT_EXPECT(render_frame(s, canvas) == OPTICS_GFX_OK);

        optics_gfx_frame *frame = nullptr;
        OT_EXPECT(optics_gfx_surface_begin_frame(s, nullptr, &frame) == OPTICS_GFX_OK);
        OT_EXPECT(optics_gfx_canvas_begin(canvas, &(optics_gfx_canvas_pass_desc){
                                                      .type = OPTICS_GFX_TYPE_CANVAS_PASS_DESC,
                                                      .frame = frame,
                                                      .clear_color = nullptr}) == OPTICS_GFX_OK);
        optics_gfx_canvas_fill_rect_color(canvas, (optics_gfx_rect){2, 2, 8, 8},
                                          optics_gfx_color_rgba_premul(0, 255, 0, 255));
        OT_EXPECT(optics_gfx_canvas_end(canvas) == OPTICS_GFX_OK);
        OT_EXPECT(optics_gfx_frame_submit(frame) == OPTICS_GFX_OK);
        OT_EXPECT(optics_gfx_frame_present(frame) == OPTICS_GFX_OK);
        memset(px, 0xCD, BYTES);
        OT_EXPECT(optics_gfx_surface_read_pixels(s, px, BYTES) == OPTICS_GFX_OK);
        const uint8_t *overlay = px_at(px, 5, 5);
        OT_EXPECT(overlay[0] < 5 && overlay[1] > 250 && overlay[2] < 5);
        OT_EXPECT(near8(px_at(px, 12, 12)[0], CLEAR_R));
        OT_EXPECT(px_at(px, W / 2, H / 2)[0] == 255);
    }

    /* --- frame ring reuse: render several frames back-to-back --- */
    {
        for (int i = 0; i < 5; ++i)
            OT_EXPECT(render_frame(s, canvas) == OPTICS_GFX_OK);
        memset(px, 0xCD, BYTES);
        OT_EXPECT(optics_gfx_surface_read_pixels(s, px, BYTES) == OPTICS_GFX_OK);
        OT_EXPECT(px_at(px, W / 2, H / 2)[0] == 255);
        OT_EXPECT(near8(px_at(px, 1, 1)[0], CLEAR_R));
    }

    /* --- resize drops old contents, then renders at the new extent --- */
    {
        OT_EXPECT(optics_gfx_surface_resize(s, W, H) == OPTICS_GFX_OK);
        OT_EXPECT(optics_gfx_surface_read_pixels(s, px, BYTES) == OPTICS_GFX_ERROR_INVALID_STATE);
        OT_EXPECT(render_frame(s, canvas) == OPTICS_GFX_OK);
        OT_EXPECT(optics_gfx_surface_read_pixels(s, px, BYTES) == OPTICS_GFX_OK);
        OT_EXPECT(px_at(px, W / 2, H / 2)[0] == 255);
    }

    optics_gfx_canvas_release(canvas);
    optics_gfx_surface_release(s);

    /* --- on-demand snapshot is the requested frame, not later frames --- */
    {
        optics_gfx_surface_desc sd = OPTICS_GFX_SURFACE_DESC_INIT;
        sd.width = W;
        sd.height = H;
        optics_gfx_surface *snapshot_surface = nullptr;
        OT_EXPECT(optics_gfx_surface_create(d, &sd, &snapshot_surface) == OPTICS_GFX_OK);
        optics_gfx_canvas *snapshot_canvas = nullptr;
        optics_gfx_canvas_desc cd = OPTICS_GFX_CANVAS_DESC_INIT;
        cd.surface = snapshot_surface;
        OT_EXPECT(optics_gfx_canvas_create(&cd, &snapshot_canvas) == OPTICS_GFX_OK);

        bool ready = false;
        OT_EXPECT(optics_gfx_surface_prepare_readback(nullptr) ==
                  OPTICS_GFX_ERROR_INVALID_ARGUMENT);
        OT_EXPECT(optics_gfx_surface_prepare_readback(snapshot_surface) == OPTICS_GFX_OK);
        OT_EXPECT(optics_gfx_surface_prepare_readback(snapshot_surface) == OPTICS_GFX_OK);
        OT_EXPECT(optics_gfx_frame_request_readback(nullptr) == OPTICS_GFX_ERROR_INVALID_STATE);
        OT_EXPECT(optics_gfx_surface_read_pixels_ready(snapshot_surface, &ready) ==
                  OPTICS_GFX_ERROR_INVALID_STATE);
        OT_EXPECT(render_solid_frame(snapshot_surface, snapshot_canvas,
                                     optics_gfx_color_rgba(220, 10, 20, 255),
                                     true) == OPTICS_GFX_OK);
        /* This newer green frame must not mutate the requested red snapshot. */
        OT_EXPECT(render_solid_frame(snapshot_surface, snapshot_canvas,
                                     optics_gfx_color_rgba(10, 210, 20, 255),
                                     false) == OPTICS_GFX_OK);
        optics_gfx_device_wait_idle(d);
        OT_EXPECT(optics_gfx_surface_read_pixels_ready(snapshot_surface, &ready) == OPTICS_GFX_OK);
        OT_EXPECT(ready);
        optics_gfx_readback *snapshot = nullptr;
        OT_EXPECT(optics_gfx_surface_take_readback(snapshot_surface, &snapshot) == OPTICS_GFX_OK);
        OT_EXPECT(snapshot != nullptr);
        optics_gfx_readback_region full_region = {};
        optics_gfx_readback_get_region(snapshot, &full_region);
        OT_EXPECT(full_region.x == 0 && full_region.y == 0);
        OT_EXPECT(full_region.width == W && full_region.height == H);
        OT_EXPECT(optics_gfx_surface_read_pixels_ready(snapshot_surface, &ready) ==
                  OPTICS_GFX_ERROR_UNSUPPORTED);
        memset(px, 0xCD, BYTES);
        OT_EXPECT(optics_gfx_readback_read_pixels(snapshot, px, BYTES) == OPTICS_GFX_OK);
        OT_EXPECT(near8(px_at(px, 1, 1)[0], 220));
        OT_EXPECT(near8(px_at(px, 1, 1)[1], 10));
        OT_EXPECT(near8(px_at(px, 1, 1)[2], 20));
        OT_EXPECT(px_at(px, 1, 1)[3] == 255);
        optics_gfx_readback_release(snapshot);

        optics_gfx_canvas_release(snapshot_canvas);
        optics_gfx_surface_release(snapshot_surface);
    }

    /* --- region snapshot copies only the requested image offset/extent --- */
    {
        optics_gfx_surface_desc sd = OPTICS_GFX_SURFACE_DESC_INIT;
        sd.width = W;
        sd.height = H;
        optics_gfx_surface *region_surface = nullptr;
        OT_EXPECT(optics_gfx_surface_create(d, &sd, &region_surface) == OPTICS_GFX_OK);
        optics_gfx_canvas *region_canvas = nullptr;
        optics_gfx_canvas_desc cd = OPTICS_GFX_CANVAS_DESC_INIT;
        cd.surface = region_surface;
        OT_EXPECT(optics_gfx_canvas_create(&cd, &region_canvas) == OPTICS_GFX_OK);

        OT_EXPECT(optics_gfx_surface_prepare_readback_region(nullptr, 12, 12) ==
                  OPTICS_GFX_ERROR_INVALID_ARGUMENT);
        OT_EXPECT(optics_gfx_surface_prepare_readback_region(region_surface, 0, 12) ==
                  OPTICS_GFX_ERROR_INVALID_ARGUMENT);
        OT_EXPECT(optics_gfx_surface_prepare_readback_region(region_surface, W + 1, 12) ==
                  OPTICS_GFX_ERROR_OUT_OF_RANGE);
        OT_EXPECT(optics_gfx_surface_prepare_readback_region(region_surface, 12, 12) ==
                  OPTICS_GFX_OK);

        const optics_gfx_readback_region wanted = {.x = W - 14, .y = 2, .width = 12, .height = 12};
        OT_EXPECT(render_pattern_region(region_surface, region_canvas, &wanted) == OPTICS_GFX_OK);
        optics_gfx_device_wait_idle(d);
        bool ready = false;
        OT_EXPECT(optics_gfx_surface_read_pixels_ready(region_surface, &ready) == OPTICS_GFX_OK);
        OT_EXPECT(ready);

        optics_gfx_readback *snapshot = nullptr;
        OT_EXPECT(optics_gfx_surface_take_readback(region_surface, &snapshot) == OPTICS_GFX_OK);
        optics_gfx_readback_region got = {1, 1, 1, 1};
        optics_gfx_readback_get_region(nullptr, &got);
        OT_EXPECT(got.x == 0 && got.y == 0 && got.width == 0 && got.height == 0);
        optics_gfx_readback_get_region(snapshot, &got);
        OT_EXPECT(got.x == wanted.x && got.y == wanted.y);
        OT_EXPECT(got.width == wanted.width && got.height == wanted.height);

        uint8_t crop[12u * 12u * 4u];
        OT_EXPECT(optics_gfx_readback_read_pixels(snapshot, crop, sizeof(crop) - 1) ==
                  OPTICS_GFX_ERROR_INVALID_ARGUMENT);
        memset(crop, 0xCD, sizeof(crop));
        OT_EXPECT(optics_gfx_readback_read_pixels(snapshot, crop, sizeof(crop)) == OPTICS_GFX_OK);
        const uint8_t *clear = px_at_width(crop, wanted.width, 0, 0); /* surface 50,2 */
        OT_EXPECT(near8(clear[0], CLEAR_R) && near8(clear[1], CLEAR_G) && near8(clear[2], CLEAR_B));
        const uint8_t *blue = px_at_width(crop, wanted.width, 2, 2); /* surface 52,4 */
        OT_EXPECT(blue[0] < 5 && near8(blue[1], 64) && blue[2] > 250 && blue[3] == 255);
        const uint8_t *outside = px_at_width(crop, wanted.width, 10, 10); /* surface 60,12 */
        OT_EXPECT(near8(outside[0], CLEAR_R) && near8(outside[1], CLEAR_G) &&
                  near8(outside[2], CLEAR_B));
        optics_gfx_readback_release(snapshot);

        optics_gfx_canvas_release(region_canvas);
        optics_gfx_surface_release(region_surface);
    }

    /* --- readback desc: pinned non-exportable, readback always works --- */
    {
        optics_gfx_surface_readback_desc rb = OPTICS_GFX_SURFACE_READBACK_DESC_INIT;
        rb.require_readback = true;
        optics_gfx_surface_desc sd = OPTICS_GFX_SURFACE_DESC_INIT;
        sd.next = &rb;
        sd.width = W;
        sd.height = H;
        optics_gfx_surface *rs = nullptr;
        OT_EXPECT(optics_gfx_surface_create(d, &sd, &rs) == OPTICS_GFX_OK);
        OT_EXPECT(rs != nullptr);
        OT_EXPECT(!optics_gfx_surface_exportable(rs));
        OT_EXPECT(optics_gfx_surface_prepare_readback_region(rs, 12, 12) ==
                  OPTICS_GFX_ERROR_UNSUPPORTED);
        bool ready = false;
        OT_EXPECT(optics_gfx_surface_read_pixels_ready(rs, &ready) ==
                  OPTICS_GFX_ERROR_INVALID_STATE);

        optics_gfx_canvas *rc = nullptr;
        optics_gfx_canvas_desc cd = OPTICS_GFX_CANVAS_DESC_INIT;
        cd.surface = rs;
        OT_EXPECT(optics_gfx_canvas_create(&cd, &rc) == OPTICS_GFX_OK);
        OT_EXPECT(render_frame(rs, rc) == OPTICS_GFX_OK);
        OT_EXPECT(optics_gfx_surface_read_pixels_ready(rs, &ready) == OPTICS_GFX_OK);
        if (!ready) {
            optics_gfx_device_wait_idle(d);
            OT_EXPECT(optics_gfx_surface_read_pixels_ready(rs, &ready) == OPTICS_GFX_OK);
        }
        OT_EXPECT(ready);
        memset(px, 0xCD, BYTES);
        OT_EXPECT(optics_gfx_surface_read_pixels(rs, px, BYTES) == OPTICS_GFX_OK);
        OT_EXPECT(px_at(px, W / 2, H / 2)[0] == 255);
        OT_EXPECT(near8(px_at(px, 1, 1)[0], CLEAR_R));
        /* Several queued frames safely share the persistent staging buffer;
         * the latest frame fence covers the last FIFO copy. */
        for (int i = 0; i < 5; ++i)
            OT_EXPECT(render_frame(rs, rc) == OPTICS_GFX_OK);
        optics_gfx_device_wait_idle(d);
        ready = false;
        OT_EXPECT(optics_gfx_surface_read_pixels_ready(rs, &ready) == OPTICS_GFX_OK);
        OT_EXPECT(ready);
        OT_EXPECT(optics_gfx_surface_read_pixels(rs, px, BYTES) == OPTICS_GFX_OK);
        OT_EXPECT(px_at(px, W / 2, H / 2)[0] == 255);
        /* Resize retires and recreates persistent staging with the images. */
        OT_EXPECT(optics_gfx_surface_resize(rs, W, H) == OPTICS_GFX_OK);
        OT_EXPECT(optics_gfx_surface_read_pixels_ready(rs, &ready) ==
                  OPTICS_GFX_ERROR_INVALID_STATE);
        OT_EXPECT(render_frame(rs, rc) == OPTICS_GFX_OK);
        OT_EXPECT(optics_gfx_surface_read_pixels(rs, px, BYTES) == OPTICS_GFX_OK);
        OT_EXPECT(near8(px_at(px, 1, 1)[0], CLEAR_R));
        /* export still refused: the surface is deliberately not exportable */
        int fd = -1;
        optics_gfx_surface_dmabuf_lease *lease = nullptr;
        OT_EXPECT(optics_gfx_surface_export_dmabuf(rs, &fd, &lease) != OPTICS_GFX_OK);
        OT_EXPECT(lease == nullptr);

        optics_gfx_canvas_release(rc);
        optics_gfx_surface_release(rs);
    }

    /* --- readback + dma-buf extensions conflict --- */
    {
        uint64_t linear_modifier = 0; /* DRM_FORMAT_MOD_LINEAR */
        optics_gfx_surface_dmabuf_desc dm = OPTICS_GFX_SURFACE_DMABUF_DESC_INIT;
        dm.modifiers = &linear_modifier;
        dm.modifier_count = 1;
        optics_gfx_surface_readback_desc rb = OPTICS_GFX_SURFACE_READBACK_DESC_INIT;
        rb.require_readback = true;
        rb.next = &dm;
        optics_gfx_surface_desc sd = OPTICS_GFX_SURFACE_DESC_INIT;
        sd.next = &rb;
        sd.width = W;
        sd.height = H;
        optics_gfx_surface *cs = nullptr;
        OT_EXPECT(optics_gfx_surface_create(d, &sd, &cs) == OPTICS_GFX_ERROR_INVALID_ARGUMENT);
        OT_EXPECT(cs == nullptr);
    }

    optics_gfx_device_release(d);
    return OT_REPORT();
}
