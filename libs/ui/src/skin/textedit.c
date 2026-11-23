/* skin/textedit.c — default text-edit skin (ADR-0059).
 * Unifies single-line and multi-line text input rendering. */

#include "../internal.h"

void optics_ui__skin_textedit(optics_ui *ui, optics_ui_node *n,
                              const optics_ui_widget_record *rec) {
    const optics_ui_style_resolved *rs = &rec->style;
    const optics_ui_widget_content *c = &rec->content;
    bool disabled = (rec->state & OPTICS_UI_STATE_DISABLED) != 0;
    bool focused = (rec->state & OPTICS_UI_STATE_FOCUSED) != 0;
    bool hovered = (rec->state & OPTICS_UI_STATE_HOVERED) != 0;

    optics_gfx_color resting_bg =
        (rs->border_width > 0.0f) ? rs->bg : ui->theme.color_surface_sunken;
    optics_gfx_color bg = (hovered && !disabled && c->multiline) ? rs->bg_hover : resting_bg;
    optics_ui__drawlist_push(ui, n,
                             (optics_ui_draw_cmd){.kind = OPTICS_UI_DRAW_RECT,
                                                  .rel = {0, 0, 0, 0},
                                                  .color = bg,
                                                  .radius = rs->corner_radius});

    /* Selection highlight (behind text) */
    for (int i = 0; i < c->sel_rect_count; i++) {
        optics_ui__drawlist_push(
            ui, n,
            (optics_ui_draw_cmd){.kind = OPTICS_UI_DRAW_RECT,
                                 .rel = c->sel_rects[i],
                                 .color = optics_ui__color_alpha(rs->accent, 0x40),
                                 .radius = 1.0f});
    }

    /* Border: disabled / error / focused chain */
    optics_gfx_color border_color = rs->border;
    if (disabled)
        border_color = rs->disabled;
    else if (c->error)
        border_color = ui->theme.color_error;
    else if (focused)
        border_color = rs->accent;

    if (rs->border_width > 0.0f && (border_color & 0xFF000000u) != 0) {
        optics_ui__drawlist_push(ui, n,
                                 (optics_ui_draw_cmd){.kind = OPTICS_UI_DRAW_BORDER,
                                                      .rel = {0, 0, 0, 0},
                                                      .color = border_color,
                                                      .width = rs->border_width,
                                                      .radius = rs->corner_radius});
    } else if (focused || c->error) {
        /* When default border is 0 (minimalist), focused or error state still provides
         * a crisp, accessible focus outline so typing affordance is unmistakable. */
        optics_gfx_color ring_col = c->error ? ui->theme.color_error : rs->accent;
        optics_ui__drawlist_push(ui, n,
                                 (optics_ui_draw_cmd){.kind = OPTICS_UI_DRAW_BORDER,
                                                      .rel = {0, 0, 0, 0},
                                                      .color = ring_col,
                                                      .width = 1.5f,
                                                      .radius = rs->corner_radius});
    }

    /* Text rendering */
    optics_gfx_color text_color = c->show_placeholder ? rs->disabled : rs->fg;
    if (c->multiline) {
        for (int i = 0; i < c->line_count; i++) {
            optics_ui__drawlist_push(
                ui, n,
                (optics_ui_draw_cmd){.kind = OPTICS_UI_DRAW_TEXT,
                                     .rel = {c->lines[i].x, c->lines[i].y, 0, 0},
                                     .color = text_color,
                                     .text = c->lines[i].text,
                                     .text_size = rs->font_size});
        }
    } else if (c->edit_text) {
        float text_x = c->edit_text_x > 0.0f ? c->edit_text_x : rs->padding;
        optics_ui__drawlist_push(ui, n,
                                 (optics_ui_draw_cmd){.kind = OPTICS_UI_DRAW_TEXT,
                                                      .rel = {text_x, c->edit_text_y, 0, 0},
                                                      .color = text_color,
                                                      .text = c->edit_text,
                                                      .text_size = rs->font_size});
    }

    /* Underline beneath the IME preedit region */
    if (c->has_preedit) {
        optics_ui__drawlist_push(ui, n,
                                 (optics_ui_draw_cmd){.kind = OPTICS_UI_DRAW_RECT,
                                                      .rel = c->preedit_underline,
                                                      .color = rs->accent});
    }

    /* Active clause of the composition, emphasised over the underline */
    if (c->preedit_clause.w > 0) {
        optics_ui__drawlist_push(ui, n,
                                 (optics_ui_draw_cmd){.kind = OPTICS_UI_DRAW_RECT,
                                                      .rel = c->preedit_clause,
                                                      .color = rs->accent});
    }

    /* Caret. `show_caret` says a caret exists and is focused; the blink is a
     * *phase* the context owns (ADR-0151), so the skin paints only while the
     * phase is on. The widget cannot decide this itself: ui advances the
     * clock once per frame in `optics_ui_begin`, and a widget that re-derived a
     * phase from its own timer would disagree with the damage walk. */
    if (c->show_caret && ui->caret_visible) {
        optics_ui__drawlist_push(
            ui, n,
            (optics_ui_draw_cmd){
                .kind = OPTICS_UI_DRAW_RECT, .rel = c->caret, .color = rs->accent, .radius = 1.0f});
    }
}
