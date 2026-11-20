/*
 * flux_image_update_region_strided / _premultiply: bounds + stride
 * validation, strided repack correctness, and premultiply math parity with
 * flux_color_rgba_premul (bit-exact, via an offscreen blit + readback).
 * Skips if no Vulkan device.
 */
#include "gfx_test.h"
#include "optics_test.h"
#include <optics/gfx.h>
#include <optics/gfx/vulkan.h>

#include <string.h>

#define IMG_W 16u
#define IMG_H 16u
#define SURF_W 16u
#define SURF_H 16u

/* RGBA8_UNORM texel bytes are R,G,B,A; flux_color is 0xAARRGGBB. */
static uint32_t straight_rgba(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    return (uint32_t)r | ((uint32_t)g << 8) | ((uint32_t)b << 16) | ((uint32_t)a << 24);
}
static uint8_t px_r(uint32_t p) {
    return (uint8_t)(p & 0xffu);
}
static uint8_t px_g(uint32_t p) {
    return (uint8_t)((p >> 8) & 0xffu);
}
static uint8_t px_b(uint32_t p) {
    return (uint8_t)((p >> 16) & 0xffu);
}
static uint8_t px_a(uint32_t p) {
    return (uint8_t)((p >> 24) & 0xffu);
}

/* The canvas samples UNORM texels through an sRGB decode/encode round trip,
 * so readback carries ±2 8-bit noise (same tolerance as test_offscreen). */
static bool near8(uint8_t got, uint8_t want) {
    int d = (int)got - (int)want;
    return d >= -2 && d <= 2;
}
static bool px_near(uint32_t got, uint32_t want) {
    return near8(px_r(got), px_r(want)) && near8(px_g(got), px_g(want)) &&
           near8(px_b(got), px_b(want)) && near8(px_a(got), px_a(want));
}

typedef struct {
    optics_gfx_image *image;
    optics_gfx_sampler *sampler;
} blit_case;

/* Identity blit: whole image to whole surface with a NEAREST sampler, so
 * surface pixel (x, y) reads back image texel (x, y) exactly. Rendering
 * follows the offscreen frame loop (ADR-0013): begin frame, draw with the
 * canvas, request the readback on the frame, then submit + present. */
static optics_gfx_result blit_and_read(optics_gfx_surface *s, optics_gfx_canvas *canvas,
                                       blit_case *bc, uint32_t *rb, size_t bytes) {
    optics_gfx_frame *frame = nullptr;
    optics_gfx_result r = optics_gfx_surface_begin_frame(s, nullptr, &frame);
    if (r != OPTICS_GFX_OK)
        return r;
    optics_gfx_color clear = optics_gfx_color_rgba(0, 0, 0, 0);
    r = optics_gfx_canvas_begin(
        canvas, &(optics_gfx_canvas_pass_desc){.type = OPTICS_GFX_TYPE_CANVAS_PASS_DESC,
                                               .frame = frame,
                                               .clear_color = &clear});
    if (r != OPTICS_GFX_OK)
        return r;
    optics_gfx_image_style opaque = OPTICS_GFX_IMAGE_STYLE_INIT;
    opaque.blend = OPTICS_GFX_BLEND_SRC;
    optics_gfx_canvas_draw_image_sampled(
        canvas, bc->image, bc->sampler, (optics_gfx_rect){0.0f, 0.0f, (float)SURF_W, (float)SURF_H},
        &opaque);
    OT_EXPECT(optics_gfx_canvas_end(canvas) == OPTICS_GFX_OK);
    r = optics_gfx_frame_request_readback(frame);
    if (r != OPTICS_GFX_OK)
        return r;
    r = optics_gfx_frame_submit(frame);
    if (r != OPTICS_GFX_OK)
        return r;
    r = optics_gfx_frame_present(frame);
    if (r != OPTICS_GFX_OK)
        return r;
    return optics_gfx_surface_read_pixels(s, rb, bytes);
}

int main(void) {
    /* --- NULL safety --- */
    OT_EXPECT(optics_gfx_image_update_region(nullptr,
                                             &(optics_gfx_image_update_region_opts){.x = 0,
                                                                                    .y = 0,
                                                                                    .width = 1,
                                                                                    .height = 1,
                                                                                    .data = "x",
                                                                                    .row_bytes = 4,
                                                                                    .bytes = 4}) ==
              OPTICS_GFX_ERROR_INVALID_ARGUMENT);
    OT_EXPECT(optics_gfx_image_update_region(
                  nullptr, &(optics_gfx_image_update_region_opts){.x = 0,
                                                                  .y = 0,
                                                                  .width = 1,
                                                                  .height = 1,
                                                                  .data = "x",
                                                                  .row_bytes = 4,
                                                                  .bytes = 4,
                                                                  .premultiply = true}) ==
              OPTICS_GFX_ERROR_INVALID_ARGUMENT);

    optics_gfx_device *d = optics_gfx_test_make_headless_device();
    if (!d) {
        fprintf(stderr, "test_image_update_strided: no Vulkan device; skipping\n");
        OT_SKIP("no Vulkan device");
    }

    uint32_t pixels[IMG_W * IMG_H];
    for (uint32_t i = 0; i < IMG_W * IMG_H; ++i)
        pixels[i] = straight_rgba(255, 0, 0, 255);
    optics_gfx_image_desc idesc = OPTICS_GFX_IMAGE_DESC_INIT;
    idesc.width = IMG_W;
    idesc.height = IMG_H;
    idesc.format = OPTICS_GFX_FORMAT_RGBA8_UNORM;
    idesc.initial_data = pixels;
    optics_gfx_image *img = nullptr;
    OT_EXPECT(optics_gfx_image_create(d, &idesc, &img) == OPTICS_GFX_OK && img != nullptr);

    optics_gfx_sampler_desc sdesc = OPTICS_GFX_SAMPLER_DESC_INIT;
    sdesc.min_filter = OPTICS_GFX_FILTER_NEAREST;
    sdesc.mag_filter = OPTICS_GFX_FILTER_NEAREST;
    sdesc.address_u = OPTICS_GFX_ADDRESS_CLAMP_TO_EDGE;
    sdesc.address_v = OPTICS_GFX_ADDRESS_CLAMP_TO_EDGE;
    optics_gfx_sampler *nearest = nullptr;
    OT_EXPECT(optics_gfx_sampler_create(d, &sdesc, &nearest) == OPTICS_GFX_OK &&
              nearest != nullptr);

    optics_gfx_surface *surf = nullptr;
    optics_gfx_surface_desc sfd = OPTICS_GFX_SURFACE_DESC_INIT;
    sfd.width = SURF_W;
    sfd.height = SURF_H;
    OT_EXPECT(optics_gfx_surface_create(d, &sfd, &surf) == OPTICS_GFX_OK && surf != nullptr);

    optics_gfx_canvas *canvas = nullptr;
    {
        optics_gfx_canvas_desc cd = OPTICS_GFX_CANVAS_DESC_INIT;
        cd.surface = surf;
        OT_EXPECT(optics_gfx_canvas_create(&cd, &canvas) == OPTICS_GFX_OK && canvas != nullptr);
    }

    blit_case bc = {.image = img, .sampler = nearest};
    uint32_t rb[SURF_W * SURF_H];

/* Render one blit frame and read the pixels back. */
#define BLIT_AND_READ() OT_EXPECT(blit_and_read(surf, canvas, &bc, rb, sizeof(rb)) == OPTICS_GFX_OK)

    /* --- strided validation --- */
    {
        uint32_t patch[16];
        /* row_bytes smaller than width * bpp */
        OT_EXPECT(optics_gfx_image_update_region(
                      img, &(optics_gfx_image_update_region_opts){.x = 0,
                                                                  .y = 0,
                                                                  .width = 8,
                                                                  .height = 2,
                                                                  .data = patch,
                                                                  .row_bytes = 8,
                                                                  .bytes = sizeof(patch)}) ==
                  OPTICS_GFX_ERROR_INVALID_ARGUMENT);
        /* bytes too small for (h-1)*stride + w*bpp */
        OT_EXPECT(optics_gfx_image_update_region(
                      img, &(optics_gfx_image_update_region_opts){.x = 0,
                                                                  .y = 0,
                                                                  .width = 8,
                                                                  .height = 2,
                                                                  .data = patch,
                                                                  .row_bytes = 64,
                                                                  .bytes = 63}) ==
                  OPTICS_GFX_ERROR_INVALID_ARGUMENT);
        /* the exact minimum is accepted */
        uint8_t exact[96];
        memset(exact, 0xAA, sizeof(exact));
        OT_EXPECT(optics_gfx_image_update_region(
                      img, &(optics_gfx_image_update_region_opts){.x = 0,
                                                                  .y = 0,
                                                                  .width = 8,
                                                                  .height = 2,
                                                                  .data = exact,
                                                                  .row_bytes = 64,
                                                                  .bytes = sizeof(exact)}) ==
                  OPTICS_GFX_OK);
        /* zero extent still rejected */
        OT_EXPECT(optics_gfx_image_update_region(
                      img, &(optics_gfx_image_update_region_opts){.x = 0,
                                                                  .y = 0,
                                                                  .width = 0,
                                                                  .height = 1,
                                                                  .data = patch,
                                                                  .row_bytes = 4,
                                                                  .bytes = 4}) ==
                  OPTICS_GFX_ERROR_INVALID_ARGUMENT);
    }

    /* --- strided happy path: 4x3 region at (2,2) from a 64-byte-pitch
     * (16 px wide) source — rows must come from the right offsets. --- */
    {
        uint32_t src[16 * 3];
        for (uint32_t row = 0; row < 3; ++row)
            for (uint32_t col = 0; col < 16; ++col)
                src[row * 16 + col] = straight_rgba((uint8_t)row, (uint8_t)col, 7, 255);
        OT_EXPECT(optics_gfx_image_update_region(img, &(optics_gfx_image_update_region_opts){
                                                          .x = 2,
                                                          .y = 2,
                                                          .width = 4,
                                                          .height = 3,
                                                          .data = src,
                                                          .row_bytes = 64,
                                                          .bytes = sizeof(src)}) == OPTICS_GFX_OK);
        BLIT_AND_READ();
        for (uint32_t row = 0; row < 3; ++row)
            for (uint32_t col = 0; col < 4; ++col) {
                /* The uploaded rows come from source columns 0..3 (the
                 * region starts at source (0,0)), landed at image (2,2). */
                uint32_t got = rb[(2 + row) * SURF_W + (2 + col)];
                OT_EXPECT(px_near(got, straight_rgba((uint8_t)row, (uint8_t)col, 7, 255)));
            }
        /* outside the updated region: still the initial red (row 2's
         * untouched columns), proving the repack did not smear rows. */
        OT_EXPECT(px_near(rb[2 * SURF_W + 0], straight_rgba(255, 0, 0, 255)));
    }

    /* --- premultiply: format gate --- */
    {
        optics_gfx_image_desc d8 = OPTICS_GFX_IMAGE_DESC_INIT;
        d8.width = 2;
        d8.height = 2;
        d8.format = OPTICS_GFX_FORMAT_R8_UNORM;
        uint8_t r8[4] = {1, 2, 3, 4};
        d8.initial_data = r8;
        optics_gfx_image *grey = nullptr;
        OT_EXPECT(optics_gfx_image_create(d, &d8, &grey) == OPTICS_GFX_OK);
        OT_EXPECT(optics_gfx_image_update_region(
                      grey, &(optics_gfx_image_update_region_opts){.x = 0,
                                                                   .y = 0,
                                                                   .width = 1,
                                                                   .height = 1,
                                                                   .data = "abcd",
                                                                   .row_bytes = 4,
                                                                   .bytes = 4,
                                                                   .premultiply = true}) ==
                  OPTICS_GFX_ERROR_UNSUPPORTED);
        optics_gfx_image_release(grey);
    }

    /* --- premultiply: bit-exact parity with flux_color_rgba_premul across
     * alpha buckets and channels, from a strided source region. --- */
    {
        static const uint8_t chans[6] = {0, 1, 127, 128, 254, 255};
        /* 8 source rows of 16 px (stride 64 B); upload 8x6 at (4,4). */
        uint32_t src[16 * 8];
        uint32_t n = 0;
        for (uint32_t row = 0; row < 8; ++row)
            for (uint32_t col = 0; col < 16; ++col) {
                uint8_t r = chans[n % 6], g = chans[(n / 6) % 6], b = chans[(n / 12) % 6];
                uint8_t a = (uint8_t)((n * 7u) % 256u);
                src[row * 16 + col] = straight_rgba(r, g, b, a);
                ++n;
            }
        OT_EXPECT(optics_gfx_image_update_region(img, &(optics_gfx_image_update_region_opts){
                                                          .x = 4,
                                                          .y = 4,
                                                          .width = 8,
                                                          .height = 6,
                                                          .data = src,
                                                          .row_bytes = 64,
                                                          .bytes = sizeof(src),
                                                          .premultiply = true}) == OPTICS_GFX_OK);
        BLIT_AND_READ();
        /* The uploaded region reads source rows 0..5, cols 0..7 and lands
         * at image (4,4). Expectation walks the *source* coordinates. */
        for (uint32_t srow = 0; srow < 6; ++srow)
            for (uint32_t scol = 0; scol < 8; ++scol) {
                uint32_t n = srow * 16 + scol;
                uint8_t r = chans[n % 6], g = chans[(n / 6) % 6], b = chans[(n / 12) % 6];
                uint8_t a = (uint8_t)((n * 7u) % 256u);
                uint32_t want_premul = optics_gfx_color_rgba_premul(r, g, b, a);
                uint8_t wr = (uint8_t)((want_premul >> 16) & 0xffu);
                uint8_t wg = (uint8_t)((want_premul >> 8) & 0xffu);
                uint8_t wb = (uint8_t)(want_premul & 0xffu);
                uint32_t got = rb[(4 + srow) * SURF_W + (4 + scol)];
                OT_EXPECT(near8(px_r(got), wr) && near8(px_g(got), wg) && near8(px_b(got), wb) &&
                          near8(px_a(got), a));
            }
    }

    /* --- premultiply: packed call (row_bytes == 0) also accepted --- */
    {
        uint32_t src[4 * 2];
        for (uint32_t i = 0; i < 8; ++i)
            src[i] = straight_rgba(200, 100, 50, 128);
        OT_EXPECT(optics_gfx_image_update_region(img, &(optics_gfx_image_update_region_opts){
                                                          .x = 0,
                                                          .y = 0,
                                                          .width = 4,
                                                          .height = 2,
                                                          .data = src,
                                                          .row_bytes = 0,
                                                          .bytes = sizeof(src),
                                                          .premultiply = true}) == OPTICS_GFX_OK);
        BLIT_AND_READ();
        /* straight RGBA(200,100,50,128) -> premul per flux_color_rgba_premul */
        uint32_t want = optics_gfx_color_rgba_premul(200, 100, 50, 128);
        uint8_t wr = (uint8_t)((want >> 16) & 0xff), wg = (uint8_t)((want >> 8) & 0xff),
                wb = (uint8_t)(want & 0xff);
        for (uint32_t i = 0; i < 8; ++i) {
            uint32_t got = rb[(i / 4) * SURF_W + (i % 4)];
            OT_EXPECT(near8(px_r(got), wr) && near8(px_g(got), wg) && near8(px_b(got), wb) &&
                      near8(px_a(got), 128));
        }
    }

    optics_gfx_canvas_release(canvas);
    optics_gfx_surface_release(surf);
    optics_gfx_sampler_release(nearest);
    optics_gfx_image_release(img);
    optics_gfx_device_release(d);
    return OT_REPORT();
}
