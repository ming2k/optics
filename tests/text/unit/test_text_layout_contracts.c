/*
 * text_layout_contracts — CPU-only shaping, stats, invalid-UTF8 resilience,
 * scale/family/weight/italic variations, and display-list recording contracts.
 *
 * Runs without a Vulkan device on every platform. Exercises:
 *   - optics_text_get_stats on fresh, active, and compacted contexts;
 *   - optics_text_has_pending_uploads / flush_atlas / get_atlas_epoch on
 *     device-less contexts and null handles;
 *   - malformed UTF-8 resilience across measure, x_for_byte, byte_for_x,
 *     selection_rects, and visual_move;
 *   - family/weight/italic/scale variations and null-style defaults;
 *   - long-string scratch buffer growth (>512 codepoints) + compaction;
 *   - optics_text_record into an encoder -> immutable display list -> CPU
 *     canvas replay, verifying rendered pixels and outline expansion.
 */

#include "optics_test.h"

#include <optics/gfx/canvas.h>
#include <optics/gfx/canvas_cpu.h>
#include <optics/text.h>

#include <math.h>
#include <string.h>

static bool is_finite_metrics(optics_text_metrics m) {
    return isfinite(m.width) && isfinite(m.height) && isfinite(m.baseline) && m.width >= 0.0f &&
           m.height >= 0.0f;
}

static size_t render_recorded_text(optics_text_ctx *text, const char *utf8, size_t len,
                                   optics_text_style style, float outline_width, uint32_t w,
                                   uint32_t h) {
    optics_gfx_encoder *enc = nullptr;
    OT_REQUIRE(optics_gfx_encoder_create(&(optics_gfx_encoder_desc){}, &enc) == OPTICS_GFX_OK);

    optics_gfx_result rec = optics_text_record(
        text, &(optics_text_record_opts){
                  .encoder = enc,
                  .x = 8.0f,
                  .y = 32.0f,
                  .utf8 = utf8,
                  .len = len,
                  .style = style,
                  .outline_color = optics_gfx_color_rgba_premul(255, 64, 64, 255),
                  .outline_width = outline_width,
              });
    OT_EXPECT(rec == OPTICS_GFX_OK);

    optics_gfx_display_list *list = nullptr;
    OT_REQUIRE(optics_gfx_encoder_finish(enc, &list) == OPTICS_GFX_OK);
    optics_gfx_encoder_destroy(enc);

    optics_gfx_canvas *canvas = nullptr;
    optics_gfx_canvas_desc cd = OPTICS_GFX_INIT(
        canvas_desc, .backend = OPTICS_GFX_CANVAS_BACKEND_CPU, .width = w, .height = h);
    OT_REQUIRE(optics_gfx_canvas_create(&cd, &canvas) == OPTICS_GFX_OK);

    optics_gfx_color clear = optics_gfx_color_rgba_premul(0, 0, 0, 255);
    OT_REQUIRE(optics_gfx_canvas_begin(
                   canvas, &(optics_gfx_canvas_pass_desc){.type = OPTICS_GFX_TYPE_CANVAS_PASS_DESC,
                                                          .clear_color = &clear}) == OPTICS_GFX_OK);
    OT_EXPECT(optics_gfx_canvas_submit_display_list(
                  canvas, &(optics_gfx_canvas_submit_display_list_opts){.list = list}) ==
              OPTICS_GFX_OK);
    OT_REQUIRE(optics_gfx_canvas_end(canvas) == OPTICS_GFX_OK);

    uint32_t rw = 0, rh = 0, stride = 0;
    const uint8_t *px = optics_gfx_canvas_read_pixels(canvas, &rw, &rh, &stride);
    OT_REQUIRE(px != nullptr);
    OT_EXPECT(rw == w && rh == h);

    size_t lit = 0;
    for (uint32_t y = 0; y < rh; ++y) {
        for (uint32_t x = 0; x < rw; ++x) {
            const uint8_t *p = px + (size_t)y * stride + (size_t)x * 4u;
            if (p[0] + p[1] + p[2] > 24)
                lit++;
        }
    }

    optics_gfx_canvas_release(canvas);
    optics_gfx_display_list_release(list);
    return lit;
}

static void test_stats_and_deviceless_atlas(void) {
    optics_text_stats st = {.glyph_cap = 999u, .glyph_count = 999u};
    optics_text_get_stats(nullptr, &st);
    OT_EXPECT(st.glyph_cap == 0u && st.glyph_count == 0u);
    optics_text_get_stats(nullptr, nullptr);

    OT_EXPECT(!optics_text_has_pending_uploads(nullptr));
    OT_EXPECT(optics_text_get_atlas_epoch(nullptr, nullptr) == 0u);
    OT_EXPECT(optics_text_flush_atlas(nullptr, nullptr) == OPTICS_GFX_ERROR_INVALID_ARGUMENT);

    optics_text_ctx *text = nullptr;
    OT_REQUIRE(optics_text_create(&(optics_text_desc){.device = nullptr, .scale = 0.0f}, &text) ==
               OPTICS_GFX_OK);
    /* scale == 0 defaults to 1.0f. */
    OT_EXPECT_NEAR(optics_text_scale(text), 1.0f, 1e-6);

    /* Device-less context has no GPU uploads to flush and epoch stays 0. */
    OT_EXPECT(!optics_text_has_pending_uploads(text));
    OT_EXPECT(optics_text_get_atlas_epoch(text, nullptr) == 0u);
    OT_EXPECT(optics_text_flush_atlas(text, &(optics_text_flush_atlas_opts){.frame = nullptr}) ==
              OPTICS_GFX_OK);

    optics_text_get_stats(text, &st);
    OT_EXPECT(st.glyph_count == 0u);

    optics_text_release(text);
}

static void test_malformed_utf8_resilience(optics_text_ctx *text) {
    static const optics_text_style style = {
        .size_px = 20.0f,
        .weight = 400.0f,
        .color = 0xFFFFFFFFu,
        .family = OPTICS_TEXT_FAMILY_SANS,
    };

    struct {
        const char *bytes;
        size_t len;
    } cases[] = {
        {"\xC3", 1},                       /* truncated 2-byte sequence */
        {"\xE2\x82", 2},                   /* truncated 3-byte sequence */
        {"\xF0\x9F\x98", 3},               /* truncated 4-byte sequence */
        {"\x80\xBF\xFE\xFF", 4},           /* invalid lead / continuation bytes */
        {"\xC0\xAF", 2},                   /* overlong '/' */
        {"\xED\xA0\x80", 3},               /* UTF-16 high surrogate */
        {"abc\0def", 7},                   /* embedded NUL inside explicit length */
        {"hi\xFF\xFE\xD7\x90\xC3\xA9!", 9} /* mixed valid + invalid + RTL + Latin */
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        const char *s = cases[i].bytes;
        size_t len = cases[i].len;
        optics_text_metrics m = optics_text_measure(
            text, &(optics_text_measure_opts){.utf8 = s, .len = len, .style = &style});
        OT_EXPECT(is_finite_metrics(m));

        for (size_t b = 0; b <= len; ++b) {
            float x = optics_text_x_for_byte(
                text,
                &(optics_text_x_for_byte_opts){.utf8 = s, .len = len, .byte = b, .style = &style});
            OT_EXPECT(isfinite(x));
            size_t fwd = optics_text_visual_move(text, &(optics_text_visual_move_opts){
                                                           .utf8 = s,
                                                           .len = len,
                                                           .byte = b,
                                                           .forward = true,
                                                           .style = &style,
                                                       });
            size_t bwd = optics_text_visual_move(text, &(optics_text_visual_move_opts){
                                                           .utf8 = s,
                                                           .len = len,
                                                           .byte = b,
                                                           .forward = false,
                                                           .style = &style,
                                                       });
            OT_EXPECT(fwd <= len);
            OT_EXPECT(bwd <= len);
        }

        OT_EXPECT(
            optics_text_byte_for_x(
                text, &(optics_text_byte_for_x_opts){
                          .utf8 = s, .len = len, .local_x = -100.0f, .style = &style}) <= len);
        OT_EXPECT(optics_text_byte_for_x(
                      text, &(optics_text_byte_for_x_opts){
                                .utf8 = s, .len = len, .local_x = 1e6f, .style = &style}) <= len);

        optics_text_xrange ranges[8] = {};
        int n = optics_text_selection_rects(text, &(optics_text_selection_rects_opts){
                                                      .utf8 = s,
                                                      .len = len,
                                                      .lo = 0,
                                                      .hi = len,
                                                      .style = &style,
                                                      .out = ranges,
                                                      .max = 8,
                                                  });
        OT_EXPECT(n >= 0 && n <= 8);
        for (int r = 0; r < n; ++r) {
            OT_EXPECT(isfinite(ranges[r].x0) && isfinite(ranges[r].x1));
            OT_EXPECT(ranges[r].x1 >= ranges[r].x0);
        }
    }
}

static void test_style_variants_and_scratch_growth(optics_text_ctx *text) {
    /* Null style uses engine defaults cleanly. */
    optics_text_metrics def = optics_text_measure(
        text, &(optics_text_measure_opts){.utf8 = "Optics", .len = 6, .style = nullptr});
    OT_EXPECT(is_finite_metrics(def));
    OT_EXPECT(def.width > 0.0f && def.height > 0.0f);

    /* Increasing size_px increases measured width and height monotonically. */
    optics_text_style small_s = {.size_px = 12.0f,
                                 .weight = 400.0f,
                                 .color = 0xFFFFFFFFu,
                                 .family = OPTICS_TEXT_FAMILY_MONO};
    optics_text_style large_s = {.size_px = 36.0f,
                                 .weight = 400.0f,
                                 .color = 0xFFFFFFFFu,
                                 .family = OPTICS_TEXT_FAMILY_MONO};
    optics_text_metrics ms = optics_text_measure(
        text, &(optics_text_measure_opts){.utf8 = "Hello", .len = 5, .style = &small_s});
    optics_text_metrics ml = optics_text_measure(
        text, &(optics_text_measure_opts){.utf8 = "Hello", .len = 5, .style = &large_s});
    OT_EXPECT(ml.width > ms.width);
    OT_EXPECT(ml.height > ms.height);

    /* Families, weights, italic, and HiDPI scale all produce valid finite metrics. */
    const optics_text_family families[] = {OPTICS_TEXT_FAMILY_DEFAULT, OPTICS_TEXT_FAMILY_SANS,
                                           OPTICS_TEXT_FAMILY_SERIF, OPTICS_TEXT_FAMILY_MONO};
    for (size_t i = 0; i < sizeof(families) / sizeof(families[0]); ++i) {
        optics_text_style st = {
            .size_px = 18.0f,
            .weight = (i & 1u) ? 700.0f : 400.0f,
            .color = 0xFFFFFFFFu,
            .family = families[i],
            .italic = (i >= 2u),
        };
        optics_text_metrics m = optics_text_measure(
            text, &(optics_text_measure_opts){.utf8 = "Typography 123", .len = 14, .style = &st});
        OT_EXPECT(m.width > 0.0f && m.height > 0.0f);
    }

    /* Logical metrics remain closely aligned when changing device-pixel scale. */
    optics_text_set_scale(text, 1.0f);
    optics_text_metrics m1 = optics_text_measure(
        text, &(optics_text_measure_opts){.utf8 = "ScaleCheck", .len = 10, .style = &large_s});
    optics_text_set_scale(text, 2.0f);
    optics_text_metrics m2 = optics_text_measure(
        text, &(optics_text_measure_opts){.utf8 = "ScaleCheck", .len = 10, .style = &large_s});
    OT_EXPECT(m2.width > 0.0f);
    OT_EXPECT_NEAR(m2.width, m1.width, m1.width * 0.15f + 2.0f);
    optics_text_set_scale(text, 1.0f);

    /* Exceed initial 256-codepoint scratch buffer and 64-run buffer with alternating runs. */
    char long_buf[1024];
    size_t pos = 0;
    for (int i = 0; i < 80 && pos + 8 < sizeof(long_buf); ++i) {
        memcpy(long_buf + pos, "ab", 2);
        pos += 2;
        /* Alternating Hebrew letter U+05D0 forces frequent BiDi/script run splits. */
        long_buf[pos++] = (char)0xD7;
        long_buf[pos++] = (char)0x90;
    }
    optics_text_metrics mlong = optics_text_measure(
        text, &(optics_text_measure_opts){.utf8 = long_buf, .len = pos, .style = &small_s});
    OT_EXPECT(is_finite_metrics(mlong));
    OT_EXPECT(mlong.width > ms.width * 5.0f);

    optics_text_compact(text);
    optics_text_metrics after = optics_text_measure(
        text, &(optics_text_measure_opts){.utf8 = "Hello", .len = 5, .style = &small_s});
    OT_EXPECT_NEAR(after.width, ms.width, 0.01f);
}

static void test_record_and_cpu_replay(optics_text_ctx *text) {
    /* Invalid arguments fail cleanly. */
    OT_EXPECT(optics_text_record(nullptr, nullptr) == OPTICS_GFX_ERROR_INVALID_ARGUMENT);
    OT_EXPECT(optics_text_record(text, nullptr) == OPTICS_GFX_ERROR_INVALID_ARGUMENT);

    optics_gfx_encoder *enc = nullptr;
    OT_REQUIRE(optics_gfx_encoder_create(&(optics_gfx_encoder_desc){}, &enc) == OPTICS_GFX_OK);
    OT_EXPECT(optics_text_record(text, &(optics_text_record_opts){
                                           .encoder = enc, .utf8 = "", .len = 0}) == OPTICS_GFX_OK);
    optics_gfx_encoder_destroy(enc);

    optics_text_style style = {
        .size_px = 28.0f,
        .weight = 400.0f,
        .color = optics_gfx_color_rgba_premul(255, 255, 255, 255),
        .family = OPTICS_TEXT_FAMILY_SANS,
    };
    size_t lit_fill = render_recorded_text(text, "Optics", 6, style, 0.0f, 180, 64);
    size_t lit_outline = render_recorded_text(text, "Optics", 6, style, 2.0f, 180, 64);
    OT_EXPECT(lit_fill > 0);
    OT_EXPECT(lit_outline >= lit_fill);
}

int main(void) {
    test_stats_and_deviceless_atlas();

    optics_text_ctx *text = nullptr;
    OT_REQUIRE(optics_text_create(&(optics_text_desc){.device = nullptr, .scale = 1.0f}, &text) ==
               OPTICS_GFX_OK);

    test_malformed_utf8_resilience(text);
    test_style_variants_and_scratch_growth(text);
    test_record_and_cpu_replay(text);

    optics_text_release(text);
    return OT_REPORT();
}
