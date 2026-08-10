/* skin/radio.c — default radio skin (ADR-0059). The pre-skin emit section
 * of lens_radio, moved verbatim: circle, inner dot for the picked option,
 * border lifted toward fg, label. */

#include "../internal.h"

void lensi_skin_radio(lens *ui, lens_node *n, const lens_widget_record *rec) {
    const lens_style_resolved *rs = &rec->style;
    bool disabled = (rec->state & LENS_STATE_DISABLED) != 0;
    bool on = (rec->state & LENS_STATE_SELECTED) != 0;

    float circle = roundf(rs->font_size);
    float circle_y = roundf((rec->bounds.h - circle) * 0.5f);

    /* circle background */
    lensi_drawlist_push(ui, n,
                        (lens_draw_cmd){.kind = LENS_DRAW_RECT,
                                        .rel = {rs->padding, circle_y, circle, circle},
                                        .color = rs->bg,
                                        .radius = circle * 0.5f});

    /* inner dot when selected */
    if (on) {
        float dot_pad = circle * 0.25f;
        lensi_drawlist_push(
            ui, n,
            (lens_draw_cmd){.kind = LENS_DRAW_RECT,
                            .rel = {rs->padding + dot_pad, circle_y + dot_pad,
                                    circle - 2.0f * dot_pad, circle - 2.0f * dot_pad},
                            .color = disabled ? rs->disabled : rs->accent,
                            .radius = (circle - 2.0f * dot_pad) * 0.5f});
    }

    /* circle border — same treatment as the checkbox: color_border alone is
     * too subtle at this size on dark cards; hover emphasizes with accent
     * instead of going darker toward color_hover. */
    flux_color idle_border = lensi_lerp_color(rs->border, rs->fg, 0.35f);
    flux_color circle_border =
        disabled ? rs->disabled
                 : (on ? rs->accent : lensi_lerp_color(idle_border, rs->accent, rec->hover_t));
    lensi_drawlist_push(ui, n,
                        (lens_draw_cmd){.kind = LENS_DRAW_BORDER,
                                        .rel = {rs->padding, circle_y, circle, circle},
                                        .color = circle_border,
                                        .radius = circle * 0.5f,
                                        .width = rs->border_width});

    /* label */
    lensi_drawlist_push(ui, n,
                        (lens_draw_cmd){.kind = LENS_DRAW_TEXT,
                                        .rel = {rs->padding + circle + 6.0f, rs->padding, 0, 0},
                                        .color = rs->fg,
                                        .text = rec->content.label,
                                        .text_size = rs->font_size});
}
