/* selectable.c — borderless, full-width selectable row (ADR-0031).
 *
 * A plain list / nav item: transparent at rest, with a subtle fill on hover
 * and a steady highlight when `selected`. By default the selected row is a
 * calm tint-only surface with foreground text — the neutral affordance;
 * decorative accent rails are flavor and belong to caller skins (ADR-0061).
 * In a stretched column the row spans the full cross width, so the whole
 * row is the hit target.
 *
 * Styling flows through the cascade (ADR-0061: per-call box.style > scope
 * > theme); emission is the skin's job (ADR-0059). This file keeps
 * identity, measuring, interaction, animation, and accessibility. The body
 * runs in fixed phases: measure -> interact -> resolve -> emit (skin call).
 */

#include "../internal.h"

static bool selectable_impl(lens *ui, lens_icon_id icon, const char *label, bool selected) {
    const lens_theme *t = &ui->theme;
    bool disabled = ui->next_disabled;
    ui->next_disabled = false;
    ui->next_error = false; /* drain so it never leaks to a later widget */
    lens_style eff = lensi_style_effective(ui);
    lens_id id = lensi_gen_widget_id(ui, label);
    lens_node *n = lensi_store_touch(ui, id);
    if (!n)
        return false;
    lensi_link_child(ui, n);
    n->is_container = false;

    /* measure */
    float font_size = lensi_style_font_size(&eff, t);
    float padding = lensi_style_padding(&eff, t);
    lens_text_metrics tm = lensi_text_measure_label(ui, label, font_size, 0.0f);
    bool has_icon = lensi_icon_valid((int32_t)icon);
    float icon_size = has_icon ? font_size : 0.0f;
    float icon_gap = has_icon ? 8.0f : 0.0f;
    float content_w = icon_size + icon_gap + tm.width;
    float w = (n->fixed_w > 0) ? n->fixed_w : content_w + 2.0f * padding;
    float h = (n->fixed_h > 0) ? n->fixed_h : font_size + 2.0f * padding;
    n->measured = (flux_point){w, h};

    /* interact */
    lens_response r = lensi_interact(ui, n, true, disabled);
    if (selected)
        r.state |= LENS_STATE_SELECTED;
    uint32_t sem_flags = (r.focused ? LENS_A11Y_FOCUSED : 0) | (disabled ? LENS_A11Y_DISABLED : 0) |
                         (selected ? LENS_A11Y_CHECKED : 0);
    lensi_node_semantics(ui, n, LENS_ROLE_BUTTON, label, NULL, sem_flags);

    if (!disabled)
        n->hover_t = r.hovered ? 1.f : 0.f;

    /* resolve */
    lens_style_resolved rs = lensi_style_resolve(&eff, t, r.state);

    /* emit — through the replaceable skin */
    lensi_skin_emit(ui, n,
                    &(lens_widget_record){
                        .kind = LENS_WIDGET_SELECTABLE,
                        .state = r.state,
                        .bounds = {0, 0, w, h},
                        .last_bounds = n->prev_rect,
                        .style = rs,
                        .style_fields = eff.fields,
                        .hover_t = n->hover_t,
                        .active_t = n->active_t,
                        .content = {.label = label, .text = tm, .icon = icon},
                    });

    ui->last_response = r;
    return r.clicked;
}

bool lens_selectable(lens *ui, const char *label, bool selected) {
    /* LENS_ICON_INVALID is the no-icon sentinel: LENS_ICON_COUNT itself is a
     * valid id once a runtime icon has been registered. */
    return selectable_impl(ui, LENS_ICON_INVALID, label, selected);
}

bool lens_selectable_icon(lens *ui, lens_icon_id icon, const char *label, bool selected) {
    return selectable_impl(ui, icon, label, selected);
}

lens_response lens_selectable_ex(lens *ui, lens_selectable_opts o) {
    lensi_apply_box(ui, o.box);
    bool scoped = o.box.id && o.box.id[0];
    if (scoped)
        lens_push_id(ui, o.box.id);
    selectable_impl(ui, LENS_ICON_INVALID, o.label ? o.label : "", o.selected);
    if (scoped)
        lens_pop_id(ui);
    if (o.box.tooltip)
        lensi_tooltip(ui, o.box.tooltip);
    return ui->last_response;
}
