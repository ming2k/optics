/* radio.c — mutually-exclusive toggle (ADR-0031). */

#include "../internal.h"
#include <math.h>

bool lens_radio(lens *ui, const char *label, int *value, int option_value) {
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

    float font_size = lensi_style_font_size(&eff, t);
    float padding = lensi_style_padding(&eff, t);
    lens_text_metrics tm = lensi_text_measure_label(ui, label, font_size, 0.0f);
    float circle = roundf(font_size);
    float line_h = tm.height > circle ? tm.height : circle;
    float h = line_h + 2.0f * padding;
    float w = 2.0f * padding + circle + (tm.width > 0 ? 6.0f + tm.width : 0.0f);
    if (n->fixed_w > 0)
        w = n->fixed_w;
    if (n->fixed_h > 0)
        h = n->fixed_h;
    n->measured = (flux_point){w, h};

    lens_response r = lensi_interact(ui, n, true, disabled);
    if (r.clicked && value) {
        *value = option_value;
        r.changed = true;
    }
    bool on = value && *value == option_value;
    if (on)
        r.state |= LENS_STATE_SELECTED; /* the picked option (ADR-0058) */

    uint32_t sem_flags = (on ? LENS_A11Y_CHECKED : 0) | (r.focused ? LENS_A11Y_FOCUSED : 0) |
                         (disabled ? LENS_A11Y_DISABLED : 0);
    lensi_node_semantics(ui, n, LENS_ROLE_RADIO, label, NULL, sem_flags);

    float dt = ui->input.dt_seconds;
    if (!disabled)
        n->hover_t = lensi_approach(ui, n->hover_t, r.hovered ? 1.f : 0.f, dt, 12.f);

    /* resolve + emit — through the replaceable skin (ADR-0059) */
    lensi_skin_emit(ui, n,
                    &(lens_widget_record){
                        .kind = LENS_WIDGET_RADIO,
                        .state = r.state,
                        .bounds = {0, 0, w, h},
                        .last_bounds = n->prev_rect,
                        .style = lensi_style_resolve(&eff, t, r.state),
                        .style_fields = eff.fields,
                        .hover_t = n->hover_t,
                        .active_t = n->active_t,
                        .content = {.label = label, .text = tm},
                    });

    ui->last_response = r;
    return r.changed;
}

lens_response lens_radio_ex(lens *ui, lens_radio_opts o) {
    lensi_apply_box(ui, o.box);
    bool scoped = o.box.id && o.box.id[0];
    if (scoped)
        lens_push_id(ui, o.box.id);
    lens_radio(ui, o.label ? o.label : "", o.value, o.option_value);
    if (scoped)
        lens_pop_id(ui);
    if (o.box.tooltip)
        lensi_tooltip(ui, o.box.tooltip);
    return ui->last_response;
}
