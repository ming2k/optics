/* test_opacity.c — optics_ui_set_opacity: the frame-scoped, node-stamped fade.
 *
 * The switch stamps each node's isolated visual group without modifying
 * its source paint colours, so
 * siblings built at different opacities coexist in one frame, and reset
 * to 1.0 at optics_ui_begin so a forgotten restore cannot dim the next frame.
 * CPU-only; reads the node's draw list via internal.h (same pattern as
 * test_drawlist_hash).
 */

#include "../../libs/ui/src/internal.h"
#include "optics_test.h"
#include "ui_test.h"
#include <optics/ui.h>

static const optics_ui_input IN0 = {.display_size = {200, 100}, .dt_seconds = 0.016f};

static uint8_t alpha_of(optics_gfx_color c) {
    return (uint8_t)(c >> 24);
}

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

    /* Default, and the setter clamps into 0..1. */
    OT_EXPECT(optics_ui_opacity(ui) == 1.0f);
    optics_ui_set_opacity(ui, 1.7f);
    OT_EXPECT(optics_ui_opacity(ui) == 1.0f);
    optics_ui_set_opacity(ui, -0.25f);
    OT_EXPECT(optics_ui_opacity(ui) == 0.0f);
    optics_ui_set_opacity(ui, 1.0f);

    const float full = (float)alpha_of(optics_ui_get_theme(ui).color_fg);

    /* Frame 1: two labels at different opacities — per-node stamps. */
    optics_ui_begin(ui, &IN0);
    optics_ui_set_opacity(ui, 0.5f);
    optics_ui_label(ui, &(optics_ui_label_opts){.text = "dimmed"});
    optics_ui_set_opacity(ui, 1.0f);
    optics_ui_label(ui, &(optics_ui_label_opts){.text = "solid"});
    optics_ui_test_end(ui);

    optics_ui_node *dimmed = optics_ui_node_first_child(optics_ui_root(ui));
    optics_ui_node *solid = dimmed ? dimmed->next_sibling : nullptr;
    const optics_ui_draw_cmd *dim_cmd = first_text_cmd(dimmed);
    const optics_ui_draw_cmd *solid_cmd = first_text_cmd(solid);
    OT_EXPECT(dim_cmd != nullptr);
    OT_EXPECT(solid_cmd != nullptr);
    OT_EXPECT(alpha_of(dim_cmd->color) == (uint8_t)full);
    OT_EXPECT(dimmed->opacity == 0.5f);
    OT_EXPECT(alpha_of(solid_cmd->color) == (uint8_t)full);

    /* Frame 2: the switch reset with optics_ui_begin — no restore needed. */
    optics_ui_begin(ui, &IN0);
    optics_ui_label(ui, &(optics_ui_label_opts){.text = "dimmed"});
    optics_ui_label(ui, &(optics_ui_label_opts){.text = "solid"});
    optics_ui_test_end(ui);
    dimmed = optics_ui_node_first_child(optics_ui_root(ui));
    dim_cmd = first_text_cmd(dimmed);
    OT_EXPECT(dim_cmd != nullptr);
    OT_EXPECT(alpha_of(dim_cmd->color) == (uint8_t)full);

    /* Premultiplied sRGB invariant (ADR-0109): optics_ui__opacity_color scales all
     * four channels proportionally so R, G, B <= A holds for linear decoding. */
    optics_gfx_color white = optics_gfx_color_rgba_premul(255, 200, 100, 255);
    optics_gfx_color half_white = optics_ui__opacity_color(white, 0.5f);
    OT_EXPECT(half_white == optics_gfx_color_rgba_premul(255, 200, 100, 128));
    OT_EXPECT(optics_ui__opacity_color(white, 0.0f) == 0u);
    OT_EXPECT(optics_ui__opacity_color(white, 1.0f) == white);

    optics_ui_release(ui);
    return OT_REPORT();
}
