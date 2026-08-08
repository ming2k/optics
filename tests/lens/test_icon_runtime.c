/* test_icon_runtime.c — runtime SVG icon registration
 * (lens_icon_register_svg). CPU-only.
 *
 * Runtime ids must flow through the same widgets as built-ins, so a
 * headless frame asserts the LENS_DRAW_ICON command lands on the retained
 * node with the runtime id attached. internal.h is included directly (same
 * pattern as test_drawlist_hash) to read the node's draw list; only struct
 * fields are read, no hidden symbols are called. The closing loop floods
 * the per-frame arena to prove the overflow flag (and its debug warning)
 * still trip with runtime icons in play. */

#include "../../libs/lens/src/internal.h"
#include "test_helpers.h"
#include <lens/lens.h>

static const lens_input IN0 = {.display_size = {200, 100}, .dt_seconds = 0.016f};

/* Feather-style: circle + line, stroke only, in a 24x24 viewBox. */
static const char SVG_STROKE[] =
    "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 24 24\" fill=\"none\""
    " stroke=\"currentColor\" stroke-width=\"2\" stroke-linecap=\"round\">"
    "<circle cx=\"12\" cy=\"12\" r=\"9\"/>"
    "<line x1=\"12\" y1=\"7\" x2=\"12\" y2=\"13\"/>"
    "</svg>";

/* Material-style: one filled rect (default black fill). */
static const char SVG_FILL[] =
    "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 24 24\">"
    "<rect x=\"4\" y=\"4\" width=\"16\" height=\"16\"/>"
    "</svg>";

static int has_icon_cmd(const lens_node *n, int32_t id) {
    if (!n)
        return 0;
    for (uint32_t i = 0; i < n->cmd_count; i++)
        if (n->cmds[i].kind == LENS_DRAW_ICON && n->cmds[i].icon_id == id)
            return 1;
    return 0;
}

int main(void) {
    /* Registration: ids continue where the built-in enum ends. */
    lens_icon_id stroke = lens_icon_register_svg(SVG_STROKE);
    CHECK((int)stroke >= (int)LENS_ICON_COUNT);
    lens_icon_id fill = lens_icon_register_svg(SVG_FILL);
    CHECK((int)fill == (int)stroke + 1);

    /* Malformed or empty input is rejected, never crashes. */
    CHECK((int)lens_icon_register_svg(NULL) == (int)LENS_ICON_INVALID);
    CHECK((int)lens_icon_register_svg("") == (int)LENS_ICON_INVALID);
    CHECK((int)lens_icon_register_svg("this is not svg <<<") == (int)LENS_ICON_INVALID);
    CHECK((int)lens_icon_register_svg("<html><body/></html>") == (int)LENS_ICON_INVALID);
    CHECK((int)lens_icon_register_svg("<svg viewBox=\"0 0 24 24\"></svg>") ==
          (int)LENS_ICON_INVALID);
    /* No viewBox/size: nanosvg falls back to the content bounds as the
     * coordinate frame, so this still registers. */
    CHECK((int)lens_icon_register_svg("<svg><circle cx=\"1\" cy=\"1\" r=\"1\"/></svg>") >=
          (int)LENS_ICON_COUNT);

    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);

    /* Runtime ids work through every icon widget, alongside a built-in. */
    lens_begin(ui, &IN0);
    lens_icon(ui, stroke, 16.0f);
    lens_icon(ui, LENS_ICON_SEARCH, 16.0f);
    lens_icon_button(ui, fill);
    lens_selectable_icon(ui, stroke, "row", false);
    lens_end(ui);
    CHECK(!lens_overflowed(ui));

    lens_node *child = lens_node_first_child(lens_root(ui));
    CHECK(has_icon_cmd(child, (int32_t)stroke));
    child = lens_node_next_sibling(child);
    CHECK(has_icon_cmd(child, (int32_t)LENS_ICON_SEARCH));
    child = lens_node_next_sibling(child);
    CHECK(has_icon_cmd(child, (int32_t)fill));
    child = lens_node_next_sibling(child);
    CHECK(has_icon_cmd(child, (int32_t)stroke));

    /* The no-icon sentinel must stay icon-less even though LENS_ICON_COUNT
     * is now a valid runtime id (the first registration took it). */
    lens_begin(ui, &IN0);
    lens_selectable(ui, "plain", false);
    lens_end(ui);
    lens_node *row = lens_node_first_child(lens_root(ui));
    CHECK(row && row->cmd_count == 1 && row->cmds[0].kind == LENS_DRAW_TEXT);

    /* Unknown and invalid ids draw nothing and do not crash. */
    lens_begin(ui, &IN0);
    lens_icon(ui, LENS_ICON_INVALID, 16.0f);
    lens_icon(ui, (lens_icon_id)(fill + 1000), 16.0f);
    lens_icon_button(ui, LENS_ICON_INVALID);
    lens_end(ui);
    CHECK(!lens_overflowed(ui));
    CHECK(lens_node_first_child(lens_root(ui)) == NULL);

    /* Flood the per-frame arena: the overflow flag (and, in debug builds,
     * the one-shot stderr warning) must trip. */
    lens_begin(ui, &IN0);
    for (int i = 0; i < 8000; i++)
        lens_icon(ui, stroke, 16.0f);
    lens_end(ui);
    CHECK(lens_overflowed(ui));

    lens_destroy(ui);
    return TEST_REPORT();
}
