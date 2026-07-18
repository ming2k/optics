/* switch.c — full-width settings row with a trailing boolean switch. */

#include "../internal.h"

static bool switch_impl(lens *ui, const char *label, const char *description, bool *value) {
    const lens_theme *t = &ui->theme;
    bool disabled = ui->next_disabled;
    ui->next_disabled = false;
    ui->next_error = false;

    lens_id id = lensi_gen_widget_id(ui, label);
    lens_node *n = lensi_store_touch(ui, id);
    if (!n)
        return false;
    lensi_link_child(ui, n);
    n->is_container = false;

    const float track_w = 38.0f;
    const float track_h = 22.0f;
    const float knob_pad = 2.0f;
    const float knob = track_h - 2.0f * knob_pad;
    const float desc_size = t->font_size * 0.84f;
    const float text_gap = 3.0f;
    bool has_description = description && description[0];

    lens_text_metrics label_tm = lensi_text_measure_label(ui, label, t->font_size, 0.0f);
    lens_text_metrics desc_tm = has_description
                                    ? lensi_text_measure_label(ui, description, desc_size, 0.0f)
                                    : (lens_text_metrics){0};
    float text_h = label_tm.height + (has_description ? text_gap + desc_tm.height : 0.0f);
    float content_h = text_h > track_h ? text_h : track_h;
    float h = content_h + 2.0f * t->padding;
    float text_w = label_tm.width > desc_tm.width ? label_tm.width : desc_tm.width;
    float w = 3.0f * t->padding + text_w + track_w;
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

    if (n->hover_t > 0.001f) {
        lensi_drawlist_push(
            ui, n,
            (lens_draw_cmd){.kind = LENS_DRAW_RECT,
                            .rel = {0, 0, 0, 0},
                            .color = lensi_lerp_color(t->color_bg, t->color_hover, n->hover_t),
                            .radius = t->corner_radius});
    }

    float text_y = (n->measured.y - text_h) * 0.5f;
    flux_color label_color = disabled ? t->color_disabled : t->color_fg;
    lensi_drawlist_push(ui, n,
                        (lens_draw_cmd){.kind = LENS_DRAW_TEXT,
                                        .rel = {t->padding, text_y, 0, 0},
                                        .color = label_color,
                                        .text = label,
                                        .text_size = t->font_size});
    if (has_description) {
        lensi_drawlist_push(
            ui, n,
            (lens_draw_cmd){.kind = LENS_DRAW_TEXT,
                            .rel = {t->padding, text_y + label_tm.height + text_gap, 0, 0},
                            .color = t->color_disabled,
                            .text = description,
                            .text_size = desc_size});
    }

    float track_y = (n->measured.y - track_h) * 0.5f;
    flux_color track_off = lensi_lerp_color(t->color_border, t->color_fg, 0.18f);
    flux_color track_color =
        disabled ? t->color_disabled : lensi_lerp_color(track_off, t->color_accent, n->active_t);
    lensi_drawlist_push(ui, n,
                        (lens_draw_cmd){.kind = LENS_DRAW_RECT,
                                        .rel = {-t->padding, track_y, track_w, track_h},
                                        .color = track_color,
                                        .radius = track_h * 0.5f});

    float travel = track_w - 2.0f * knob_pad - knob;
    float knob_right = t->padding + knob_pad + (1.0f - n->active_t) * travel;
    flux_color knob_color = disabled ? t->color_bg : t->color_fg;
    lensi_drawlist_push(ui, n,
                        (lens_draw_cmd){.kind = LENS_DRAW_RECT,
                                        .rel = {-knob_right, track_y + knob_pad, knob, knob},
                                        .color = knob_color,
                                        .radius = knob * 0.5f});

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
