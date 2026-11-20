/* test_icon_runtime.c — runtime SVG icon registration
 * (lens_icon_register_svg). CPU-only.
 */

#include "../../libs/ui/src/internal.h"
#include "lens_test.h"
#include "optics_test.h"
#include <optics/ui.h>

static const optics_ui_input IN0 = {.display_size = {200, 100}, .dt_seconds = 0.016f};

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

static int has_icon_cmd(const optics_ui_node *n, int32_t id) {
    if (!n)
        return 0;
    for (uint32_t i = 0; i < n->cmd_count; i++)
        if (n->cmds[i].kind == OPTICS_UI_DRAW_ICON && n->cmds[i].icon_id == id)
            return 1;
    return 0;
}

int main(void) {
    /* Registration: ids continue where the built-in enum ends. */
    optics_ui_icon_id stroke = optics_ui_icon_register_svg(SVG_STROKE);
    OT_EXPECT((int)stroke >= (int)OPTICS_UI_ICON_COUNT);
    optics_ui_icon_id fill = optics_ui_icon_register_svg(SVG_FILL);
    OT_EXPECT((int)fill == (int)stroke + 1);

    /* Malformed or empty input is rejected, never crashes. */
    OT_EXPECT((int)optics_ui_icon_register_svg(nullptr) == (int)OPTICS_UI_ICON_INVALID);
    OT_EXPECT((int)optics_ui_icon_register_svg("") == (int)OPTICS_UI_ICON_INVALID);
    OT_EXPECT((int)optics_ui_icon_register_svg("this is not svg <<<") ==
              (int)OPTICS_UI_ICON_INVALID);
    OT_EXPECT((int)optics_ui_icon_register_svg("<html><body/></html>") ==
              (int)OPTICS_UI_ICON_INVALID);
    OT_EXPECT((int)optics_ui_icon_register_svg("<svg viewBox=\"0 0 24 24\"></svg>") ==
              (int)OPTICS_UI_ICON_INVALID);
    OT_EXPECT((int)optics_ui_icon_register_svg("<svg><circle cx=\"1\" cy=\"1\" r=\"1\"/></svg>") >=
              (int)OPTICS_UI_ICON_COUNT);

    optics_ui *ui = nullptr;
    OT_EXPECT(optics_ui_create(&(optics_ui_desc){}, &ui) == OPTICS_GFX_OK);

    /* Runtime ids work through every icon widget, alongside a built-in. */
    optics_ui_begin(ui, &IN0);
    optics_ui_icon(ui, &(optics_ui_icon_opts){.id = stroke, .size = 16.0f});
    optics_ui_icon(ui, &(optics_ui_icon_opts){.id = OPTICS_UI_ICON_SEARCH, .size = 16.0f});
    optics_ui_button(ui, &(optics_ui_button_opts){.icon = fill});
    optics_ui_selectable(
        ui, &(optics_ui_selectable_opts){.icon = stroke, .label = "row", .selected = false});
    optics_ui_test_end(ui);
    OT_EXPECT(!optics_ui_overflowed(ui));

    optics_ui_node *child = optics_ui_node_first_child(optics_ui_root(ui));
    OT_EXPECT(has_icon_cmd(child, (int32_t)stroke));
    child = optics_ui_node_next_sibling(child);
    OT_EXPECT(has_icon_cmd(child, (int32_t)OPTICS_UI_ICON_SEARCH));
    child = optics_ui_node_next_sibling(child);
    OT_EXPECT(has_icon_cmd(child, (int32_t)fill));
    child = optics_ui_node_next_sibling(child);
    OT_EXPECT(has_icon_cmd(child, (int32_t)stroke));

    /* The no-icon sentinel must stay icon-less even though LENS_ICON_COUNT
     * is now a valid runtime id (the first registration took it). */
    optics_ui_begin(ui, &IN0);
    optics_ui_selectable(ui, &(optics_ui_selectable_opts){.label = "plain", .selected = false});
    optics_ui_test_end(ui);
    optics_ui_node *row = optics_ui_node_first_child(optics_ui_root(ui));
    OT_EXPECT(row && row->cmd_count == 1 && row->cmds[0].kind == OPTICS_UI_DRAW_TEXT);

    /* Unknown and invalid ids draw nothing and do not crash. */
    optics_ui_begin(ui, &IN0);
    optics_ui_icon(ui, &(optics_ui_icon_opts){.id = OPTICS_UI_ICON_INVALID, .size = 16.0f});
    optics_ui_icon(ui,
                   &(optics_ui_icon_opts){.id = (optics_ui_icon_id)(fill + 1000), .size = 16.0f});
    optics_ui_test_end(ui);
    OT_EXPECT(!optics_ui_overflowed(ui));
    OT_EXPECT(optics_ui_node_first_child(optics_ui_root(ui)) == nullptr);

    /* Flood the per-frame arena */
    optics_ui_begin(ui, &IN0);
    for (int i = 0; i < 8000; i++)
        optics_ui_icon(ui, &(optics_ui_icon_opts){.id = stroke, .size = 16.0f});
    optics_ui_test_end(ui);
    OT_EXPECT(optics_ui_overflowed(ui));

    optics_ui_release(ui);
    return OT_REPORT();
}
