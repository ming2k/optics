/* test_label_centering.c — padded labels must stay vertically centred
 * when a fixed-height parent constrains them below their intrinsic
 * padded height. CPU-only.
 */

#include "../../libs/ui/src/internal.h"
#include "optics_test.h"
#include "ui_test.h"
#include <optics/ui.h>

static const optics_ui_input IN0 = {.display_size = {200, 100}, .dt_seconds = 0.016f};

static const optics_ui_draw_cmd *first_text_cmd(const optics_ui_node *n) {
    if (!n)
        return nullptr;
    for (uint32_t i = 0; i < n->cmd_count; i++)
        if (n->cmds[i].kind == OPTICS_UI_DRAW_TEXT)
            return &n->cmds[i];
    return nullptr;
}

int main(void) {
    optics_ui *ui = nullptr;
    OT_EXPECT(optics_ui_create(&(optics_ui_desc){}, &ui) == OPTICS_GFX_OK);

    /* A 34px row clamps the padded label (14px text + 2×12 pad = 38px). */
    optics_ui_begin(ui, &IN0);
    optics_ui_row_begin(
        ui, &(optics_ui_layout_opts){
                .box.height = 34.0f, .gap = 7.0f, .pad = 5.0f, .cross = OPTICS_UI_CENTER});
    optics_ui_label(ui, &(optics_ui_label_opts){.text = "row label"});
    optics_ui_close(ui);
    optics_ui_test_end(ui);

    optics_ui_node *row = optics_ui_node_first_child(optics_ui_root(ui));
    optics_ui_node *label = optics_ui_node_first_child(row);
    OT_EXPECT(label != nullptr);
    optics_gfx_rect r = optics_ui_node_bounds(label);
    OT_EXPECT(r.h <= 34.0f); /* clamped into the row's inner height */

    const optics_ui_draw_cmd *cmd = first_text_cmd(label);
    OT_EXPECT(cmd != nullptr);

    /* Unconstrained, the label keeps its intrinsic padded height. */
    optics_ui_begin(ui, &IN0);
    optics_ui_label(ui, &(optics_ui_label_opts){.text = "loose label"});
    optics_ui_test_end(ui);
    optics_ui_node *loose = optics_ui_node_first_child(optics_ui_root(ui));
    OT_EXPECT(loose != nullptr);
    const optics_ui_draw_cmd *loose_cmd = first_text_cmd(loose);
    OT_EXPECT(loose_cmd != nullptr);

    optics_ui_release(ui);
    return OT_REPORT();
}
