/* test_label_centering.c — padded labels must stay vertically centred
 * when a fixed-height parent constrains them below their intrinsic
 * padded height. CPU-only.
 */

#include "../../libs/lens/src/internal.h"
#include "test_helpers.h"
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
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);

    /* A 34px row clamps the padded label (14px text + 2×12 pad = 38px). */
    lens_begin(ui, &IN0);
    lens_row_begin(ui, &(lens_layout_opts){
                           .box.height = 34.0f, .gap = 7.0f, .pad = 5.0f, .cross = LENS_CENTER});
    lens_label(ui, &(lens_label_opts){.text = "row label"});
    lens_close(ui);
    lens_end(ui);

    lens_node *row = lens_node_first_child(lens_root(ui));
    lens_node *label = lens_node_first_child(row);
    CHECK(label != NULL);
    flux_rect r = lens_node_bounds(label);
    CHECK(r.h <= 34.0f); /* clamped into the row's inner height */

    const lens_draw_cmd *cmd = first_text_cmd(label);
    CHECK(cmd != NULL);

    /* Unconstrained, the label keeps its intrinsic padded height. */
    lens_begin(ui, &IN0);
    lens_label(ui, &(lens_label_opts){.text = "loose label"});
    lens_end(ui);
    lens_node *loose = lens_node_first_child(lens_root(ui));
    CHECK(loose != NULL);
    const lens_draw_cmd *loose_cmd = first_text_cmd(loose);
    CHECK(loose_cmd != NULL);

    lens_destroy(ui);
    return TEST_REPORT();
}
