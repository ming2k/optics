/* test_icon_runtime_runs.c — per-shape colour runs recorded by
 * lens_icon_register_svg (lens_icon_run). CPU-only; links icon_runtime.c
 * (+ icon_data.c) directly, same as test_icon_runtime_content.
 *
 * Asserts WHAT was registered:
 *  - a stroke="#FF0000" shape records a stroke run with color 0xFF0000FF;
 *  - shapes painted currentColor record color == 0 (theme colour) and an
 *    icon whose every shape is currentColor collapses to runs == NULL —
 *    byte-identical replay to a built-in;
 *  - consecutive shapes with identical (color, fill) merge into one run;
 *  - mixed icons keep shape order and bracket the whole cmd stream.
 */

#include "../../libs/lens/src/internal.h"
#include "lens_test.h"
#include "optics_test.h"
#include <lens/lens.h>

/* Explicit red stroke, one circle. */
static const char SVG_RED[] =
    "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 24 24\" fill=\"none\""
    " stroke=\"#FF0000\">"
    "<circle cx=\"12\" cy=\"12\" r=\"9\"/></svg>";

/* currentColor only: must collapse to runs == NULL. */
static const char SVG_THEME[] =
    "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 24 24\" fill=\"none\""
    " stroke=\"currentColor\">"
    "<circle cx=\"12\" cy=\"12\" r=\"9\"/>"
    "<line x1=\"12\" y1=\"7\" x2=\"12\" y2=\"13\"/></svg>";

/* Two red-filled rects, adjacent in document order: one merged run. */
static const char SVG_TWO_RED[] =
    "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 24 24\" fill=\"#00FF00\">"
    "<rect x=\"2\" y=\"2\" width=\"8\" height=\"8\"/>"
    "<rect x=\"14\" y=\"14\" width=\"8\" height=\"8\"/></svg>";

/* Mixed: red filled rect + currentColor stroked circle + blue filled rect. */
static const char SVG_MIXED[] =
    "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 24 24\">"
    "<rect x=\"2\" y=\"2\" width=\"8\" height=\"8\" fill=\"#112233\"/>"
    "<circle cx=\"18\" cy=\"6\" r=\"4\" fill=\"none\" stroke=\"currentColor\"/>"
    "<rect x=\"10\" y=\"14\" width=\"8\" height=\"8\" fill=\"#445566\" fill-opacity=\"0.5\"/>"
    "</svg>";

/* Mixed theme: currentColor stroke circle + currentColor filled circle.
 * Even though all colors are theme (0), runs MUST NOT collapse because
 * stroke and fill modes are mixed. */
static const char SVG_THEME_MIXED[] =
    "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 24 24\">"
    "<circle cx=\"12\" cy=\"12\" r=\"9\" fill=\"none\" stroke=\"currentColor\"/>"
    "<circle cx=\"12\" cy=\"12\" r=\"3\" fill=\"currentColor\"/>"
    "</svg>";

int main(void) {
    /* Explicit colour -> exactly one run, colour preserved, stroke mode. */
    lens_icon_id red = lens_icon_register_svg(SVG_RED);
    OT_EXPECT((int)red >= (int)LENS_ICON_COUNT);
    {
        const lens_icon_desc *d = lensi_icon_desc((int32_t)red);
        OT_EXPECT(d != NULL);
        OT_EXPECT(d->runs != NULL);
        OT_EXPECT(d->run_count == 1);
        OT_EXPECT(d->runs[0].color == 0xFFFF0000u); /* R=FF A=FF */
        OT_EXPECT(d->runs[0].fill == 0);
        OT_EXPECT(d->runs[0].first_cmd == 0);
        OT_EXPECT(d->runs[0].count == d->count);
    }

    /* currentColor everywhere -> collapsed to the single-paint form. */
    lens_icon_id theme = lens_icon_register_svg(SVG_THEME);
    OT_EXPECT((int)theme >= (int)LENS_ICON_COUNT);
    {
        const lens_icon_desc *d = lensi_icon_desc((int32_t)theme);
        OT_EXPECT(d != NULL);
        OT_EXPECT(d->runs == NULL);
        OT_EXPECT(d->run_count == 0);
        /* Two shapes still produce two subpaths (two MOVE verbs). */
        uint32_t moves = 0;
        for (uint32_t i = 0; i < d->count; i++)
            if (d->cmds[i].type == 0)
                moves++;
        OT_EXPECT(moves == 2);
    }

    /* Two same-colour filled shapes merge into one run. */
    lens_icon_id two = lens_icon_register_svg(SVG_TWO_RED);
    OT_EXPECT((int)two >= (int)LENS_ICON_COUNT);
    {
        const lens_icon_desc *d = lensi_icon_desc((int32_t)two);
        OT_EXPECT(d != NULL);
        OT_EXPECT(d->runs != NULL);
        OT_EXPECT(d->run_count == 1);
        OT_EXPECT(d->runs[0].color == 0xFF00FF00u); /* G=FF A=FF */
        OT_EXPECT(d->runs[0].fill == 1);
        OT_EXPECT(d->runs[0].count == d->count);
    }

    /* Mixed paints: three runs, order preserved, alpha carried through,
     * currentColor run recorded as theme (color == 0). */
    lens_icon_id mixed = lens_icon_register_svg(SVG_MIXED);
    OT_EXPECT((int)mixed >= (int)LENS_ICON_COUNT);
    {
        const lens_icon_desc *d = lensi_icon_desc((int32_t)mixed);
        OT_EXPECT(d != NULL);
        OT_EXPECT(d->runs != NULL);
        OT_EXPECT(d->run_count == 3);
        OT_EXPECT(d->runs[0].color == 0xFF112233u); /* R=11 G=22 B=33 A=FF */
        OT_EXPECT(d->runs[0].fill == 1);
        OT_EXPECT(d->runs[1].color == 0u); /* currentColor -> theme */
        OT_EXPECT(d->runs[1].fill == 0);   /* stroke run */
        /* fill-opacity 0.5 -> alpha 127 or 128 depending on nanosvg's
         * rounding; accept either (the straight-alpha channel is what
         * matters, not the exact rounding of the opacity). */
        uint32_t a = d->runs[2].color >> 24;
        OT_EXPECT(a >= 127 && a <= 128);
        OT_EXPECT((d->runs[2].color & 0xFFFFFFu) == 0x445566u);
        OT_EXPECT(d->runs[2].fill == 1);
        /* Runs bracket the whole stream with no gaps. */
        OT_EXPECT(d->runs[0].first_cmd == 0);
        OT_EXPECT(d->runs[0].first_cmd + d->runs[0].count == d->runs[1].first_cmd);
        OT_EXPECT(d->runs[1].first_cmd + d->runs[1].count == d->runs[2].first_cmd);
        OT_EXPECT(d->runs[2].first_cmd + d->runs[2].count == d->count);
    }

    /* Mixed theme stroke and fill: must preserve runs even though all colors are theme (0). */
    lens_icon_id theme_mixed = lens_icon_register_svg(SVG_THEME_MIXED);
    OT_EXPECT((int)theme_mixed >= (int)LENS_ICON_COUNT);
    {
        const lens_icon_desc *d = lensi_icon_desc((int32_t)theme_mixed);
        OT_EXPECT(d != NULL);
        OT_EXPECT(d->runs != NULL);
        OT_EXPECT(d->run_count == 2);
        OT_EXPECT(d->runs[0].color == 0u);
        OT_EXPECT(d->runs[0].fill == 0); /* stroke */
        OT_EXPECT(d->runs[1].color == 0u);
        OT_EXPECT(d->runs[1].fill == 1); /* fill */
        OT_EXPECT(d->runs[0].first_cmd == 0);
        OT_EXPECT(d->runs[0].first_cmd + d->runs[0].count == d->runs[1].first_cmd);
        OT_EXPECT(d->runs[1].first_cmd + d->runs[1].count == d->count);
    }

    return OT_REPORT();
}
