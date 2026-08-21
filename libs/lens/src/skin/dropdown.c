/* skin/dropdown.c — default dropdown-trigger skin (ADR-0059): the filled
 * trigger with preview label, trailing chevron, and border, moved verbatim
 * from widgets/dropdown.c. The popup (placement, option list, dismissal)
 * is place + cascade machinery and stays in the widget. */

#include "../internal.h"

void lensi_skin_dropdown(lens *ui, lens_node *n, const lens_widget_record *rec) {
    const lens_style_resolved *rs = &rec->style;
    bool disabled = (rec->state & LENS_STATE_DISABLED) != 0;
    bool open = rec->content.popup_open;
    bool focused = (rec->state & LENS_STATE_FOCUSED) != 0;
    float h = rec->bounds.h;

    float emphasis = (open || focused) ? 0.72f : rec->hover_t;
    flux_color bg = disabled ? rs->disabled : lensi_lerp_color(rs->bg, rs->bg_hover, emphasis);
    lensi_drawlist_push(
        ui, n,
        (lens_draw_cmd){
            .kind = LENS_DRAW_RECT, .rel = {0, 0, 0, 0}, .color = bg, .radius = rs->corner_radius});

    lensi_drawlist_push(
        ui, n,
        (lens_draw_cmd){.kind = LENS_DRAW_TEXT,
                        .rel = {rs->padding, (h - rec->content.text.height) * 0.5f, 0, 0},
                        .color = rs->fg,
                        .text = rec->content.label,
                        .text_size = rs->font_size});

    float icon_size = rs->font_size;
    float icon_y = (h - icon_size) * 0.5f;
    lensi_drawlist_push(ui, n,
                        (lens_draw_cmd){.kind = LENS_DRAW_ICON,
                                        .rel = {-rs->padding, icon_y, icon_size, icon_size},
                                        .color = (open || focused) ? rs->accent : rs->fg,
                                        .width = 1.8f * (icon_size / 24.0f),
                                        .icon_id = rec->content.icon});

    lensi_drawlist_push(ui, n,
                        (lens_draw_cmd){.kind = LENS_DRAW_BORDER,
                                        .rel = {0, 0, 0, 0},
                                        .color = rs->border,
                                        .radius = rs->corner_radius,
                                        .width = rs->border_width});
}
