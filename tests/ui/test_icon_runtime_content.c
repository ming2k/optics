/* test_icon_runtime_content.c — content of the streams produced by
 * optics_ui_icon_register_svg. CPU-only.
 *
 * Widget-level coverage lives in test_icon_runtime.c; this binary compiles
 * icon_runtime.c (+ icon_data.c) directly — the same pattern as
 * test_drawlist_hash — because the registry accessors (optics_ui__icon_desc and
 * friends) are hidden symbols that do not link from liboptics_ui.so. The point
 * is to assert WHAT got registered, not just that an id came back:
 *
 *  - every shape survives flattening (a circle + line icon keeps two
 *    subpaths; a gear keeps hub AND body);
 *  - zero-length cubics from nanosvg's element conversions are dropped —
 *    one of those once flooded the flattener's scratch buffer with 2^16
 *    duplicate points, truncating every following shape (a settings gear
 *    rendered as just its hub circle);
 *  - coordinates normalize into the shared 24x24 icon box;
 *  - stroke vs fill mode follows the shapes' paint.
 */

#include "../../libs/ui/src/internal.h"
#include "optics_test.h"
#include "ui_test.h"
#include <optics/ui.h>

/* Feather-style: circle + line, stroke only, in a 24x24 viewBox. */
static const char SVG_STROKE[] =
    "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 24 24\" fill=\"none\""
    " stroke=\"currentColor\" stroke-width=\"2\" stroke-linecap=\"round\">"
    "<circle cx=\"12\" cy=\"12\" r=\"9\"/>"
    "<line x1=\"12\" y1=\"7\" x2=\"12\" y2=\"13\"/>"
    "</svg>";

/* Material-style: one filled rect (default black fill). */
static const char SVG_FILL[] = "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 24 24\">"
                               "<rect x=\"4\" y=\"4\" width=\"16\" height=\"16\"/>"
                               "</svg>";

/* A 48x48 viewBox: coordinates must halve into the 24 box. */
static const char SVG_WIDE_VIEWBOX[] =
    "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 48 48\" fill=\"none\""
    " stroke=\"currentColor\"><circle cx=\"24\" cy=\"24\" r=\"18\"/></svg>";

int main(void) {
    optics_ui_icon_id stroke = optics_ui_icon_register_svg(SVG_STROKE);
    OT_EXPECT((int)stroke >= (int)OPTICS_UI_ICON_COUNT);
    optics_ui_icon_id fill = optics_ui_icon_register_svg(SVG_FILL);
    OT_EXPECT((int)fill == (int)stroke + 1);

    const optics_ui_icon_desc *desc = optics_ui__icon_desc((int32_t)stroke);
    OT_EXPECT(desc != nullptr);

    /* Both shapes survive: exactly two MOVE verbs (circle subpath + line
     * subpath), 4 cubic arcs for the circle, 1 line verb for the line, and no degenerate cubics. */
    uint32_t moves = 0, lines = 0, degenerate = 0, cubics = 0;
    float px = 0.0f, py = 0.0f;
    float min_x = 1e30f, min_y = 1e30f, max_x = -1e30f, max_y = -1e30f;
#define TRACK(x, y)                                                                                \
    do {                                                                                           \
        if ((x) < min_x)                                                                           \
            min_x = (x);                                                                           \
        if ((x) > max_x)                                                                           \
            max_x = (x);                                                                           \
        if ((y) < min_y)                                                                           \
            min_y = (y);                                                                           \
        if ((y) > max_y)                                                                           \
            max_y = (y);                                                                           \
    } while (0)
    for (uint32_t i = 0; i < desc->count; i++) {
        const optics_ui_icon_cmd *cmd = &desc->cmds[i];
        if (cmd->type == 0) {
            moves++;
            px = cmd->params[0];
            py = cmd->params[1];
            TRACK(px, py);
        } else if (cmd->type == 1) {
            lines++;
            TRACK(cmd->params[0], cmd->params[1]);
            px = cmd->params[0];
            py = cmd->params[1];
        } else if (cmd->type == 2) {
            cubics++;
            float d1 = (cmd->params[0] - px) * (cmd->params[0] - px) +
                       (cmd->params[1] - py) * (cmd->params[1] - py);
            float d2 = (cmd->params[2] - px) * (cmd->params[2] - px) +
                       (cmd->params[3] - py) * (cmd->params[3] - py);
            float d3 = (cmd->params[4] - px) * (cmd->params[4] - px) +
                       (cmd->params[5] - py) * (cmd->params[5] - py);
            float farthest = d1 > d2 ? d1 : d2;
            farthest = farthest > d3 ? farthest : d3;
            if (farthest < 1e-8f)
                degenerate++;
            TRACK(cmd->params[0], cmd->params[1]);
            TRACK(cmd->params[2], cmd->params[3]);
            TRACK(cmd->params[4], cmd->params[5]);
            px = cmd->params[4];
            py = cmd->params[5];
        }
    }
#undef TRACK
    OT_EXPECT(moves == 2);
    OT_EXPECT(degenerate == 0);
    /* circle = 4 cubic arcs, line = 1 line verb */
    OT_EXPECT(cubics == 4);
    OT_EXPECT(lines == 1);
    /* 24-box normalization: r=9 circle centred at 12 spans 3..21. */
    OT_EXPECT(min_x > 2.9f && min_x < 3.1f);
    OT_EXPECT(max_x > 20.9f && max_x < 21.1f);
    OT_EXPECT(min_y > 2.9f && max_y < 21.1f);

    /* Render mode follows the shapes' paint. */
    OT_EXPECT(optics_ui__icon_mode((int32_t)stroke) == OPTICS_UI_ICON_RENDER_STROKE);
    OT_EXPECT(optics_ui__icon_mode((int32_t)fill) == OPTICS_UI_ICON_RENDER_FILL);

    /* A 48-wide viewBox scales down into the 24 box (r=18 -> 9). */
    optics_ui_icon_id wide = optics_ui_icon_register_svg(SVG_WIDE_VIEWBOX);
    const optics_ui_icon_desc *wd = optics_ui__icon_desc((int32_t)wide);
    OT_EXPECT(wd != nullptr && wd->count > 0);
    OT_EXPECT(wd->cmds[0].type == 0);
    OT_EXPECT(wd->cmds[0].params[0] > 20.9f && wd->cmds[0].params[0] < 21.1f);
    OT_EXPECT(wd->cmds[0].params[1] > 11.9f && wd->cmds[0].params[1] < 12.1f);

    return OT_REPORT();
}
