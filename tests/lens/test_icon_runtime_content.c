/* test_icon_runtime_content.c — content of the streams produced by
 * lens_icon_register_svg. CPU-only.
 *
 * Widget-level coverage lives in test_icon_runtime.c; this binary compiles
 * icon_runtime.c (+ icon_data.c, nanosvg.c) directly — the same pattern as
 * test_drawlist_hash — because the registry accessors (lensi_icon_desc and
 * friends) are hidden symbols that do not link from liblens.so. The point
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

#include "../../libs/lens/src/internal.h"
#include "test_helpers.h"
#include <lens/lens.h>

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
    lens_icon_id stroke = lens_icon_register_svg(SVG_STROKE);
    CHECK((int)stroke >= (int)LENS_ICON_COUNT);
    lens_icon_id fill = lens_icon_register_svg(SVG_FILL);
    CHECK((int)fill == (int)stroke + 1);

    const lens_icon_desc *desc = lensi_icon_desc((int32_t)stroke);
    CHECK(desc != NULL);

    /* Both shapes survive: exactly two MOVE verbs (circle subpath + line
     * subpath), and no all-coincident cubic anywhere. */
    uint32_t moves = 0, degenerate = 0, cubics = 0;
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
        const lens_icon_cmd *cmd = &desc->cmds[i];
        if (cmd->type == 0) {
            moves++;
            px = cmd->params[0];
            py = cmd->params[1];
            TRACK(px, py);
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
    CHECK(moves == 2);
    CHECK(degenerate == 0);
    /* circle = 4 arcs (+ skipped trailing duplicate), line = 1 cubic */
    CHECK(cubics >= 5 && cubics <= 6);
    /* 24-box normalization: r=9 circle centred at 12 spans 3..21. */
    CHECK(min_x > 2.9f && min_x < 3.1f);
    CHECK(max_x > 20.9f && max_x < 21.1f);
    CHECK(min_y > 2.9f && max_y < 21.1f);

    /* Render mode follows the shapes' paint. */
    CHECK(lensi_icon_mode((int32_t)stroke) == LENSI_ICON_RENDER_STROKE);
    CHECK(lensi_icon_mode((int32_t)fill) == LENSI_ICON_RENDER_FILL);

    /* A 48-wide viewBox scales down into the 24 box (r=18 -> 9). */
    lens_icon_id wide = lens_icon_register_svg(SVG_WIDE_VIEWBOX);
    const lens_icon_desc *wd = lensi_icon_desc((int32_t)wide);
    CHECK(wd != NULL && wd->count > 0);
    CHECK(wd->cmds[0].type == 0);
    CHECK(wd->cmds[0].params[0] > 20.9f && wd->cmds[0].params[0] < 21.1f);
    CHECK(wd->cmds[0].params[1] > 11.9f && wd->cmds[0].params[1] < 12.1f);

    return TEST_REPORT();
}
