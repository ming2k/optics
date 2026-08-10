/* skin/collapsing.c — default collapsing-header skin (ADR-0059): the dimmed
 * label that brightens on hover plus the disclosure chevron, moved verbatim
 * from widgets/collapsing.c. The open/close state machine and the body
 * container stay in the widget. */

#include "../internal.h"

void lensi_skin_collapsing(lens *ui, lens_node *n, const lens_widget_record *rec) {
    const lens_style_resolved *rs = &rec->style;
    bool disabled = (rec->state & LENS_STATE_DISABLED) != 0;
    float h = rec->bounds.h;
    float tm_h = rec->content.text.height;
    float label_size = rs->font_size * 0.86f;

    float glow = rec->hover_t > 0.0f ? 0.72f : 0.42f;
    flux_color fg = disabled ? rs->disabled : lensi_lerp_color(rs->disabled, rs->fg, glow);

    /* label */
    float text_y = (h - tm_h) * 0.5f;
    if (text_y < 0.0f)
        text_y = 0.0f;
    lensi_drawlist_push(ui, n,
                        (lens_draw_cmd){.kind = LENS_DRAW_TEXT,
                                        .rel = {0, text_y, 0, 0},
                                        .color = fg,
                                        .text = rec->content.label,
                                        .text_size = label_size,
                                        .text_weight = 400.0f});

    /* chevron disclosure indicator, placed after the label */
    float arrow = tm_h * 0.82f;
    float arrow_x = rec->content.text.width + 6.0f;
    float arrow_y = (h - arrow) * 0.5f;
    lensi_drawlist_push(ui, n,
                        (lens_draw_cmd){.kind = LENS_DRAW_ICON,
                                        .rel = {arrow_x, arrow_y, arrow, arrow},
                                        .color = fg,
                                        .width = 1.8f * (arrow / 24.0f),
                                        .icon_id = rec->content.icon});
}
