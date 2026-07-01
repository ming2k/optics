/* selectable.c — borderless, full-width selectable row (ADR-0008).
 *
 * A VS Code-style list / nav item: transparent at rest, a subtle fill on
 * hover, and a steady highlight when `selected`. The selected row uses the
 * theme corner radius and follows `active_indicator_width`: when the width is
 * positive, draw the left accent bar and accent text; when it is zero, keep a
 * calm tint-only row with foreground text. In a stretched column it spans the
 * full cross width, so the whole row is the hit target.
 */

#include "../internal.h"

static bool selectable_impl(lens *ui, lens_icon_id icon, const char *label, bool selected) {
    const lens_theme *t = &ui->theme;
    bool disabled = ui->next_disabled;
    ui->next_disabled = false;
    ui->next_error = false; /* drain so it never leaks to a later widget */
    lens_id id = lensi_gen_widget_id(ui, label);
    lens_node *n = lensi_store_touch(ui, id);
    if (!n)
        return false;
    lensi_link_child(ui, n);
    n->is_container = false;

    lens_text_metrics tm = lensi_text_measure_label(ui, label, t->font_size, 0.0f);
    bool has_icon = (unsigned)icon < LENS_ICON_COUNT;
    float icon_size = has_icon ? t->font_size : 0.0f;
    float icon_gap = has_icon ? 8.0f : 0.0f;
    float content_w = icon_size + icon_gap + tm.width;
    float w = (n->fixed_w > 0) ? n->fixed_w : content_w + 2.0f * t->padding;
    float h = (n->fixed_h > 0) ? n->fixed_h : t->font_size + 2.0f * t->padding;
    n->measured = (flux_point){w, h};

    lens_response r = lensi_interact(ui, n, true, disabled);
    uint32_t sem_flags = (r.focused ? LENS_A11Y_FOCUSED : 0) | (disabled ? LENS_A11Y_DISABLED : 0) |
                         (selected ? LENS_A11Y_CHECKED : 0);
    lensi_node_semantics(ui, n, LENS_ROLE_BUTTON, label, NULL, sem_flags);

    if (!disabled)
        n->hover_t = r.hovered ? 1.f : 0.f;

    /* Optional accent bar marks the selected row when the theme asks for it. */
    float indicator_w = selected ? t->active_indicator_width : 0.0f;

    /* Background: transparent at rest. A selected row keeps a steady fill;
     * when accent indicators are disabled, keep the fill neutral so list rows
     * do not turn into blue/purple selection pills. */
    float fill = n->hover_t * 0.6f;
    if (selected || fill > 0.001f) {
        flux_color selected_bg = (indicator_w > 0.0f)
                                     ? t->color_active
                                     : lensi_lerp_color(t->color_bg, t->color_hover, 0.55f);
        flux_color bg = selected ? selected_bg : lensi_lerp_color(t->color_bg, t->color_hover, fill);
        lensi_drawlist_push(
            ui, n,
            (lens_draw_cmd){
                .kind = LENS_DRAW_RECT, .rel = {0, 0, 0, 0}, .color = bg, .radius = t->corner_radius});
    }

    if (indicator_w > 0.0f)
        lensi_drawlist_push(ui, n,
                            (lens_draw_cmd){.kind = LENS_DRAW_RECT,
                                            .rel = {0, 0, indicator_w, 0},
                                            .color = t->color_accent,
                                            .radius = t->corner_radius});

    float text_y = (h - tm.height) * 0.5f;
    if (text_y < 0.0f)
        text_y = 0.0f;

    float x = t->padding;
    flux_color fg = disabled                    ? t->color_disabled
                    : indicator_w > 0.0f        ? t->color_accent
                                                : t->color_fg;
    if (has_icon) {
        float icon_y = (h - icon_size) * 0.5f;
        lensi_drawlist_push(ui, n,
                            (lens_draw_cmd){.kind = LENS_DRAW_ICON,
                                            .rel = {x, icon_y, icon_size, icon_size},
                                            .color = fg,
                                            .width = 1.75f * (icon_size / 24.0f),
                                            .icon_id = icon});
        x += icon_size + icon_gap;
    }

    lensi_drawlist_push(
        ui, n,
        (lens_draw_cmd){.kind = LENS_DRAW_TEXT,
                        .rel = {x, text_y, 0, 0}, /* left-aligned (rel.w 0) */
                        .color = fg,
                        .text = label,
                        .text_size = t->font_size});

    ui->last_response = r;
    return r.clicked;
}

bool lens_selectable(lens *ui, const char *label, bool selected) {
    return selectable_impl(ui, LENS_ICON_COUNT, label, selected);
}

bool lens_selectable_icon(lens *ui, lens_icon_id icon, const char *label, bool selected) {
    return selectable_impl(ui, icon, label, selected);
}

lens_response lens_selectable_ex(lens *ui, lens_selectable_opts o) {
    lensi_apply_box(ui, o.box);
    bool scoped = o.box.id && o.box.id[0];
    if (scoped)
        lens_push_id(ui, o.box.id);
    selectable_impl(ui, LENS_ICON_COUNT, o.label ? o.label : "", o.selected);
    if (scoped)
        lens_pop_id(ui);
    if (o.box.tooltip)
        lensi_tooltip(ui, o.box.tooltip);
    return ui->last_response;
}
