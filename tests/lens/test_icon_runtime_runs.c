/* test_icon_runtime_runs.c — per-shape colour runs recorded by
 * lens_icon_register_svg (lens_icon_run). CPU-only; links icon_runtime.c
 * (+ icon_data.c, icon_svg.c) directly, same as test_icon_runtime_content.
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
#include "test_helpers.h"
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

int main(void) {
    /* Explicit colour -> exactly one run, colour preserved, stroke mode. */
    lens_icon_id red = lens_icon_register_svg(SVG_RED);
    CHECK((int)red >= (int)LENS_ICON_COUNT);
    {
        const lens_icon_desc *d = lensi_icon_desc((int32_t)red);
        CHECK(d != NULL);
        CHECK(d->runs != NULL);
        CHECK(d->run_count == 1);
        CHECK(d->runs[0].color == 0xFFFF0000u); /* R=FF A=FF */
        CHECK(d->runs[0].fill == 0);
        CHECK(d->runs[0].first_cmd == 0);
        CHECK(d->runs[0].count == d->count);
    }

    /* currentColor everywhere -> collapsed to the single-paint form. */
    lens_icon_id theme = lens_icon_register_svg(SVG_THEME);
    CHECK((int)theme >= (int)LENS_ICON_COUNT);
    {
        const lens_icon_desc *d = lensi_icon_desc((int32_t)theme);
        CHECK(d != NULL);
        CHECK(d->runs == NULL);
        CHECK(d->run_count == 0);
        /* Two shapes still produce two subpaths (two MOVE verbs). */
        uint32_t moves = 0;
        for (uint32_t i = 0; i < d->count; i++)
            if (d->cmds[i].type == 0)
                moves++;
        CHECK(moves == 2);
    }

    /* Two same-colour filled shapes merge into one run. */
    lens_icon_id two = lens_icon_register_svg(SVG_TWO_RED);
    CHECK((int)two >= (int)LENS_ICON_COUNT);
    {
        const lens_icon_desc *d = lensi_icon_desc((int32_t)two);
        CHECK(d != NULL);
        CHECK(d->runs != NULL);
        CHECK(d->run_count == 1);
        CHECK(d->runs[0].color == 0xFF00FF00u); /* G=FF A=FF */
        CHECK(d->runs[0].fill == 1);
        CHECK(d->runs[0].count == d->count);
    }

    /* Mixed paints: three runs, order preserved, alpha carried through,
     * currentColor run recorded as theme (color == 0). */
    lens_icon_id mixed = lens_icon_register_svg(SVG_MIXED);
    CHECK((int)mixed >= (int)LENS_ICON_COUNT);
    {
        const lens_icon_desc *d = lensi_icon_desc((int32_t)mixed);
        CHECK(d != NULL);
        CHECK(d->runs != NULL);
        CHECK(d->run_count == 3);
        CHECK(d->runs[0].color == 0xFF112233u); /* R=11 G=22 B=33 A=FF */
        CHECK(d->runs[0].fill == 1);
        CHECK(d->runs[1].color == 0u); /* currentColor -> theme */
        CHECK(d->runs[1].fill == 0);   /* stroke run */
        /* fill-opacity 0.5 -> alpha 127 or 128 depending on nanosvg's
         * rounding; accept either (the straight-alpha channel is what
         * matters, not the exact rounding of the opacity). */
        uint32_t a = d->runs[2].color >> 24;
        CHECK(a >= 127 && a <= 128);
        CHECK((d->runs[2].color & 0xFFFFFFu) == 0x445566u);
        CHECK(d->runs[2].fill == 1);
        /* Runs bracket the whole stream with no gaps. */
        CHECK(d->runs[0].first_cmd == 0);
        CHECK(d->runs[0].first_cmd + d->runs[0].count == d->runs[1].first_cmd);
        CHECK(d->runs[1].first_cmd + d->runs[1].count == d->runs[2].first_cmd);
        CHECK(d->runs[2].first_cmd + d->runs[2].count == d->count);
    }

    return TEST_REPORT();
}
