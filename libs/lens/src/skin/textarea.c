/* skin/textarea.c — default multi-line text-edit skin (ADR-0059): hover
 * surface, per-line selection highlight, the visible line slices (IME
 * preedit pre-composed by the widget), preedit underline and
 * active-clause emphasis, caret, and the border state chain — moved
 * verbatim from widgets/textarea.c. Editing, line windowing, scroll
 * clamping, and the platform caret/text-context reporting (ADR-0036) are
 * behaviour and stay in the widget; every rect and slice here is
 * precomputed and node-local. */

#include "../internal.h"

void lensi_skin_textarea(lens *ui, lens_node *n, const lens_widget_record *rec) {
    const lens_style_resolved *rs = &rec->style;
    const lens_widget_content *c = &rec->content;
    bool disabled = (rec->state & LENS_STATE_DISABLED) != 0;
    bool focused = (rec->state & LENS_STATE_FOCUSED) != 0;
    bool hovered = (rec->state & LENS_STATE_HOVERED) != 0;

    uint32_t bg = (hovered && !disabled) ? rs->bg_hover : rs->bg;
    lensi_drawlist_push(
        ui, n,
        (lens_draw_cmd){
            .kind = LENS_DRAW_RECT, .rel = {0, 0, 0, 0}, .color = bg, .radius = rs->corner_radius});

    /* Selection highlight (behind text) */
    for (int i = 0; i < c->sel_rect_count; i++) {
        lensi_drawlist_push(ui, n,
                            (lens_draw_cmd){.kind = LENS_DRAW_RECT,
                                            .rel = c->sel_rects[i],
                                            .color = lensi_color_alpha(rs->accent, 0x40),
                                            .radius = 1.0f});
    }

    /* Text lines (the placeholder rides lines[0] when the buffer is empty
     * and unfocused) */
    flux_color text_color = c->show_placeholder ? rs->disabled : rs->fg;
    for (int i = 0; i < c->line_count; i++) {
        lensi_drawlist_push(ui, n,
                            (lens_draw_cmd){.kind = LENS_DRAW_TEXT,
                                            .rel = {c->lines[i].x, c->lines[i].y, 0, 0},
                                            .color = text_color,
                                            .text = c->lines[i].text,
                                            .text_size = rs->font_size});
    }

    /* Underline beneath the IME preedit region */
    if (c->has_preedit) {
        lensi_drawlist_push(ui, n,
                            (lens_draw_cmd){.kind = LENS_DRAW_RECT,
                                            .rel = c->preedit_underline,
                                            .color = rs->accent});
    }

    /* Active clause of the composition, emphasised over the underline */
    if (c->preedit_clause.w > 0) {
        lensi_drawlist_push(
            ui, n,
            (lens_draw_cmd){.kind = LENS_DRAW_RECT, .rel = c->preedit_clause, .color = rs->accent});
    }

    /* Caret */
    if (c->show_caret) {
        lensi_drawlist_push(
            ui, n,
            (lens_draw_cmd){
                .kind = LENS_DRAW_RECT, .rel = c->caret, .color = rs->accent, .radius = 1.0f});
    }

    /* Border: error / focused / disabled chain. The error token has no
     * style slot, so the built-in skin reads it from the theme (the
     * ADR-0059 carve-out for slot-less tokens). */
    uint32_t border_color =
        c->error ? ui->theme.color_error : ((focused && !disabled) ? rs->accent : rs->border);
    if (disabled)
        border_color = rs->disabled;
    lensi_drawlist_push(ui, n,
                        (lens_draw_cmd){.kind = LENS_DRAW_BORDER,
                                        .rel = {0, 0, 0, 0},
                                        .color = border_color,
                                        .width = rs->border_width,
                                        .radius = rs->corner_radius});
}
