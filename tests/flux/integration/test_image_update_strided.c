/*
 * flux_image_update_region_strided / _premultiply: bounds + stride
 * validation, strided repack correctness, and premultiply math parity with
 * flux_color_rgba_premul (bit-exact, via an offscreen blit + readback).
 * Skips if no Vulkan device.
 */
#include "test_helpers.h"
#include <flux/flux.h>
#include <flux/vulkan.h>

#include <string.h>

#define IMG_W 16u
#define IMG_H 16u
#define SURF_W 16u
#define SURF_H 16u

/* RGBA8_UNORM texel bytes are R,G,B,A; flux_color is 0xAARRGGBB. */
static uint32_t straight_rgba(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    return (uint32_t)r | ((uint32_t)g << 8) | ((uint32_t)b << 16) | ((uint32_t)a << 24);
}
static uint8_t px_r(uint32_t p) { return (uint8_t)(p & 0xffu); }
static uint8_t px_g(uint32_t p) { return (uint8_t)((p >> 8) & 0xffu); }
static uint8_t px_b(uint32_t p) { return (uint8_t)((p >> 16) & 0xffu); }
static uint8_t px_a(uint32_t p) { return (uint8_t)((p >> 24) & 0xffu); }

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
    flux_image *image;
    flux_sampler *sampler;
} blit_case;

/* Identity blit: whole image to whole surface with a NEAREST sampler, so
 * surface pixel (x, y) reads back image texel (x, y) exactly. Rendering
 * follows the offscreen frame loop (ADR-0013): begin frame, draw with the
 * canvas, request the readback on the frame, then submit + present. */
static flux_result blit_and_read(flux_surface *s, flux_canvas *canvas, blit_case *bc,
                                 uint32_t *rb, size_t bytes) {
    flux_frame *frame = nullptr;
    flux_result r = flux_surface_begin_frame(s, nullptr, &frame);
    if (r != FLUX_OK)
        return r;
    flux_color clear = flux_color_rgba(0, 0, 0, 0);
    r = flux_canvas_begin_frame(canvas, frame, &clear);
    if (r != FLUX_OK)
        return r;
    flux_paint opaque = flux_paint_solid(flux_color_rgba(255, 255, 255, 255));
    opaque.blend = FLUX_BLEND_SRC;
    flux_canvas_draw_image_sampled(canvas, bc->image, bc->sampler,
                                   (flux_rect){0.0f, 0.0f, (float)SURF_W, (float)SURF_H},
                                   &opaque);
    flux_canvas_end_frame(canvas);
    r = flux_frame_request_readback(frame);
    if (r != FLUX_OK)
        return r;
    r = flux_frame_submit(frame);
    if (r != FLUX_OK)
        return r;
    r = flux_frame_present(frame);
    if (r != FLUX_OK)
        return r;
    return flux_surface_read_pixels(s, rb, bytes);
}

int main(void) {
    /* --- NULL safety --- */
    EXPECT(flux_image_update_region_strided(nullptr, 0, 0, 1, 1, "x", 4, 4) ==
           FLUX_ERROR_INVALID_ARGUMENT);
    EXPECT(flux_image_update_region_premultiply(nullptr, 0, 0, 1, 1, "x", 4, 4) ==
           FLUX_ERROR_INVALID_ARGUMENT);

    flux_device *d = test_helpers_make_headless_device();
    if (!d) {
        fprintf(stderr, "test_image_update_strided: no Vulkan device; skipping\n");
        TEST_SUMMARY();
    }

    uint32_t pixels[IMG_W * IMG_H];
    for (uint32_t i = 0; i < IMG_W * IMG_H; ++i)
        pixels[i] = straight_rgba(255, 0, 0, 255);
    flux_image_desc idesc = FLUX_IMAGE_DESC_INIT;
    idesc.width = IMG_W;
    idesc.height = IMG_H;
    idesc.format = FLUX_FORMAT_RGBA8_UNORM;
    idesc.initial_data = pixels;
    flux_image *img = nullptr;
    EXPECT(flux_image_create(d, &idesc, &img) == FLUX_OK && img != nullptr);

    flux_sampler_desc sdesc = FLUX_SAMPLER_DESC_INIT;
    sdesc.min_filter = FLUX_FILTER_NEAREST;
    sdesc.mag_filter = FLUX_FILTER_NEAREST;
    sdesc.address_u = FLUX_ADDRESS_CLAMP_TO_EDGE;
    sdesc.address_v = FLUX_ADDRESS_CLAMP_TO_EDGE;
    flux_sampler *nearest = nullptr;
    EXPECT(flux_sampler_create(d, &sdesc, &nearest) == FLUX_OK && nearest != nullptr);

    flux_surface *surf = nullptr;
    flux_surface_desc sfd = FLUX_SURFACE_DESC_INIT;
    sfd.width = SURF_W;
    sfd.height = SURF_H;
    EXPECT(flux_surface_create(d, &sfd, &surf) == FLUX_OK && surf != nullptr);

    flux_canvas *canvas = nullptr;
    {
        flux_canvas_desc cd = FLUX_CANVAS_DESC_INIT;
        cd.surface = surf;
        EXPECT(flux_canvas_create(&cd, &canvas) == FLUX_OK && canvas != nullptr);
    }

    blit_case bc = {.image = img, .sampler = nearest};
    uint32_t rb[SURF_W * SURF_H];

/* Render one blit frame and read the pixels back. */
#define BLIT_AND_READ() EXPECT(blit_and_read(surf, canvas, &bc, rb, sizeof(rb)) == FLUX_OK)

    /* --- strided validation --- */
    {
        uint32_t patch[16];
        /* row_bytes smaller than width * bpp */
        EXPECT(flux_image_update_region_strided(img, 0, 0, 8, 2, patch, 8, sizeof(patch)) ==
               FLUX_ERROR_INVALID_ARGUMENT);
        /* bytes too small for (h-1)*stride + w*bpp */
        EXPECT(flux_image_update_region_strided(img, 0, 0, 8, 2, patch, 64, 63) ==
               FLUX_ERROR_INVALID_ARGUMENT);
        /* the exact minimum is accepted */
        uint8_t exact[96];
        memset(exact, 0xAA, sizeof(exact));
        EXPECT(flux_image_update_region_strided(img, 0, 0, 8, 2, exact, 64, sizeof(exact)) ==
               FLUX_OK);
        /* zero extent still rejected */
        EXPECT(flux_image_update_region_strided(img, 0, 0, 0, 1, patch, 4, 4) ==
               FLUX_ERROR_INVALID_ARGUMENT);
    }

    /* --- strided happy path: 4x3 region at (2,2) from a 64-byte-pitch
     * (16 px wide) source — rows must come from the right offsets. --- */
    {
        uint32_t src[16 * 3];
        for (uint32_t row = 0; row < 3; ++row)
            for (uint32_t col = 0; col < 16; ++col)
                src[row * 16 + col] = straight_rgba((uint8_t)row, (uint8_t)col, 7, 255);
        EXPECT(flux_image_update_region_strided(img, 2, 2, 4, 3, src, 64, sizeof(src)) == FLUX_OK);
        BLIT_AND_READ();
        for (uint32_t row = 0; row < 3; ++row)
            for (uint32_t col = 0; col < 4; ++col) {
                /* The uploaded rows come from source columns 0..3 (the
                 * region starts at source (0,0)), landed at image (2,2). */
                uint32_t got = rb[(2 + row) * SURF_W + (2 + col)];
                EXPECT(px_near(got, straight_rgba((uint8_t)row, (uint8_t)col, 7, 255)));
            }
        /* outside the updated region: still the initial red (row 2's
         * untouched columns), proving the repack did not smear rows. */
        EXPECT(px_near(rb[2 * SURF_W + 0], straight_rgba(255, 0, 0, 255)));
    }

    /* --- premultiply: format gate --- */
    {
        flux_image_desc d8 = FLUX_IMAGE_DESC_INIT;
        d8.width = 2;
        d8.height = 2;
        d8.format = FLUX_FORMAT_R8_UNORM;
        uint8_t r8[4] = {1, 2, 3, 4};
        d8.initial_data = r8;
        flux_image *grey = nullptr;
        EXPECT(flux_image_create(d, &d8, &grey) == FLUX_OK);
        EXPECT(flux_image_update_region_premultiply(grey, 0, 0, 1, 1, "abcd", 4, 4) ==
               FLUX_ERROR_UNSUPPORTED);
        flux_image_release(grey);
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
        EXPECT(flux_image_update_region_premultiply(img, 4, 4, 8, 6, src, 64, sizeof(src)) ==
               FLUX_OK);
        BLIT_AND_READ();
        /* The uploaded region reads source rows 0..5, cols 0..7 and lands
         * at image (4,4). Expectation walks the *source* coordinates. */
        for (uint32_t srow = 0; srow < 6; ++srow)
            for (uint32_t scol = 0; scol < 8; ++scol) {
                uint32_t n = srow * 16 + scol;
                uint8_t r = chans[n % 6], g = chans[(n / 6) % 6], b = chans[(n / 12) % 6];
                uint8_t a = (uint8_t)((n * 7u) % 256u);
                uint32_t want_premul = flux_color_rgba_premul(r, g, b, a);
                uint8_t wr = (uint8_t)((want_premul >> 16) & 0xffu);
                uint8_t wg = (uint8_t)((want_premul >> 8) & 0xffu);
                uint8_t wb = (uint8_t)(want_premul & 0xffu);
                uint32_t got = rb[(4 + srow) * SURF_W + (4 + scol)];
                EXPECT(near8(px_r(got), wr) && near8(px_g(got), wg) && near8(px_b(got), wb) &&
                       near8(px_a(got), a));
            }
    }

    /* --- premultiply: packed call (row_bytes == 0) also accepted --- */
    {
        uint32_t src[4 * 2];
        for (uint32_t i = 0; i < 8; ++i)
            src[i] = straight_rgba(200, 100, 50, 128);
        EXPECT(flux_image_update_region_premultiply(img, 0, 0, 4, 2, src, 0, sizeof(src)) ==
               FLUX_OK);
        BLIT_AND_READ();
        /* straight RGBA(200,100,50,128) -> premul per flux_color_rgba_premul */
        uint32_t want = flux_color_rgba_premul(200, 100, 50, 128);
        uint8_t wr = (uint8_t)((want >> 16) & 0xff), wg = (uint8_t)((want >> 8) & 0xff),
                wb = (uint8_t)(want & 0xff);
        for (uint32_t i = 0; i < 8; ++i) {
            uint32_t got = rb[(i / 4) * SURF_W + (i % 4)];
            EXPECT(near8(px_r(got), wr) && near8(px_g(got), wg) && near8(px_b(got), wb) &&
                   near8(px_a(got), 128));
        }
    }

    flux_canvas_destroy(canvas);
    flux_surface_release(surf);
    flux_sampler_release(nearest);
    flux_image_release(img);
    flux_device_release(d);
    TEST_SUMMARY();
}
