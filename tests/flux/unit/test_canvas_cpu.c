/*
 * Headless CPU-canvas rendering test. No Vulkan device or window: it exercises
 * the software backend (flux_canvas_backend_cpu) end to end — create, record,
 * read back pixels — across the solid, SDF rounded-rect, and gradient paths.
 */
#include "test_helpers.h"
#include <flux/canvas.h>
#include <flux/canvas_cpu.h>
#include <flux/math.h>

/* Fetch an RGBA8 pixel from a premultiplied framebuffer. */
static void px(const uint8_t *fb, uint32_t stride, uint32_t x, uint32_t y, uint8_t out[4]) {
    const uint8_t *p = fb + (size_t)y * stride + (size_t)x * 4;
    out[0] = p[0];
    out[1] = p[1];
    out[2] = p[2];
    out[3] = p[3];
}

int main(void) {
    const uint32_t W = 64, H = 64;

    /* ---- Solid fill on an opaque black clear ---- */
    flux_canvas *c = nullptr;
    EXPECT(flux_canvas_create_cpu(W, H, 1.0f, &c) == FLUX_OK);
    EXPECT(c != nullptr);

    flux_color black = flux_color_rgba_premul(0, 0, 0, 255);
    flux_color red = flux_color_rgba_premul(255, 0, 0, 255);
    EXPECT(flux_canvas_cpu_begin(c, &black) == FLUX_OK);
    flux_canvas_fill_rect_color(c, (flux_rect){16, 16, 32, 32}, red);
    flux_canvas_cpu_end(c);

    uint32_t w = 0, h = 0, stride = 0;
    const uint8_t *fb = flux_canvas_cpu_pixels(c, &w, &h, &stride);
    EXPECT(fb != nullptr);
    EXPECT(w == W && h == H && stride == W * 4);

    uint8_t p[4];
    px(fb, stride, 32, 32, p); /* inside the rect → red */
    EXPECT(p[0] > 250 && p[1] < 5 && p[2] < 5 && p[3] > 250);
    px(fb, stride, 3, 3, p); /* outside → cleared black */
    EXPECT(p[0] < 5 && p[1] < 5 && p[2] < 5 && p[3] > 250);
    px(fb, stride, 40, 40, p); /* still inside */
    EXPECT(p[0] > 250 && p[3] > 250);

    /* ---- SDF rounded rect on a transparent clear: corners are cut ---- */
    EXPECT(flux_canvas_cpu_begin(c, nullptr) == FLUX_OK);
    flux_color white = flux_color_rgba_premul(255, 255, 255, 255);
    flux_canvas_fill_rrect(c, (flux_rect){0, 0, 64, 64}, 20.0f, white);
    flux_canvas_cpu_end(c);
    fb = flux_canvas_cpu_pixels(c, &w, &h, &stride);
    px(fb, stride, 32, 32, p); /* centre → opaque white */
    EXPECT(p[0] > 250 && p[1] > 250 && p[2] > 250 && p[3] > 250);
    px(fb, stride, 1, 1, p); /* rounded corner → transparent */
    EXPECT(p[3] < 20);

    /* ---- Clip rect confines a fill ---- */
    EXPECT(flux_canvas_cpu_begin(c, &black) == FLUX_OK);
    flux_canvas_clip_rect(c, (flux_rect){0, 0, 32, 64});
    flux_canvas_fill_rect_color(c, (flux_rect){0, 0, 64, 64}, red);
    flux_canvas_cpu_end(c);
    fb = flux_canvas_cpu_pixels(c, &w, &h, &stride);
    px(fb, stride, 10, 32, p); /* inside clip → red */
    EXPECT(p[0] > 250 && p[3] > 250);
    px(fb, stride, 50, 32, p); /* outside clip → untouched black */
    EXPECT(p[0] < 5 && p[3] > 250);

    flux_canvas_destroy(c);

    /* ---- Linear gradient ---- */
    EXPECT(flux_canvas_create_cpu(W, H, 1.0f, &c) == FLUX_OK);
    flux_gradient_stop stops[2] = {
        {0.0f, flux_color_rgba_premul(255, 0, 0, 255)},
        {1.0f, flux_color_rgba_premul(0, 0, 255, 255)},
    };
    flux_paint g =
        flux_paint_linear_gradient((flux_point){0, 0}, (flux_point){(float)W, 0}, stops, 2);
    EXPECT(flux_canvas_cpu_begin(c, &black) == FLUX_OK);
    flux_canvas_fill_rect(c, (flux_rect){0, 0, (float)W, (float)H}, &g);
    flux_canvas_cpu_end(c);
    fb = flux_canvas_cpu_pixels(c, &w, &h, &stride);
    px(fb, stride, 2, 32, p); /* left → red end */
    EXPECT(p[0] > 200 && p[2] < 60);
    px(fb, stride, 61, 32, p); /* right → blue end */
    EXPECT(p[2] > 200 && p[0] < 60);
    flux_canvas_destroy(c);

    /* ---- Unified factory + pass API (Skia SkSurface-style) ---- */
    flux_canvas_desc d = FLUX_CANVAS_DESC_INIT;
    d.backend = FLUX_CANVAS_BACKEND_CPU;
    d.width = W;
    d.height = H;
    d.scale = 1.0f;
    EXPECT(flux_canvas_create(&d, &c) == FLUX_OK);
    EXPECT(flux_canvas_begin_frame(c, nullptr, &black) == FLUX_OK);
    flux_canvas_fill_rect_color(c, (flux_rect){0, 0, (float)W, (float)H}, red);
    flux_canvas_end_frame(c);
    fb = flux_canvas_read_pixels(c, &w, &h, &stride);
    EXPECT(fb != nullptr && w == W && h == H);
    px(fb, stride, 32, 32, p);
    EXPECT(p[0] > 250 && p[1] < 5 && p[2] < 5 && p[3] > 250);
    flux_canvas_destroy(c);

    TEST_SUMMARY();
}
