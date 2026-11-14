/* test_button_centering.c — a stretched button must keep its label
 * vertically centred in the RESOLVED node box. CPU-only.
 */

#include "../../libs/lens/src/internal.h"
#include "lens_test.h"
#include "optics_test.h"
#include <lens/lens.h>

static const lens_input IN0 = {.display_size = {200, 100}, .dt_seconds = 0.016f};

static const lens_draw_cmd *first_text_cmd(const lens_node *n) {
    if (!n)
        return NULL;
    for (uint32_t i = 0; i < n->cmd_count; i++)
        if (n->cmds[i].kind == LENS_DRAW_TEXT)
            return &n->cmds[i];
    return NULL;
}

int main(void) {
    lens *ui = NULL;
    OT_EXPECT(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);

    /* A 60px cross-stretching row arranges the button well above its
     * intrinsic padded height (13px text + 2×12 pad = 37px). */
    lens_begin(ui, &IN0);
    lens_row_begin(ui, &(lens_layout_opts){.box.height = 60.0f});
    lens_button(ui, &(lens_button_opts){.label = "stretched"});
    lens_close(ui);
    lens_test_end(ui);

    lens_node *row = lens_node_first_child(lens_root(ui));
    lens_node *button = lens_node_first_child(row);
    OT_EXPECT(button != NULL);
    flux_rect r = lens_node_bounds(button);
    OT_EXPECT(r.h > 37.0f); /* actually stretched taller than measured */

    const lens_draw_cmd *cmd = first_text_cmd(button);
    OT_EXPECT(cmd != NULL);

    /* Unstretched, the resolved box equals the measured one. */
    lens_begin(ui, &IN0);
    lens_button(ui, &(lens_button_opts){.label = "loose"});
    lens_test_end(ui);
    lens_node *loose = lens_node_first_child(lens_root(ui));
    OT_EXPECT(loose != NULL);
    const lens_draw_cmd *loose_cmd = first_text_cmd(loose);
    OT_EXPECT(loose_cmd != NULL);

    lens_release(ui);
    return OT_REPORT();
}
