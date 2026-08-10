/* skin/textfield.c — default single-line text-edit skin (ADR-0059): field
 * surface, selection highlight, border state chain, display string (with
 * the IME preedit pre-composed by the widget), preedit underline, and the
 * caret — moved verbatim from widgets/textfield.c. Editing, selection,
 * caret geometry, and the platform caret reporting (ADR-0036) are
 * behaviour and stay in the widget; every rect here is precomputed and
 * node-local. */

#include "../internal.h"

void lensi_skin_textfield(lens *ui, lens_node *n, const lens_widget_record *rec) {
    const lens_style_resolved *rs = &rec->style;
    const lens_widget_content *c = &rec->content;
    bool disabled = (rec->state & LENS_STATE_DISABLED) != 0;
    bool focused = (rec->state & LENS_STATE_FOCUSED) != 0;

    /* Background */
    lensi_drawlist_push(ui, n,
                        (lens_draw_cmd){.kind = LENS_DRAW_RECT,
                                        .rel = {0, 0, 0, 0},
                                        .color = rs->bg,
                                        .radius = rs->corner_radius});

    /* Selection highlight (behind text) */
    for (int i = 0; i < c->sel_rect_count; i++) {
        lensi_drawlist_push(ui, n,
                            (lens_draw_cmd){.kind = LENS_DRAW_RECT,
                                            .rel = c->sel_rects[i],
                                            .color = lensi_color_alpha(rs->accent, 0x40),
                                            .radius = 1.0f});
    }

    /* Border: disabled / error / focused chain. The error token has no
     * style slot, so the built-in skin reads it from the theme (the
     * ADR-0059 carve-out for slot-less tokens). */
    flux_color border_color = rs->border;
    if (disabled)
        border_color = rs->disabled;
    else if (c->error)
        border_color = ui->theme.color_error;
    else if (focused)
        border_color = rs->accent;
    lensi_drawlist_push(ui, n,
                        (lens_draw_cmd){.kind = LENS_DRAW_BORDER,
                                        .rel = {0, 0, 0, 0},
                                        .color = border_color,
                                        .width = rs->border_width,
                                        .radius = rs->corner_radius});

    /* Display string (buffer, or buffer with the preedit composed in, or
     * the placeholder) */
    if (c->edit_text) {
        lensi_drawlist_push(ui, n,
                            (lens_draw_cmd){.kind = LENS_DRAW_TEXT,
                                            .rel = {rs->padding, c->edit_text_y, 0, 0},
                                            .color = c->show_placeholder ? rs->disabled : rs->fg,
                                            .text = c->edit_text,
                                            .text_size = rs->font_size});
    }

    /* Underline beneath the preedit region */
    if (c->has_preedit) {
        lensi_drawlist_push(ui, n,
                            (lens_draw_cmd){.kind = LENS_DRAW_RECT,
                                            .rel = c->preedit_underline,
                                            .color = rs->accent});
    }

    /* Caret */
    if (c->show_caret) {
        lensi_drawlist_push(ui, n,
                            (lens_draw_cmd){.kind = LENS_DRAW_RECT,
                                            .rel = c->caret,
                                            .color = rs->accent,
                                            .radius = 1.0f});
    }
}
