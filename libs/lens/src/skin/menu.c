/* skin/menu.c — default menu skin (ADR-0059): the bar trigger, the menu
 * item (check/radio glyph, label, dimmed shortcut), the submenu row
 * (trailing chevron), and the thin separator — moved verbatim from
 * widgets/menu.c. Open state, the bar-switch behaviour, hover-dwell
 * timers, and the place popups stay in the widget. */

#include "../internal.h"

void lensi_skin_menu_item(lens *ui, lens_node *n, const lens_widget_record *rec) {
    const lens_style_resolved *rs = &rec->style;
    const lens_widget_content *c = &rec->content;
    bool disabled = (rec->state & LENS_STATE_DISABLED) != 0;
    float h = rec->bounds.h;
    float tm_h = c->text.height;
    float font_size = rs->font_size;
    float padding = rs->padding;

    if (c->menu_separator) {
        lensi_drawlist_push(
            ui, n,
            (lens_draw_cmd){.kind = LENS_DRAW_RECT,
                            .rel = {padding, padding * 0.5f, -padding * 2.0f, 1.0f},
                            .color = rs->border,
                            .radius = 0.0f});
        return;
    }

    if (c->menu_trigger) {
        flux_color bg = c->popup_open
                            ? rs->bg_hover
                            : lensi_lerp_color(rs->bg, rs->bg_hover, rec->hover_t);
        lensi_drawlist_push(
            ui, n,
            (lens_draw_cmd){
                .kind = LENS_DRAW_RECT, .rel = {0, 0, 0, 0}, .color = bg, .radius = rs->corner_radius});
        lensi_drawlist_push(ui, n,
                            (lens_draw_cmd){.kind = LENS_DRAW_TEXT,
                                            .rel = {padding, (h - tm_h) * 0.5f, 0, 0},
                                            .color = rs->fg,
                                            .text = c->label,
                                            .text_size = font_size});
        return;
    }

    /* item / submenu row: hover fill (the submenu draws it even at rest,
     * matching the pre-skin code). */
    if (c->submenu || (rec->hover_t > 0.01f && !disabled)) {
        flux_color bg = lensi_lerp_color(rs->bg_hover, rs->bg_pressed, rec->hover_t);
        lensi_drawlist_push(
            ui, n,
            (lens_draw_cmd){
                .kind = LENS_DRAW_RECT, .rel = {0, 0, 0, 0}, .color = bg, .radius = 0.0f});
    }

    /* check / radio glyph on the left */
    float glyph = 0.0f;
    if (c->menu_check) {
        glyph = font_size * 0.8f + padding;
        lensi_drawlist_push(ui, n,
                            (lens_draw_cmd){.kind = LENS_DRAW_TEXT,
                                            .rel = {padding, (h - tm_h) * 0.5f, 0, 0},
                                            .color = rs->accent,
                                            .text = c->menu_radio ? "●" : "✓",
                                            .text_size = font_size * 0.8f});
    }

    /* label */
    lensi_drawlist_push(ui, n,
                        (lens_draw_cmd){.kind = LENS_DRAW_TEXT,
                                        .rel = {padding + glyph, (h - tm_h) * 0.5f, 0, 0},
                                        .color = disabled ? rs->disabled : rs->fg,
                                        .text = c->label,
                                        .text_size = font_size});

    /* shortcut right-aligned, dimmed */
    float sc_w = 0.0f;
    if (c->shortcut)
        sc_w = c->shortcut_text.width + padding * 2.0f;
    if (c->shortcut && sc_w > 0.0f) {
        lensi_drawlist_push(ui, n,
                            (lens_draw_cmd){.kind = LENS_DRAW_TEXT,
                                            .rel = {-padding, (h - tm_h) * 0.5f, sc_w, 0},
                                            .color = rs->disabled,
                                            .text = c->shortcut,
                                            .text_size = font_size * 0.9f});
    }

    /* trailing chevron for a submenu */
    if (c->submenu) {
        float icon_size = font_size;
        lensi_drawlist_push(
            ui, n,
            (lens_draw_cmd){.kind = LENS_DRAW_ICON,
                            .rel = {-padding, (h - icon_size) * 0.5f, icon_size, icon_size},
                            .color = rs->disabled,
                            .width = 1.8f * (icon_size / 24.0f),
                            .icon_id = LENS_ICON_CHEVRON_RIGHT});
    }
}
