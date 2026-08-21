/* button.c — filled button (ADR-0031). Style cascade: ADR-0058/0061.
 * Emission lives in the skin (ADR-0059): this file keeps identity,
 * measuring, interaction, animation, and accessibility, then packs a
 * record and calls the skin. */

#include "../internal.h"

/* The widget body runs in fixed phases (ADR-0058): measure through the
 * text seam -> interact (state bits) -> resolve the style (pure) -> emit.
 * The style is the cascade-effective one (ADR-0061: per-call box.style >
 * scope > theme); with nothing set anywhere the built-in default skin
 * renders the themed default, pixel-identical. */
static bool button_ex_impl(lens *ui, const char *label, lens_response *out) {
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

    /* measure — geometry slots are state-independent, so they come straight
     * from the shared fallback (cascade wins, else theme). */
    float font_size = lensi_style_font_size(&eff, t);
    float padding = lensi_style_padding(&eff, t);
    lens_text_metrics tm = lensi_text_measure_label(ui, label, font_size, 0.0f);
    float w = (n->fixed_w > 0) ? n->fixed_w : tm.width + 2.0f * padding;
    float h = (n->fixed_h > 0) ? n->fixed_h : font_size + 2.0f * padding;
    n->measured = (flux_point){w, h};

    /* interact */
    lens_response r = lensi_interact(ui, n, true, disabled);
    uint32_t sem_flags = (r.focused ? LENS_A11Y_FOCUSED : 0) | (disabled ? LENS_A11Y_DISABLED : 0);
    lensi_node_semantics(ui, n, LENS_ROLE_BUTTON, label, NULL, sem_flags);

    float dt = ui->input.dt_seconds;
    if (!disabled) {
        n->hover_t = lensi_approach(ui, n->hover_t, r.hovered ? 1.f : 0.f, dt, 12.f);
        n->active_t = lensi_approach(ui, n->active_t, (ui->active_id == id) ? 1.f : 0.f, dt, 18.f);
    }

    /* resolve */
    lens_style_resolved rs = lensi_style_resolve(&eff, t, r.state);

    /* emit — through the replaceable skin */
    lensi_skin_emit(ui, n,
                    &(lens_widget_record){
                        .kind = LENS_WIDGET_BUTTON,
                        .state = r.state,
                        .bounds = {0, 0, w, h},
                        .last_bounds = n->prev_rect,
                        .style = rs,
                        .style_fields = eff.fields,
                        .hover_t = n->hover_t,
                        .active_t = n->active_t,
                        .content = {.label = label, .text = tm},
                    });

    ui->last_response = r;
    if (out)
        *out = r;
    return r.clicked;
}

static bool button_impl(lens *ui, const char *label) {
    return button_ex_impl(ui, label, NULL);
}

bool lens_button(lens *ui, const char *label) {
    return button_impl(ui, label);
}

bool lens_button_mouse(lens *ui, const char *label, int button) {
    /* Click variants for widgets where secondary buttons carry meaning
     * (palette slots: right-click re-colours). The interaction layer has
     * always tracked them (lens_response); only the button wrapper
     * forced them to LEFT. */
    lens_response r;
    button_ex_impl(ui, label, &r);
    switch (button) {
    case LENS_MOUSE_RIGHT:
        return r.right_clicked;
    case LENS_MOUSE_MIDDLE:
        return r.middle_clicked;
    default:
        return r.clicked;
    }
}

bool lens_link(lens *ui, const char *label) {
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

    float font_size = lensi_style_font_size(&eff, t);
    lens_text_metrics tm = lensi_text_measure_label(ui, label, font_size, 0.0f);
    float w = n->fixed_w > 0.0f ? n->fixed_w : tm.width;
    float h = n->fixed_h > 0.0f ? n->fixed_h : tm.height + 6.0f;
    n->measured = (flux_point){w, h};

    lens_response r = lensi_interact(ui, n, true, disabled);
    uint32_t sem_flags = (r.focused ? LENS_A11Y_FOCUSED : 0) | (disabled ? LENS_A11Y_DISABLED : 0);
    lensi_node_semantics(ui, n, LENS_ROLE_LINK, label, NULL, sem_flags);

    if (!disabled)
        n->hover_t = lensi_approach(ui, n->hover_t, (r.hovered || r.focused) ? 1.0f : 0.0f,
                                    ui->input.dt_seconds, 18.0f);

    lens_style_resolved rs = lensi_style_resolve(&eff, t, r.state);

    /* emit — through the replaceable skin (ADR-0059) */
    lensi_skin_emit(ui, n,
                    &(lens_widget_record){
                        .kind = LENS_WIDGET_LINK,
                        .state = r.state,
                        .bounds = {0, 0, w, h},
                        .last_bounds = n->prev_rect,
                        .style = rs,
                        .style_fields = eff.fields,
                        .hover_t = n->hover_t,
                        .active_t = n->active_t,
                        .content = {.label = label, .text = tm},
                    });

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
