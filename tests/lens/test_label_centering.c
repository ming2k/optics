/* test_label_centering.c — padded labels must stay vertically centred
 * when a fixed-height parent constrains them below their intrinsic
 * padded height. CPU-only.
 *
 * A padded label measures text + 2×theme padding, so any chrome row
 * shorter than that clamps the node at layout time. The draw command must
 * therefore carry the replay-time centring convention (negative rel.h —
 * "centre in the final node height", shared with lens_heading) instead of
 * a build-time pad offset: a fixed rel.y glued the text to the bottom of
 * the clamped box (the "header text sits low" regression). internal.h is
 * included directly (same pattern as test_drawlist_hash) to read the
 * node's draw list; only struct fields are read.
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
    lens_row_ex(ui, (lens_layout_opts){
                        .box.height = 34.0f, .gap = 7.0f, .pad = 5.0f, .cross = LENS_CENTER});
    lens_label(ui, "row label");
    lens_close(ui);
    lens_end(ui);

    lens_node *row = lens_node_first_child(lens_root(ui));
    lens_node *label = lens_node_first_child(row);
    CHECK(label != NULL);
    flux_rect r = lens_node_bounds(label);
    CHECK(r.h <= 34.0f); /* clamped into the row's inner height */

    const lens_draw_cmd *cmd = first_text_cmd(label);
    CHECK(cmd != NULL);
    CHECK(cmd->rel.h < 0.0f); /* replay centres vertically in the final box */

    /* Unconstrained, the label keeps its intrinsic padded height and the
     * same convention (centring is then identical to the old top-pad). */
    lens_begin(ui, &IN0);
    lens_label(ui, "loose label");
    lens_end(ui);
    lens_node *loose = lens_node_first_child(lens_root(ui));
    CHECK(loose != NULL);
    const lens_draw_cmd *loose_cmd = first_text_cmd(loose);
    CHECK(loose_cmd != NULL);
    CHECK(loose_cmd->rel.h < 0.0f);

    lens_destroy(ui);
    return TEST_REPORT();
}
