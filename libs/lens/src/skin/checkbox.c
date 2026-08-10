/* skin/checkbox.c — default checkbox skin (ADR-0059). The pre-skin emit
 * section of lens_checkbox, moved verbatim: filled box, accent mark when
 * on, border lifted toward fg so it survives dark cards, label. */

#include "../internal.h"

void lensi_skin_checkbox(lens *ui, lens_node *n, const lens_widget_record *rec) {
    const lens_style_resolved *rs = &rec->style;
    bool disabled = (rec->state & LENS_STATE_DISABLED) != 0;
    bool on = (rec->state & LENS_STATE_ACTIVE) != 0;

    /* Box is a font-size square so it stays visible even with no caption
     * (e.g. "##id" labels used for form layout). */
    float box = rs->font_size;
    float box_y = (rec->bounds.h - box) * 0.5f;

    /* color_border is tuned for layout hairlines (separators, card outlines)
     * and nearly vanishes on dark cards at this size, so lift the idle box
     * border toward fg. Hover emphasizes with accent (same idiom as the
     * focused border in textfield/textarea); the previous border->hover lerp
     * went darker on the dark theme, making a hovered box less visible. */
    flux_color idle_border = lensi_lerp_color(rs->border, rs->fg, 0.35f);
    flux_color box_border =
        disabled ? rs->disabled
                 : (on ? rs->accent : lensi_lerp_color(idle_border, rs->accent, rec->hover_t));

    /* box background — always fill so the border has something to contrast
     * against, especially on dark cards in light mode. */
    lensi_drawlist_push(ui, n,
                        (lens_draw_cmd){.kind = LENS_DRAW_RECT,
                                        .rel = {rs->padding, box_y, box, box},
                                        .color = rs->bg,
                                        .radius = 3.0f});

    if (on)
        lensi_drawlist_push(
            ui, n,
            (lens_draw_cmd){.kind = LENS_DRAW_RECT,
                            .rel = {rs->padding + 3.0f, box_y + 3.0f, box - 6.0f, box - 6.0f},
                            .color = disabled ? rs->disabled : rs->accent,
                            .radius = 2.0f});

    /* box border */
    lensi_drawlist_push(ui, n,
                        (lens_draw_cmd){.kind = LENS_DRAW_BORDER,
                                        .rel = {rs->padding, box_y, box, box},
                                        .color = box_border,
                                        .radius = 3.0f,
                                        .width = rs->border_width});

    lensi_drawlist_push(ui, n,
                        (lens_draw_cmd){.kind = LENS_DRAW_TEXT,
                                        .rel = {rs->padding + box + 6.0f, rs->padding, 0, 0},
                                        .color = rs->fg,
                                        .text = rec->content.label,
                                        .text_size = rs->font_size});
}
