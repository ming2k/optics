/* switch.c — full-width settings row with a trailing boolean switch. */

#include "../internal.h"

static bool switch_impl(lens *ui, const char *label, const char *description, bool *value) {
    const lens_theme *t = &ui->theme;
    bool disabled = ui->next_disabled;
    ui->next_disabled = false;
    ui->next_error = false;
    lens_style eff = lensi_style_effective(ui);

    lens_id id = lensi_gen_widget_id(ui, label);
    lens_node *n = lensi_store_touch(ui, id);
    if (!n)
        return false;
    lensi_link_child(ui, n);
    n->is_container = false;

    const float track_w = 38.0f;
    const float track_h = 22.0f;
    float font_size = lensi_style_font_size(ui, &eff, t);
    float padding = lensi_style_padding(&eff, t);
    const float desc_size = font_size * 0.84f;
    const float text_gap = 3.0f;
    bool has_description = description && description[0];

    lens_text_metrics label_tm = lensi_text_measure_label(ui, label, font_size, 0.0f);
    lens_text_metrics desc_tm = has_description
                                    ? lensi_text_measure_label(ui, description, desc_size, 0.0f)
                                    : (lens_text_metrics){0};
    float text_h = label_tm.height + (has_description ? text_gap + desc_tm.height : 0.0f);
    float content_h = text_h > track_h ? text_h : track_h;
    float h = content_h + 2.0f * padding;
    float text_w = label_tm.width > desc_tm.width ? label_tm.width : desc_tm.width;
    float w = 3.0f * padding + text_w + track_w;
    if (n->fixed_w > 0)
        w = n->fixed_w;
    if (n->fixed_h > 0)
        h = n->fixed_h;
    n->measured = (flux_point){w, h};

    lens_response r = lensi_interact(ui, n, true, disabled);
    if (r.clicked && value) {
        *value = !*value;
        r.changed = true;
    }
    bool on = value && *value;
    if (on)
        r.state |= LENS_STATE_ACTIVE; /* toggle on-state (ADR-0058) */
    uint32_t sem_flags = (on ? LENS_A11Y_CHECKED : 0) | (r.focused ? LENS_A11Y_FOCUSED : 0) |
                         (disabled ? LENS_A11Y_DISABLED : 0);
    lensi_node_semantics(ui, n, LENS_ROLE_CHECKBOX, label, NULL, sem_flags);

    float dt = ui->input.dt_seconds;
    if (disabled) {
        n->hover_t = 0.0f;
    } else {
        n->hover_t = lensi_approach(ui, n->hover_t, r.hovered ? 1.0f : 0.0f, dt, 12.0f);
    }
    n->active_t = lensi_approach(ui, n->active_t, on ? 1.0f : 0.0f, dt, 18.0f);

    /* resolve + emit — through the replaceable skin (ADR-0059) */
    lensi_skin_emit(ui, n,
                    &(lens_widget_record){
                        .kind = LENS_WIDGET_SWITCH,
                        .state = r.state,
                        .bounds = {0, 0, w, h},
                        .last_bounds = n->prev_rect,
                        .style = lensi_style_resolve(ui, &eff, t, r.state),
                        .style_fields = eff.fields,
                        .hover_t = n->hover_t,
                        .active_t = n->active_t,
                        .content = {.label = label,
                                    .text = label_tm,
                                    .description = description,
                                    .desc_text = desc_tm},
                    });

    ui->last_response = r;
    return r.changed;
}

bool lens_switch(lens *ui, const char *label, bool *value) {
    return switch_impl(ui, label, NULL, value);
}

lens_response lens_switch_ex(lens *ui, lens_switch_opts o) {
    lensi_apply_box(ui, o.box);
    bool scoped = o.box.id && o.box.id[0];
    if (scoped)
        lens_push_id(ui, o.box.id);
    switch_impl(ui, o.label ? o.label : "", o.description, o.value);
    if (scoped)
        lens_pop_id(ui);
    if (o.box.tooltip)
        lensi_tooltip(ui, o.box.tooltip);
    return ui->last_response;
}
