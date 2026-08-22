/*
 * Headless CPU-canvas GLYPH rendering test (ADR-0019).
 *
 * Draws text on a device-less CPU canvas via flux_text — the host R8 coverage
 * atlas path that replaced the old "silently drop glyph draws" behaviour. No
 * Vulkan device is required. The check is backend-availability-agnostic: if no
 * shaping backend is present the context falls back to measure-only and the
 * test skips (no ink drawn) rather than failing.
 */
#include "test_helpers.h"
#include <flux-text/text.h>
#include <flux/canvas.h>
#include <flux/canvas_cpu.h>
#include <flux/math.h>
#include <string.h>

#define W 128
#define H 64

/* Render a 17px probe either cold or after interleaving a 33px layout. The
 * two images must be identical: measuring another size may change which
 * FT_Size is active, but never the bitmap stored under the 17px glyph-cache
 * key. This regresses the retained-size LRU corruption found by a real editor
 * that lays out all heading sizes before painting its body text. */
static bool render_size_probe(bool interleaved, uint8_t out[W * H * 4]) {
    flux_text *text = nullptr;
    if (flux_text_create(&(flux_text_desc){.device = nullptr, .scale = 1.0f}, &text) != FLUX_OK ||
        !text)
        return false;

    flux_text_style small = {.size_px = 17.0f, .color = flux_color_rgba_premul(255, 255, 255, 255)};
    flux_text_style large = {.size_px = 33.0f, .color = flux_color_rgba_premul(255, 255, 255, 255)};
    if (flux_text_measure(text, "sf", 2, &small).width <= 0.0f) {
        flux_text_destroy(text);
        return false;
    }
    if (interleaved) {
        (void)flux_text_measure(text, "sf", 2, &large);
        (void)flux_text_measure(text, "sf", 2, &small);
    }

    flux_canvas *canvas = nullptr;
    if (flux_canvas_create_cpu(W, H, 1.0f, &canvas) != FLUX_OK || !canvas) {
        flux_text_destroy(text);
        return false;
    }
    flux_color black = flux_color_rgba_premul(0, 0, 0, 255);
    if (flux_canvas_cpu_begin(canvas, &black) != FLUX_OK) {
        flux_canvas_destroy(canvas);
        flux_text_destroy(text);
        return false;
    }
    flux_text_draw(text, canvas, nullptr, 4.0f, 4.0f, "sf", 2, &small);
    flux_canvas_cpu_end(canvas);

    uint32_t w = 0, h = 0, stride = 0;
    const uint8_t *pixels = flux_canvas_cpu_pixels(canvas, &w, &h, &stride);
    bool ok = pixels && w == W && h == H && stride == W * 4;
    if (ok)
        memcpy(out, pixels, sizeof(uint8_t[W * H * 4]));
    flux_canvas_destroy(canvas);
    flux_text_destroy(text);
    return ok;
}

int main(void) {
    flux_text *t = nullptr;
    EXPECT(flux_text_create(&(flux_text_desc){.device = nullptr, .scale = 1.0f}, &t) == FLUX_OK);
    EXPECT(t != nullptr);

    /* Gate on the backend being live: measure returns a width only when
     * FreeType/HarfBuzz are present. If not, skip (no ink to check). */
    flux_text_metrics m = flux_text_measure(t, "A", 1, nullptr);
    if (m.width <= 0.0f)
        TEST_SUMMARY();

    flux_canvas *c = nullptr;
    EXPECT(flux_canvas_create_cpu(W, H, 1.0f, &c) == FLUX_OK);
    EXPECT(c != nullptr);

    flux_color black = flux_color_rgba_premul(0, 0, 0, 255);
    flux_color white = flux_color_rgba_premul(255, 255, 255, 255);
    EXPECT(flux_canvas_cpu_begin(c, &black) == FLUX_OK);

    flux_text_style style = {0};
    style.size_px = 24.0f;
    style.color = white;
    flux_text_draw(t, c, nullptr, 8.0f, 8.0f, "Aa", 2, &style);
    flux_text_draw_outlined(t, c, nullptr, 72.0f, 8.0f, "Aa", 2, &style,
                            flux_color_rgba_premul(255, 0, 0, 255), 2.0f);

    flux_canvas_cpu_end(c);

    uint32_t w = 0, h = 0, stride = 0;
    const uint8_t *fb = flux_canvas_cpu_pixels(c, &w, &h, &stride);
    EXPECT(fb != nullptr && w == W && h == H && stride == W * 4);

    /* Count lit (non-black) pixels in the band where the text should sit. */
    int lit = 0;
    int outline = 0;
    for (uint32_t y = 0; y < H; y++) {
        for (uint32_t x = 0; x < W; x++) {
            const uint8_t *p = fb + (size_t)y * stride + x * 4;
            if (p[0] > 40 || p[1] > 40 || p[2] > 40)
                lit++;
            if (p[0] > 80 && p[0] > p[1] * 2 && p[0] > p[2] * 2)
                outline++;
        }
    }
    /* A 24px glyph should deposit well over a hundred lit pixels. The exact
     * count is font-dependent, so a loose floor is enough to prove the host
     * atlas path produced ink (vs. the old behaviour of zero). */
    EXPECT(lit > 100);
    /* The outlined run must retain visible contour pixels after its opaque
     * white foreground is painted over the centre. */
    EXPECT(outline > 20);

    uint8_t cold[W * H * 4], interleaved[W * H * 4];
    EXPECT(render_size_probe(false, cold));
    EXPECT(render_size_probe(true, interleaved));
    EXPECT(memcmp(cold, interleaved, sizeof cold) == 0);

    flux_canvas_destroy(c);
    flux_text_destroy(t);
    TEST_SUMMARY();
}
