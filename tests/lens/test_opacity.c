/* test_opacity.c — lens_set_opacity: the frame-scoped, node-stamped fade.
 *
 * The switch stamps each node's isolated visual group without modifying
 * its source paint colours, so
 * siblings built at different opacities coexist in one frame, and reset
 * to 1.0 at lens_begin so a forgotten restore cannot dim the next frame.
 * CPU-only; reads the node's draw list via internal.h (same pattern as
 * test_drawlist_hash).
 */

#include "../../libs/lens/src/internal.h"
#include "lens_test.h"
#include "optics_test.h"
#include <lens/lens.h>

static const lens_input IN0 = {.display_size = {200, 100}, .dt_seconds = 0.016f};

static uint8_t alpha_of(flux_color c) {
    return (uint8_t)(c >> 24);
}

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

    /* Default, and the setter clamps into 0..1. */
    OT_EXPECT(lens_opacity(ui) == 1.0f);
    lens_set_opacity(ui, 1.7f);
    OT_EXPECT(lens_opacity(ui) == 1.0f);
    lens_set_opacity(ui, -0.25f);
    OT_EXPECT(lens_opacity(ui) == 0.0f);
    lens_set_opacity(ui, 1.0f);

    const float full = (float)alpha_of(lens_get_theme(ui).color_fg);

    /* Frame 1: two labels at different opacities — per-node stamps. */
    lens_begin(ui, &IN0);
    lens_set_opacity(ui, 0.5f);
    lens_label(ui, &(lens_label_opts){.text = "dimmed"});
    lens_set_opacity(ui, 1.0f);
    lens_label(ui, &(lens_label_opts){.text = "solid"});
    lens_test_end(ui);

    lens_node *dimmed = lens_node_first_child(lens_root(ui));
    lens_node *solid = dimmed ? dimmed->next_sibling : nullptr;
    const lens_draw_cmd *dim_cmd = first_text_cmd(dimmed);
    const lens_draw_cmd *solid_cmd = first_text_cmd(solid);
    OT_EXPECT(dim_cmd != nullptr);
    OT_EXPECT(solid_cmd != nullptr);
    OT_EXPECT(alpha_of(dim_cmd->color) == (uint8_t)full);
    OT_EXPECT(dimmed->opacity == 0.5f);
    OT_EXPECT(alpha_of(solid_cmd->color) == (uint8_t)full);

    /* Frame 2: the switch reset with lens_begin — no restore needed. */
    lens_begin(ui, &IN0);
    lens_label(ui, &(lens_label_opts){.text = "dimmed"});
    lens_label(ui, &(lens_label_opts){.text = "solid"});
    lens_test_end(ui);
    dimmed = lens_node_first_child(lens_root(ui));
    dim_cmd = first_text_cmd(dimmed);
    OT_EXPECT(dim_cmd != nullptr);
    OT_EXPECT(alpha_of(dim_cmd->color) == (uint8_t)full);

    /* Premultiplied sRGB invariant (ADR-0109): lensi_opacity_color scales all
     * four channels proportionally so R, G, B <= A holds for linear decoding. */
    flux_color white = flux_color_rgba_premul(255, 200, 100, 255);
    flux_color half_white = lensi_opacity_color(white, 0.5f);
    OT_EXPECT(half_white == flux_color_rgba_premul(255, 200, 100, 128));
    OT_EXPECT(lensi_opacity_color(white, 0.0f) == 0u);
    OT_EXPECT(lensi_opacity_color(white, 1.0f) == white);

    lens_release(ui);
    return OT_REPORT();
}
