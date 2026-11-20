/*
 * Headless CPU-canvas GLYPH rendering test (ADR-0019).
 *
 * Draws text on a device-less CPU canvas via glyph_ctx — the host R8 coverage
 * atlas path that replaced the old "silently drop glyph draws" behaviour. No
 * Vulkan device is required. The check is backend-availability-agnostic: if no
 * shaping backend is present the context falls back to measure-only and the
 * test skips (no ink drawn) rather than failing.
 */
#include "optics_test.h"
#include <optics/gfx/canvas.h>
#include <optics/gfx/canvas_cpu.h>
#include <optics/gfx/math.h>
#include <optics/text.h>
#include <string.h>

#define W 128
#define H 64

static optics_text_metrics measure(optics_text_ctx *t, const char *s, size_t len,
                                   const optics_text_style *style) {
    return optics_text_measure(t,
                               &(optics_text_measure_opts){.utf8 = s, .len = len, .style = style});
}

static void draw(optics_text_ctx *t, optics_gfx_canvas *canvas, optics_gfx_arena *arena, float x,
                 float y, const char *s, size_t len, const optics_text_style *style) {
    optics_text_draw(t, &(optics_text_draw_opts){.canvas = canvas,
                                                 .arena = arena,
                                                 .x = x,
                                                 .y = y,
                                                 .utf8 = s,
                                                 .len = len,
                                                 .style = style});
}

static void draw_outlined(optics_text_ctx *t, optics_gfx_canvas *canvas, optics_gfx_arena *arena,
                          float x, float y, const char *s, size_t len,
                          const optics_text_style *style, optics_gfx_color outline_color,
                          float outline_width) {
    optics_text_draw(t, &(optics_text_draw_opts){.canvas = canvas,
                                                 .arena = arena,
                                                 .x = x,
                                                 .y = y,
                                                 .utf8 = s,
                                                 .len = len,
                                                 .style = style,
                                                 .outline_color = outline_color,
                                                 .outline_width = outline_width});
}

/* Render a 17px probe either cold or after interleaving a 33px layout. The
 * two images must be identical: measuring another size may change which
 * FT_Size is active, but never the bitmap stored under the 17px glyph-cache
 * key. This regresses the retained-size LRU corruption found by a real editor
 * that lays out all heading sizes before painting its body text. */
static bool render_size_probe(bool interleaved, uint8_t out[W * H * 4]) {
    optics_text_ctx *text = nullptr;
    if (optics_text_create(&(optics_text_desc){.device = nullptr, .scale = 1.0f}, &text) !=
            OPTICS_GFX_OK ||
        !text)
        return false;

    optics_text_style small = {.size_px = 17.0f,
                               .color = optics_gfx_color_rgba_premul(255, 255, 255, 255)};
    optics_text_style large = {.size_px = 33.0f,
                               .color = optics_gfx_color_rgba_premul(255, 255, 255, 255)};
    if (measure(text, "sf", 2, &small).width <= 0.0f) {
        optics_text_release(text);
        return false;
    }
    if (interleaved) {
        (void)measure(text, "sf", 2, &large);
        (void)measure(text, "sf", 2, &small);
    }

    optics_gfx_canvas *canvas = nullptr;
    if (optics_gfx_canvas_create(&(optics_gfx_canvas_desc){.type = OPTICS_GFX_TYPE_CANVAS_DESC,
                                                           .backend = OPTICS_GFX_CANVAS_BACKEND_CPU,
                                                           .scale = 1.0f,
                                                           .width = W,
                                                           .height = H},
                                 &canvas) != OPTICS_GFX_OK ||
        !canvas) {
        optics_text_release(text);
        return false;
    }
    optics_gfx_color black = optics_gfx_color_rgba_premul(0, 0, 0, 255);
    if (optics_gfx_canvas_begin(
            canvas, &(optics_gfx_canvas_pass_desc){.type = OPTICS_GFX_TYPE_CANVAS_PASS_DESC,
                                                   .clear_color = &black}) != OPTICS_GFX_OK) {
        optics_gfx_canvas_release(canvas);
        optics_text_release(text);
        return false;
    }
    draw(text, canvas, nullptr, 4.0f, 4.0f, "sf", 2, &small);
    OT_EXPECT(optics_gfx_canvas_end(canvas) == OPTICS_GFX_OK);

    uint32_t w = 0, h = 0, stride = 0;
    const uint8_t *pixels = optics_gfx_canvas_cpu_pixels(canvas, &w, &h, &stride);
    bool ok = pixels && w == W && h == H && stride == W * 4;
    if (ok)
        memcpy(out, pixels, sizeof(uint8_t[W * H * 4]));
    optics_gfx_canvas_release(canvas);
    optics_text_release(text);
    return ok;
}

int main(void) {
    optics_text_ctx *t = nullptr;
    OT_EXPECT(optics_text_create(&(optics_text_desc){.device = nullptr, .scale = 1.0f}, &t) ==
              OPTICS_GFX_OK);
    OT_EXPECT(t != nullptr);

    /* Gate on the backend being live: measure returns a width only when
     * FreeType/HarfBuzz are present. If not, skip (no ink to check). */
    optics_text_metrics m = measure(t, "A", 1, nullptr);
    if (m.width <= 0.0f) {
        optics_text_release(t);
        OT_SKIP("no shaping backend");
    }

    optics_gfx_canvas *c = nullptr;
    OT_EXPECT(
        optics_gfx_canvas_create(&(optics_gfx_canvas_desc){.type = OPTICS_GFX_TYPE_CANVAS_DESC,
                                                           .backend = OPTICS_GFX_CANVAS_BACKEND_CPU,
                                                           .scale = 1.0f,
                                                           .width = W,
                                                           .height = H},
                                 &c) == OPTICS_GFX_OK);
    OT_EXPECT(c != nullptr);

    optics_gfx_color black = optics_gfx_color_rgba_premul(0, 0, 0, 255);
    optics_gfx_color white = optics_gfx_color_rgba_premul(255, 255, 255, 255);
    OT_EXPECT(optics_gfx_canvas_begin(
                  c, &(optics_gfx_canvas_pass_desc){.type = OPTICS_GFX_TYPE_CANVAS_PASS_DESC,
                                                    .clear_color = &black}) == OPTICS_GFX_OK);

    optics_text_style style = {};
    style.size_px = 24.0f;
    style.color = white;
    draw(t, c, nullptr, 8.0f, 8.0f, "Aa", 2, &style);
    draw_outlined(t, c, nullptr, 72.0f, 8.0f, "Aa", 2, &style,
                  optics_gfx_color_rgba_premul(255, 0, 0, 255), 2.0f);

    OT_EXPECT(optics_gfx_canvas_end(c) == OPTICS_GFX_OK);

    uint32_t w = 0, h = 0, stride = 0;
    const uint8_t *fb = optics_gfx_canvas_cpu_pixels(c, &w, &h, &stride);
    OT_EXPECT(fb != nullptr && w == W && h == H && stride == W * 4);

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
    OT_EXPECT(lit > 100);
    /* The outlined run must retain visible contour pixels after its opaque
     * white foreground is painted over the centre. */
    OT_EXPECT(outline > 20);

    uint8_t cold[W * H * 4], interleaved[W * H * 4];
    OT_EXPECT(render_size_probe(false, cold));
    OT_EXPECT(render_size_probe(true, interleaved));
    OT_EXPECT(memcmp(cold, interleaved, sizeof cold) == 0);

    optics_gfx_canvas_release(c);
    optics_text_release(t);
    return OT_REPORT();
}
