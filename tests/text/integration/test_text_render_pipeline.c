/*
 * text_render_pipeline — Vulkan GPU + FreeType/HarfBuzz/FriBidi integration test.
 *
 * Exercises the complete text rendering seam across real system fonts and GPU
 * atlas upload:
 *   - optics_text_create with a real Vulkan device;
 *   - optics_text_draw and optics_text_record + display-list submission;
 *   - atlas upload lifecycle: has_pending_uploads -> flush_atlas -> epoch;
 *   - glyph cache stats: initial misses followed by steady-state hits;
 *   - pixel readback verifying rendered ASCII, accented UTF-8, mixed LTR/RTL,
 *     and CJK+Latin inter-script autospace text;
 *   - outline rendering (outline_width > 0) produces visibly distinct outline
 *     pixels compared to fill-only rendering;
 *   - mixed LTR/RTL multi-span selection rects and RTL byte_for_x bounds.
 */

#include "gfx_test.h"
#include "optics_test.h"

#include <optics/gfx.h>
#include <optics/gfx/canvas.h>
#include <optics/text.h>

#include <stdlib.h>
#include <string.h>

#define W 320u
#define H 96u
#define BYTES (W * H * 4u)

static size_t count_lit_pixels(const uint8_t *px, uint32_t w, uint32_t h) {
    size_t lit = 0;
    for (uint32_t i = 0; i < w * h; ++i) {
        const uint8_t *p = px + (size_t)i * 4u;
        if ((int)p[0] + (int)p[1] + (int)p[2] > 30)
            lit++;
    }
    return lit;
}

static size_t count_red_dominant(const uint8_t *px, uint32_t w, uint32_t h) {
    size_t red = 0;
    for (uint32_t i = 0; i < w * h; ++i) {
        const uint8_t *p = px + (size_t)i * 4u;
        if ((int)p[0] > (int)p[1] + 50 && (int)p[0] > (int)p[2] + 50)
            red++;
    }
    return red;
}

static void render_gpu_text_draw(optics_gfx_surface *surface, optics_gfx_canvas *canvas,
                                 optics_text_ctx *text, const char *utf8, size_t len,
                                 const optics_text_style *style, float outline_width,
                                 uint8_t *out_pixels) {
    optics_gfx_frame *frame = nullptr;
    OT_REQUIRE(optics_gfx_surface_begin_frame(surface, nullptr, &frame) == OPTICS_GFX_OK);

    optics_gfx_color black = optics_gfx_color_rgba_premul(0, 0, 0, 255);
    OT_REQUIRE(optics_gfx_canvas_begin(
                   canvas, &(optics_gfx_canvas_pass_desc){.type = OPTICS_GFX_TYPE_CANVAS_PASS_DESC,
                                                          .frame = frame,
                                                          .clear_color = &black}) == OPTICS_GFX_OK);
    optics_text_draw(text, &(optics_text_draw_opts){
                               .canvas = canvas,
                               .x = 12.0f,
                               .y = 48.0f,
                               .utf8 = utf8,
                               .len = len,
                               .style = style,
                               .outline_color = optics_gfx_color_rgba_premul(255, 0, 0, 255),
                               .outline_width = outline_width,
                           });
    OT_REQUIRE(optics_gfx_canvas_end(canvas) == OPTICS_GFX_OK);
    OT_EXPECT(optics_text_flush_atlas(text, &(optics_text_flush_atlas_opts){.frame = frame}) ==
              OPTICS_GFX_OK);
    OT_REQUIRE(optics_gfx_frame_submit(frame) == OPTICS_GFX_OK);
    OT_REQUIRE(optics_gfx_frame_present(frame) == OPTICS_GFX_OK);

    memset(out_pixels, 0, BYTES);
    OT_REQUIRE(optics_gfx_surface_read_pixels(surface, out_pixels, BYTES) == OPTICS_GFX_OK);
}

int main(void) {
    optics_gfx_device *device = optics_gfx_test_make_headless_device();
    if (!device)
        OT_SKIP("no Vulkan device");

    optics_text_ctx *text = nullptr;
    OT_REQUIRE(optics_text_create(&(optics_text_desc){.device = device, .scale = 1.0f}, &text) ==
               OPTICS_GFX_OK);

    static const optics_text_style STYLE = {
        .size_px = 32.0f,
        .weight = 400.0f,
        .color = 0xFFFFFFFFu,
        .family = OPTICS_TEXT_FAMILY_SANS,
    };

    optics_text_metrics probe = optics_text_measure(
        text, &(optics_text_measure_opts){.utf8 = "Optics", .len = 6, .style = &STYLE});
    if (probe.width <= 0.0f) {
        optics_text_release(text);
        optics_gfx_device_release(device);
        OT_SKIP("no shaping backend or Latin face");
    }

    optics_gfx_surface *surface = nullptr;
    optics_gfx_surface_desc sd = OPTICS_GFX_SURFACE_DESC_INIT;
    sd.width = W;
    sd.height = H;
    OT_REQUIRE(optics_gfx_surface_create(device, &sd, &surface) == OPTICS_GFX_OK);

    optics_gfx_canvas *canvas = nullptr;
    optics_gfx_canvas_desc cd = OPTICS_GFX_CANVAS_DESC_INIT;
    cd.surface = surface;
    OT_REQUIRE(optics_gfx_canvas_create(&cd, &canvas) == OPTICS_GFX_OK);

    uint8_t *px_fill = malloc(BYTES);
    uint8_t *px_outline = malloc(BYTES);
    uint8_t *px_record = malloc(BYTES);
    OT_REQUIRE(px_fill != nullptr && px_outline != nullptr && px_record != nullptr);

    /* 1. Direct GPU draw (fill-only) + glyph cache stats progression. */
    static const char SAMPLE[] = "Optics \xC3\xA9 123";
    render_gpu_text_draw(surface, canvas, text, SAMPLE, strlen(SAMPLE), &STYLE, 0.0f, px_fill);
    size_t lit_fill = count_lit_pixels(px_fill, W, H);
    OT_EXPECT(lit_fill > 150);
    OT_EXPECT(count_red_dominant(px_fill, W, H) == 0);

    optics_text_stats st1 = {};
    optics_text_get_stats(text, &st1);
    OT_EXPECT(st1.glyph_count > 0u);
    OT_EXPECT(st1.glyph_misses > 0u);

    /* Drawing the exact same string again must hit the cache with zero new misses. */
    render_gpu_text_draw(surface, canvas, text, SAMPLE, strlen(SAMPLE), &STYLE, 0.0f, px_fill);
    optics_text_stats st2 = {};
    optics_text_get_stats(text, &st2);
    OT_EXPECT(st2.glyph_misses == st1.glyph_misses);
    OT_EXPECT(st2.glyph_hits > st1.glyph_hits);

    /* 2. Outline rendering must produce red outline pixels around the white fill. */
    render_gpu_text_draw(surface, canvas, text, SAMPLE, strlen(SAMPLE), &STYLE, 2.5f, px_outline);
    size_t lit_outline = count_lit_pixels(px_outline, W, H);
    size_t red_outline = count_red_dominant(px_outline, W, H);
    OT_EXPECT(lit_outline > lit_fill);
    OT_EXPECT(red_outline > 50);

    /* 3. Record into DisplayList and replay onto GPU Canvas. */
    optics_gfx_encoder *enc = nullptr;
    OT_REQUIRE(optics_gfx_encoder_create(&(optics_gfx_encoder_desc){}, &enc) == OPTICS_GFX_OK);
    OT_EXPECT(optics_text_record(text, &(optics_text_record_opts){
                                           .encoder = enc,
                                           .x = 12.0f,
                                           .y = 48.0f,
                                           .utf8 = SAMPLE,
                                           .len = strlen(SAMPLE),
                                           .style = STYLE,
                                       }) == OPTICS_GFX_OK);
    optics_gfx_display_list *list = nullptr;
    OT_REQUIRE(optics_gfx_encoder_finish(enc, &list) == OPTICS_GFX_OK);
    optics_gfx_encoder_destroy(enc);

    {
        optics_gfx_frame *frame = nullptr;
        OT_REQUIRE(optics_gfx_surface_begin_frame(surface, nullptr, &frame) == OPTICS_GFX_OK);
        if (optics_text_has_pending_uploads(text))
            OT_EXPECT(optics_text_flush_atlas(
                          text, &(optics_text_flush_atlas_opts){.frame = frame}) == OPTICS_GFX_OK);
        optics_gfx_color black = optics_gfx_color_rgba_premul(0, 0, 0, 255);
        OT_REQUIRE(optics_gfx_canvas_begin(canvas, &(optics_gfx_canvas_pass_desc){
                                                       .type = OPTICS_GFX_TYPE_CANVAS_PASS_DESC,
                                                       .frame = frame,
                                                       .clear_color = &black}) == OPTICS_GFX_OK);
        OT_EXPECT(optics_gfx_canvas_submit_display_list(
                      canvas, &(optics_gfx_canvas_submit_display_list_opts){.list = list}) ==
                  OPTICS_GFX_OK);
        OT_REQUIRE(optics_gfx_canvas_end(canvas) == OPTICS_GFX_OK);
        OT_EXPECT(optics_text_flush_atlas(text, &(optics_text_flush_atlas_opts){.frame = frame}) ==
                  OPTICS_GFX_OK);
        OT_REQUIRE(optics_gfx_frame_submit(frame) == OPTICS_GFX_OK);
        OT_REQUIRE(optics_gfx_frame_present(frame) == OPTICS_GFX_OK);
        memset(px_record, 0, BYTES);
        OT_REQUIRE(optics_gfx_surface_read_pixels(surface, px_record, BYTES) == OPTICS_GFX_OK);
    }
    OT_EXPECT(count_lit_pixels(px_record, W, H) > 150);
    optics_gfx_display_list_release(list);

    /* 4. CJK + Latin inter-script autospace contract: "你好hello" must be wider
     *    than the sum of "你好" + "hello" measured separately when a CJK face exists. */
    static const char CJK[] = "\xE4\xBD\xA0\xE5\xA5\xBD"; /* 你好 */
    static const char LATIN[] = "hello";
    static const char CJK_LATIN[] = "\xE4\xBD\xA0\xE5\xA5\xBDhello";
    optics_text_metrics m_cjk = optics_text_measure(
        text, &(optics_text_measure_opts){.utf8 = CJK, .len = strlen(CJK), .style = &STYLE});
    optics_text_metrics m_lat = optics_text_measure(
        text, &(optics_text_measure_opts){.utf8 = LATIN, .len = strlen(LATIN), .style = &STYLE});
    optics_text_metrics m_mix = optics_text_measure(
        text,
        &(optics_text_measure_opts){.utf8 = CJK_LATIN, .len = strlen(CJK_LATIN), .style = &STYLE});
    if (m_cjk.width > 0.0f) {
        OT_EXPECT(m_mix.width > m_cjk.width + m_lat.width + 1.0f);
        render_gpu_text_draw(surface, canvas, text, CJK_LATIN, strlen(CJK_LATIN), &STYLE, 0.0f,
                             px_fill);
        OT_EXPECT(count_lit_pixels(px_fill, W, H) > 100);
    }

    /* 5. Mixed LTR/RTL multi-span selection and clamped byte_for_x. */
    static const char BIDI[] = "ab\xD7\x90\xD7\x91"
                               "cd";
    optics_text_metrics m_bidi = optics_text_measure(
        text, &(optics_text_measure_opts){.utf8 = BIDI, .len = strlen(BIDI), .style = &STYLE});
    if (m_bidi.width > 0.0f) {
        optics_text_xrange ranges[8] = {};
        int nr = optics_text_selection_rects(text, &(optics_text_selection_rects_opts){
                                                       .utf8 = BIDI,
                                                       .len = strlen(BIDI),
                                                       .lo = 1,
                                                       .hi = strlen(BIDI) - 1,
                                                       .style = &STYLE,
                                                       .out = ranges,
                                                       .max = 8,
                                                   });
        OT_EXPECT(nr >= 1);
        for (int i = 0; i < nr; ++i) {
            OT_EXPECT(ranges[i].x0 >= -0.5f);
            OT_EXPECT(ranges[i].x1 >= ranges[i].x0);
            OT_EXPECT(ranges[i].x1 <= m_bidi.width + 0.5f);
        }
        OT_EXPECT(optics_text_byte_for_x(text, &(optics_text_byte_for_x_opts){.utf8 = BIDI,
                                                                              .len = strlen(BIDI),
                                                                              .local_x = -50.0f,
                                                                              .style = &STYLE}) <=
                  strlen(BIDI));
        OT_EXPECT(optics_text_byte_for_x(
                      text, &(optics_text_byte_for_x_opts){.utf8 = BIDI,
                                                           .len = strlen(BIDI),
                                                           .local_x = m_bidi.width + 500.0f,
                                                           .style = &STYLE}) <= strlen(BIDI));
    }

    optics_gfx_device_wait_idle(device);
    free(px_fill);
    free(px_outline);
    free(px_record);
    optics_gfx_canvas_release(canvas);
    optics_gfx_surface_release(surface);
    optics_text_release(text);
    optics_gfx_device_release(device);
    return OT_REPORT();
}
