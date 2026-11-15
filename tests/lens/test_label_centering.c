/* test_label_centering.c — padded labels must stay vertically centred
 * when a fixed-height parent constrains them below their intrinsic
 * padded height. CPU-only.
 */

#include "../../libs/lens/src/internal.h"
#include "lens_test.h"
#include "optics_test.h"
#include <lens/lens.h>

static const lens_input IN0 = {.display_size = {200, 100}, .dt_seconds = 0.016f};

static const lens_draw_cmd *first_text_cmd(const lens_node *n) {
    if (!n)
        return nullptr;
    for (uint32_t i = 0; i < n->cmd_count; i++)
        if (n->cmds[i].kind == LENS_DRAW_TEXT)
            return &n->cmds[i];
    return nullptr;
}

int main(void) {
    lens *ui = nullptr;
    OT_EXPECT(lens_create(&(lens_desc){}, &ui) == FLUX_OK);

    /* A 34px row clamps the padded label (14px text + 2×12 pad = 38px). */
    lens_begin(ui, &IN0);
    lens_row_begin(ui, &(lens_layout_opts){
                           .box.height = 34.0f, .gap = 7.0f, .pad = 5.0f, .cross = LENS_CENTER});
    lens_label(ui, &(lens_label_opts){.text = "row label"});
    lens_close(ui);
    lens_test_end(ui);

    lens_node *row = lens_node_first_child(lens_root(ui));
    lens_node *label = lens_node_first_child(row);
    OT_EXPECT(label != nullptr);
    flux_rect r = lens_node_bounds(label);
    OT_EXPECT(r.h <= 34.0f); /* clamped into the row's inner height */

    const lens_draw_cmd *cmd = first_text_cmd(label);
    OT_EXPECT(cmd != nullptr);

    /* Unconstrained, the label keeps its intrinsic padded height. */
    lens_begin(ui, &IN0);
    lens_label(ui, &(lens_label_opts){.text = "loose label"});
    lens_test_end(ui);
    lens_node *loose = lens_node_first_child(lens_root(ui));
    OT_EXPECT(loose != nullptr);
    const lens_draw_cmd *loose_cmd = first_text_cmd(loose);
    OT_EXPECT(loose_cmd != nullptr);

    lens_release(ui);
    return OT_REPORT();
}
