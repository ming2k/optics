/* selectable.c — borderless, full-width selectable row (ADR-0008).
 *
 * A VS Code-style list / nav item: transparent at rest, a subtle fill on
 * hover, and a steady highlight (fill + accent text + left accent bar) when
 * `selected`. Unlike lens_button it paints no filled accent background and no
 * rounded "pill" shape, so a column of selectables reads as one flat
 * navigation list rather than a stack of bordered buttons. In a stretched
 * column it spans the full cross width, so the whole row is the hit target.
 */

#include "../internal.h"

static bool selectable_impl(lens *ui, const char *label, bool selected) {
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
    float w = (n->fixed_w > 0) ? n->fixed_w : tm.width + 2.0f * t->padding;
    float h = (n->fixed_h > 0) ? n->fixed_h : t->font_size + 2.0f * t->padding;
    n->measured = (flux_point){w, h};

    lens_response r = lensi_interact(ui, n, true, disabled);
    uint32_t sem_flags = (r.focused ? LENS_A11Y_FOCUSED : 0) | (disabled ? LENS_A11Y_DISABLED : 0) |
                         (selected ? LENS_A11Y_CHECKED : 0);
    lensi_node_semantics(ui, n, LENS_ROLE_BUTTON, label, NULL, sem_flags);

    float dt = ui->input.dt_seconds;
    if (!disabled)
        n->hover_t = lensi_approach(ui, n->hover_t, r.hovered ? 1.f : 0.f, dt, 12.f);

    /* Background: transparent at rest. A selected row keeps a steady fill;
     * an unselected one lifts toward color_hover only while hovered. No
     * border and no corner radius, so adjacent rows tile into a flat list. */
    float fill = selected ? 1.0f : n->hover_t * 0.6f;
    if (fill > 0.001f) {
        flux_color bg = lensi_lerp_color(t->color_bg, t->color_hover, fill);
        lensi_drawlist_push(
            ui, n,
            (lens_draw_cmd){
                .kind = LENS_DRAW_RECT, .rel = {0, 0, 0, 0}, .color = bg, .radius = 0.0f});
    }

    /* Left accent bar marks the selected row (the activity-indicator idiom). */
    if (selected)
        lensi_drawlist_push(ui, n,
                            (lens_draw_cmd){.kind = LENS_DRAW_RECT,
                                            .rel = {0, 0, 2.0f, 0},
                                            .color = t->color_accent,
                                            .radius = 0.0f});

    float text_y = (h - tm.height) * 0.5f;
    if (text_y < 0.0f)
        text_y = 0.0f;

    lensi_drawlist_push(
        ui, n,
        (lens_draw_cmd){.kind = LENS_DRAW_TEXT,
                        .rel = {t->padding, text_y, 0, 0}, /* left-aligned (rel.w 0) */
                        .color = disabled   ? t->color_disabled
                                 : selected ? t->color_accent
                                            : t->color_fg,
                        .text = label,
                        .text_size = t->font_size});

    ui->last_response = r;
    return r.clicked;
}

bool lens_selectable(lens *ui, const char *label, bool selected) {
    return selectable_impl(ui, label, selected);
}

lens_response lens_selectable_ex(lens *ui, lens_selectable_opts o) {
    lensi_apply_box(ui, o.box);
    bool scoped = o.box.id && o.box.id[0];
    if (scoped)
        lens_push_id(ui, o.box.id);
    selectable_impl(ui, o.label ? o.label : "", o.selected);
    if (scoped)
        lens_pop_id(ui);
    if (o.box.tooltip)
        lensi_tooltip(ui, o.box.tooltip);
    return ui->last_response;
}
