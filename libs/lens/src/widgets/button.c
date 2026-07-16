/* button.c — filled button (ADR-0008). */

#include "../internal.h"

bool lens_button(lens *ui, const char *label) {
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
    uint32_t sem_flags = (r.focused ? LENS_A11Y_FOCUSED : 0) | (disabled ? LENS_A11Y_DISABLED : 0);
    lensi_node_semantics(ui, n, LENS_ROLE_BUTTON, label, NULL, sem_flags);

    float dt = ui->input.dt_seconds;
    if (!disabled) {
        n->hover_t = lensi_approach(ui, n->hover_t, r.hovered ? 1.f : 0.f, dt, 12.f);
        n->active_t = lensi_approach(ui, n->active_t, (ui->active_id == id) ? 1.f : 0.f, dt, 18.f);
    }

    flux_color bg =
        disabled
            ? t->color_disabled
            : lensi_lerp_color(t->color_accent, t->color_active,
                               n->active_t > n->hover_t * 0.4f ? n->active_t : n->hover_t * 0.4f);

    float text_y = (h - tm.height) * 0.5f;
    if (text_y < 0.0f)
        text_y = 0.0f;

    lensi_drawlist_push(
        ui, n,
        (lens_draw_cmd){
            .kind = LENS_DRAW_RECT, .rel = {0, 0, 0, 0}, .color = bg, .radius = t->corner_radius});

    lensi_drawlist_push(
        ui, n,
        (lens_draw_cmd){.kind = LENS_DRAW_TEXT,
                        .rel = {t->padding, text_y, -1.0f, 0},
                        .color = disabled ? t->color_fg : flux_color_rgba(0xff, 0xff, 0xff, 0xff),
                        .text = label,
                        .text_size = t->font_size});

    if (r.focused)
        lensi_drawlist_push(ui, n,
                            (lens_draw_cmd){.kind = LENS_DRAW_BORDER,
                                            .rel = {0, 0, 0, 0},
                                            .color = t->color_fg,
                                            .radius = t->corner_radius,
                                            .width = t->border_width});

    ui->last_response = r;
    return r.clicked;
}

bool lens_link(lens *ui, const char *label) {
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

    lens_text_metrics tm = lensi_text_measure_label(ui, label, t->font_size, 0.0f);
    float w = n->fixed_w > 0.0f ? n->fixed_w : tm.width;
    float h = n->fixed_h > 0.0f ? n->fixed_h : tm.height + 6.0f;
    n->measured = (flux_point){w, h};

    lens_response r = lensi_interact(ui, n, true, disabled);
    uint32_t sem_flags = (r.focused ? LENS_A11Y_FOCUSED : 0) | (disabled ? LENS_A11Y_DISABLED : 0);
    lensi_node_semantics(ui, n, LENS_ROLE_BUTTON, label, NULL, sem_flags);

    if (!disabled)
        n->hover_t = lensi_approach(ui, n->hover_t, (r.hovered || r.focused) ? 1.0f : 0.0f,
                                    ui->input.dt_seconds, 18.0f);

    float text_y = fmaxf((h - tm.height) * 0.5f - 1.0f, 0.0f);
    flux_color fg = disabled ? t->color_disabled
                             : lensi_lerp_color(t->color_fg, t->color_accent, n->hover_t * 0.35f);
    lensi_drawlist_push(ui, n,
                        (lens_draw_cmd){.kind = LENS_DRAW_TEXT,
                                        .rel = {0.0f, text_y, 0.0f, 0.0f},
                                        .color = fg,
                                        .text = label,
                                        .text_size = t->font_size,
                                        .text_weight = 0.0f});

    if (n->hover_t > 0.001f) {
        float underline_w = tm.width * n->hover_t;
        float underline_y = fminf(text_y + tm.height + 2.0f, h - 1.5f);
        lensi_drawlist_push(ui, n,
                            (lens_draw_cmd){.kind = LENS_DRAW_RECT,
                                            .rel = {0.0f, underline_y, underline_w, 1.5f},
                                            .color = t->color_accent,
                                            .radius = 0.75f});
    }

    ui->last_response = r;
    return r.clicked;
}

lens_response lens_button_ex(lens *ui, lens_button_opts o) {
    lensi_apply_box(ui, o.box);
    bool scoped = o.box.id && o.box.id[0];
    if (scoped)
        lens_push_id(ui, o.box.id);
    lens_button(ui, o.label ? o.label : "");
    if (scoped)
        lens_pop_id(ui);
    if (o.box.tooltip)
        lensi_tooltip(ui, o.box.tooltip);
    return ui->last_response;
}
