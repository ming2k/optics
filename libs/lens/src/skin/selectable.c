/* skin/selectable.c — default selectable-row skin (ADR-0059).
 *
 * The pre-skin emit section of lens_selectable / lens_selectable_icon:
 * transparent at rest, subtle hover fill, steady active-surface tint when
 * selected. That tint is the neutral selection affordance; decorative
 * accent rails are flavor and belong to caller skins (ADR-0061). */

#include "../internal.h"

void lensi_skin_selectable(lens *ui, lens_node *n, const lens_widget_record *rec) {
    const lens_style_resolved *rs = &rec->style;
    bool disabled = (rec->state & LENS_STATE_DISABLED) != 0;
    bool selected = (rec->state & LENS_STATE_SELECTED) != 0;
    float h = rec->bounds.h;

    bool has_icon = rec->content.icon != 0 && lensi_icon_valid((int32_t)rec->content.icon);
    float icon_size = has_icon ? rs->font_size : 0.0f;
    float icon_gap = has_icon ? 8.0f : 0.0f;

    /* Background: transparent at rest. A selected row always uses the
     * active-surface colour. */
    float fill = rec->hover_t * 0.6f;
    if (selected || fill > 0.001f) {
        flux_color bg = selected ? rs->bg_pressed : lensi_lerp_color(rs->bg, rs->bg_hover, fill);
        lensi_drawlist_push(ui, n,
                            (lens_draw_cmd){.kind = LENS_DRAW_RECT,
                                            .rel = {0, 0, 0, 0},
                                            .color = bg,
                                            .radius = rs->corner_radius});
    }

    float text_y = (h - rec->content.text.height) * 0.5f;
    if (text_y < 0.0f)
        text_y = 0.0f;

    float x = rs->padding;
    flux_color fg = disabled ? rs->disabled : rs->fg;
    if (has_icon) {
        float icon_y = (h - icon_size) * 0.5f;
        lensi_drawlist_push(ui, n,
                            (lens_draw_cmd){.kind = LENS_DRAW_ICON,
                                            .rel = {x, icon_y, icon_size, icon_size},
                                            .color = fg,
                                            .width = 2.0f * (icon_size / 24.0f),
                                            .icon_id = rec->content.icon});
        x += icon_size + icon_gap;
    }

    lensi_drawlist_push(ui, n,
                        (lens_draw_cmd){.kind = LENS_DRAW_TEXT,
                                        .rel = {x, text_y, 0, 0}, /* left-aligned (rel.w 0) */
                                        .color = fg,
                                        .text = rec->content.label,
                                        .text_size = rs->font_size});
}
