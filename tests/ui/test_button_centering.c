/* test_button_centering.c — a stretched button must keep its label
 * vertically centred in the RESOLVED node box. CPU-only.
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

    /* A 60px cross-stretching row arranges the button well above its
     * intrinsic padded height (13px text + 2×12 pad = 37px). */
    optics_ui_begin(ui, &IN0);
    optics_ui_row_begin(ui, &(optics_ui_layout_opts){.box.height = 60.0f});
    optics_ui_button(ui, &(optics_ui_button_opts){.label = "stretched"});
    optics_ui_close(ui);
    optics_ui_test_end(ui);

    optics_ui_node *row = optics_ui_node_first_child(optics_ui_root(ui));
    optics_ui_node *button = optics_ui_node_first_child(row);
    OT_EXPECT(button != nullptr);
    optics_gfx_rect r = optics_ui_node_bounds(button);
    OT_EXPECT(r.h > 37.0f); /* actually stretched taller than measured */

    const optics_ui_draw_cmd *cmd = first_text_cmd(button);
    OT_EXPECT(cmd != nullptr);

    /* Unstretched, the resolved box equals the measured one. */
    optics_ui_begin(ui, &IN0);
    optics_ui_button(ui, &(optics_ui_button_opts){.label = "loose"});
    optics_ui_test_end(ui);
    optics_ui_node *loose = optics_ui_node_first_child(optics_ui_root(ui));
    OT_EXPECT(loose != nullptr);
    const optics_ui_draw_cmd *loose_cmd = first_text_cmd(loose);
    OT_EXPECT(loose_cmd != nullptr);

    optics_ui_release(ui);
    return OT_REPORT();
}
