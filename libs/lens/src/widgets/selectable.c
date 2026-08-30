/* selectable.c — selectable list/tree row item */

#include "../internal.h"

lens_response lens_selectable(lens *ui, const lens_selectable_opts *opts) {
    lens_selectable_opts default_opts = {0};
    if (!opts)
        opts = &default_opts;

    lensi_apply_box(ui, opts->box);
    const lens_theme *t = &ui->theme;
    bool disabled = ui->next_disabled;
    ui->next_disabled = false;
    ui->next_error = false;

    lens_style eff = lensi_style_effective(ui);
    const char *label = opts->label ? opts->label : "";
    lens_id id =
        opts->box.id ? lensi_gen_widget_id(ui, opts->box.id) : lensi_gen_widget_id(ui, label);
    lens_node *n = lensi_store_touch(ui, id);
    if (!n)
        return (lens_response){0};

    lensi_link_child(ui, n);
    n->is_container = false;

    float font_size = lensi_style_font_size(ui, &eff, t);
    float padding = lensi_style_padding(&eff, t);
    lens_text_metrics tm = lensi_text_measure_label(ui, label, font_size, 0.0f);
    bool has_icon = opts->icon != 0 && lensi_icon_valid((int32_t)opts->icon);
    float icon_size = has_icon ? font_size : 0.0f;
    float icon_gap = has_icon ? 8.0f : 0.0f;
    float content_w = icon_size + icon_gap + tm.width;
    float w = (n->fixed_w > 0) ? n->fixed_w : content_w + 2.0f * padding;
    float h = (n->fixed_h > 0) ? n->fixed_h : font_size + 2.0f * padding;
    n->measured = (flux_point){w, h};

    lens_response r = lensi_interact(ui, n, true, disabled);
    if (opts->selected)
        r.state |= LENS_STATE_SELECTED;

    uint32_t sem_flags = (r.focused ? LENS_A11Y_FOCUSED : 0) | (disabled ? LENS_A11Y_DISABLED : 0) |
                         (opts->selected ? LENS_A11Y_CHECKED : 0);
    lensi_node_semantics(ui, n, LENS_ROLE_BUTTON, label, NULL, sem_flags);

    if (!disabled)
        n->hover_t = r.hovered ? 1.0f : 0.0f;

    lens_style_resolved rs = lensi_style_resolve(ui, &eff, t, r.state);

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
                        .content = {.label = label, .text = tm, .icon = opts->icon},
                    });

    ui->last_response = r;
    return r;
}
