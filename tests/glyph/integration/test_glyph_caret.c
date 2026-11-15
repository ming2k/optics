/*
 * glyph_caret — deterministic caret, selection and visual-movement geometry.
 *
 * The fuzz lane calls these functions with arbitrary bytes; this lane pins the
 * invariants that fuzzing cannot express:
 *
 *   - LTR caret x is non-decreasing across byte boundaries and ends at the
 *     measured width;
 *   - byte_for_x is monotonic and clamps to [0, len];
 *   - visual_move steps exactly one cluster in each direction and stops at
 *     the edges;
 *   - a multi-byte UTF-8 cluster is one caret step, not one per byte;
 *   - a pure RTL run places the logical start at the right edge and the
 *     logical end at the left edge;
 *   - mixed-direction text keeps every caret/selection span inside the
 *     measured run.
 *
 * The font stack is a real boundary. A host without a shaping backend (or
 * without any Latin face) skips explicitly instead of passing vacuously.
 */

#include "optics_test.h"

#include <glyph/glyph.h>

#include <string.h>

static const glyph_style STYLE = {
    .size_px = 32.0f,
    .weight = 400.0f,
    .color = 0xFFFFFFFFu,
    .family = GLYPH_FAMILY_SANS,
};

static glyph_metrics measure(glyph_ctx *t, const char *s, size_t len, const glyph_style *style) {
    return glyph_measure(t, &(glyph_measure_opts){.utf8 = s, .len = len, .style = style});
}

static float caret_x(glyph_ctx *t, const char *s, size_t len, size_t byte,
                     const glyph_style *style) {
    return glyph_x_for_byte(
        t, &(glyph_x_for_byte_opts){.utf8 = s, .len = len, .byte = byte, .style = style});
}

static size_t caret_byte(glyph_ctx *t, const char *s, size_t len, float x,
                         const glyph_style *style) {
    return glyph_byte_for_x(
        t, &(glyph_byte_for_x_opts){.utf8 = s, .len = len, .local_x = x, .style = style});
}

static int selection(glyph_ctx *t, const char *s, size_t len, size_t lo, size_t hi,
                     const glyph_style *style, glyph_xrange *out, int max) {
    return glyph_selection_rects(
        t, &(glyph_selection_rects_opts){
               .utf8 = s, .len = len, .lo = lo, .hi = hi, .style = style, .out = out, .max = max});
}

static size_t visual_move(glyph_ctx *t, const char *s, size_t len, size_t byte, bool forward,
                          const glyph_style *style) {
    return glyph_visual_move(t, &(glyph_visual_move_opts){
                                    .utf8 = s,
                                    .len = len,
                                    .byte = byte,
                                    .forward = forward,
                                    .style = style,
                                });
}

static bool span_inside(glyph_xrange r, float width) {
    return r.x0 >= -0.5f && r.x1 >= r.x0 && r.x1 <= width + 0.5f;
}

int main(void) {
    glyph_ctx *text = nullptr;
    OT_REQUIRE(glyph_create(&(glyph_desc){.device = nullptr, .scale = 1.0f}, &text) == FLUX_OK);
    OT_REQUIRE(text != nullptr);

    static const char LTR[] = "abc";
    glyph_metrics ltr = measure(text, LTR, strlen(LTR), &STYLE);
    OT_SKIP_IF(ltr.width <= 0.0f, "no shaping backend or Latin face");

    float x0 = caret_x(text, LTR, strlen(LTR), 0, &STYLE);
    float x1 = caret_x(text, LTR, strlen(LTR), 1, &STYLE);
    float x2 = caret_x(text, LTR, strlen(LTR), 2, &STYLE);
    float x3 = caret_x(text, LTR, strlen(LTR), 3, &STYLE);
    OT_EXPECT_NEAR(x0, 0.0f, 0.01f);
    OT_EXPECT(x1 > x0);
    OT_EXPECT(x2 > x1);
    OT_EXPECT(x3 > x2);
    OT_EXPECT_NEAR(x3, ltr.width, 0.5f);

    /* Out-of-range bytes clamp to the trailing edge; never negative. */
    OT_EXPECT(caret_x(text, LTR, strlen(LTR), 99, &STYLE) >= x3 - 0.01f);

    /* byte_for_x is monotonic and clamps at both ends. */
    size_t previous = 0;
    for (int i = 0; i <= 32; i++) {
        float x = ltr.width * (float)i / 32.0f;
        size_t at = caret_byte(text, LTR, strlen(LTR), x, &STYLE);
        OT_EXPECT(at >= previous);
        OT_EXPECT(at <= strlen(LTR));
        previous = at;
    }
    OT_EXPECT(caret_byte(text, LTR, strlen(LTR), 0.0f, &STYLE) == 0);
    OT_EXPECT(caret_byte(text, LTR, strlen(LTR), ltr.width, &STYLE) == strlen(LTR));

    /* One cluster per step, and the edges are sticky. */
    OT_EXPECT(visual_move(text, LTR, strlen(LTR), 0, true, &STYLE) == 1);
    OT_EXPECT(visual_move(text, LTR, strlen(LTR), 1, false, &STYLE) == 0);
    OT_EXPECT(visual_move(text, LTR, strlen(LTR), 2, true, &STYLE) == 3);
    OT_EXPECT(visual_move(text, LTR, strlen(LTR), 3, false, &STYLE) == 2);
    OT_EXPECT(visual_move(text, LTR, strlen(LTR), 0, false, &STYLE) == 0);
    OT_EXPECT(visual_move(text, LTR, strlen(LTR), 3, true, &STYLE) == 3);

    /* A selection is a forward span inside the run; an empty range is none. */
    glyph_xrange rects[8] = {};
    int n = selection(text, LTR, strlen(LTR), 0, 2, &STYLE, rects, 8);
    OT_EXPECT(n >= 1);
    for (int i = 0; i < n; i++)
        OT_EXPECT(span_inside(rects[i], ltr.width));
    OT_EXPECT(selection(text, LTR, strlen(LTR), 1, 1, &STYLE, rects, 8) == 0);
    OT_EXPECT(selection(text, LTR, strlen(LTR), 0, 3, &STYLE, rects, 0) == 0);

    /* A two-byte cluster is one visual step. */
    static const char ACUTE[] = "\xC3\xA9"; /* U+00E9 é */
    glyph_metrics acute = measure(text, ACUTE, strlen(ACUTE), &STYLE);
    if (acute.width > 0.0f) {
        float a0 = caret_x(text, ACUTE, strlen(ACUTE), 0, &STYLE);
        float a2 = caret_x(text, ACUTE, strlen(ACUTE), 2, &STYLE);
        OT_EXPECT(a2 > a0);
        OT_EXPECT_NEAR(a2, acute.width, 0.5f);
        OT_EXPECT(visual_move(text, ACUTE, strlen(ACUTE), 0, true, &STYLE) == 2);
        OT_EXPECT(visual_move(text, ACUTE, strlen(ACUTE), 2, false, &STYLE) == 0);
    }

    /* Pure RTL: logical start is the right edge, logical end the left edge. */
    static const char RTL[] = "\xD7\x90\xD7\x91\xD7\x92"; /* אבג */
    glyph_metrics rtl = measure(text, RTL, strlen(RTL), &STYLE);
    if (rtl.width > 0.0f) {
        float start = caret_x(text, RTL, strlen(RTL), 0, &STYLE);
        float end = caret_x(text, RTL, strlen(RTL), strlen(RTL), &STYLE);
        OT_EXPECT(start > end);
        OT_EXPECT(start <= rtl.width + 0.5f);
        OT_EXPECT(end >= -0.5f);

        n = selection(text, RTL, strlen(RTL), 0, strlen(RTL), &STYLE, rects, 8);
        OT_EXPECT(n >= 1);
        for (int i = 0; i < n; i++)
            OT_EXPECT(span_inside(rects[i], rtl.width));

        size_t moved = visual_move(text, RTL, strlen(RTL), 0, true, &STYLE);
        OT_EXPECT(moved <= strlen(RTL));
        moved = visual_move(text, RTL, strlen(RTL), strlen(RTL), false, &STYLE);
        OT_EXPECT(moved <= strlen(RTL));
    }

    /* Mixed LTR/RTL: every reported span stays inside the measured run and
     * movement never escapes the byte range. */
    static const char MIXED[] = "abc\xD7\x90\xD7\x91\xD7\x92";
    glyph_metrics mixed = measure(text, MIXED, strlen(MIXED), &STYLE);
    if (mixed.width > 0.0f) {
        for (size_t b = 0; b <= strlen(MIXED); b++) {
            float x = caret_x(text, MIXED, strlen(MIXED), b, &STYLE);
            OT_EXPECT(x >= -0.5f && x <= mixed.width + 0.5f);
        }
        n = selection(text, MIXED, strlen(MIXED), 0, strlen(MIXED), &STYLE, rects, 8);
        OT_EXPECT(n >= 1);
        for (int i = 0; i < n; i++)
            OT_EXPECT(span_inside(rects[i], mixed.width));

        size_t start = 0;
        for (int step = 0; step < 32; step++) {
            size_t next = visual_move(text, MIXED, strlen(MIXED), start, true, &STYLE);
            OT_EXPECT(next <= strlen(MIXED));
            if (next == start)
                break;
            start = next;
        }
        OT_EXPECT(start <= strlen(MIXED));
    }

    glyph_compact(text);
    glyph_metrics after = measure(text, LTR, strlen(LTR), &STYLE);
    OT_EXPECT_NEAR(after.width, ltr.width, 0.001f);

    glyph_release(text);
    return OT_REPORT();
}
