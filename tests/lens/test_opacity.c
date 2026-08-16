/* test_opacity.c — lens_set_opacity: the frame-scoped, node-stamped fade.
 *
 * The switch must bake into every draw command's colour alpha at emission
 * (one knob fading text/rects/icons/images together), stamp per node so
 * siblings built at different opacities coexist in one frame, and reset
 * to 1.0 at lens_begin so a forgotten restore cannot dim the next frame.
 * CPU-only; reads the node's draw list via internal.h (same pattern as
 * test_drawlist_hash).
 */

#include "../../libs/lens/src/internal.h"
#include "test_helpers.h"
#include <lens/lens.h>

static const lens_input IN0 = {.display_size = {200, 100}, .dt_seconds = 0.016f};

static uint8_t alpha_of(flux_color c) {
    return (uint8_t)(c >> 24);
}

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

    /* Default, and the setter clamps into 0..1. */
    CHECK(lens_opacity(ui) == 1.0f);
    lens_set_opacity(ui, 1.7f);
    CHECK(lens_opacity(ui) == 1.0f);
    lens_set_opacity(ui, -0.25f);
    CHECK(lens_opacity(ui) == 0.0f);
    lens_set_opacity(ui, 1.0f);

    const float full = (float)alpha_of(lens_get_theme(ui).color_fg);

    /* Frame 1: two labels at different opacities — per-node stamps. */
    lens_begin(ui, &IN0);
    lens_set_opacity(ui, 0.5f);
    lens_label(ui, "dimmed");
    lens_set_opacity(ui, 1.0f);
    lens_label(ui, "solid");
    lens_end(ui);

    lens_node *dimmed = lens_node_first_child(lens_root(ui));
    lens_node *solid = dimmed ? dimmed->next_sibling : NULL;
    const lens_draw_cmd *dim_cmd = first_text_cmd(dimmed);
    const lens_draw_cmd *solid_cmd = first_text_cmd(solid);
    CHECK(dim_cmd != NULL);
    CHECK(solid_cmd != NULL);
    CHECK(alpha_of(dim_cmd->color) == (uint8_t)(full * 0.5f + 0.5f));
    CHECK(alpha_of(solid_cmd->color) == (uint8_t)full);

    /* Frame 2: the switch reset with lens_begin — no restore needed. */
    lens_begin(ui, &IN0);
    lens_label(ui, "dimmed");
    lens_label(ui, "solid");
    lens_end(ui);
    dimmed = lens_node_first_child(lens_root(ui));
    dim_cmd = first_text_cmd(dimmed);
    CHECK(dim_cmd != NULL);
    CHECK(alpha_of(dim_cmd->color) == (uint8_t)full);

    lens_destroy(ui);
    return TEST_REPORT();
}
