/* checkbox.c — boolean toggle widget (checkbox, switch, radio) */

#include "../internal.h"
#include <math.h>

lens_response lens_checkbox(lens *ui, const lens_checkbox_opts *opts) {
    lens_checkbox_opts default_opts = {0};
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
    lens_text_metrics tm = {0};
    if (label[0])
        tm = lensi_text_measure_label(ui, label, font_size, 0.0f);

    float box_w = font_size;
    if (opts->appearance == LENS_CHECKBOX_SWITCH) {
        box_w = 38.0f;
    }

    float line_h = tm.height > box_w ? tm.height : box_w;
    float h = (n->fixed_h > 0) ? n->fixed_h : (line_h + 2.0f * padding);
    float w = (n->fixed_w > 0) ? n->fixed_w
                               : (2.0f * padding + box_w + (tm.width > 0 ? 8.0f + tm.width : 0.0f));
    n->measured = (flux_point){w, h};

    lens_response r = lensi_interact(ui, n, true, disabled);
    bool *value = opts->value;
    if (r.clicked && value) {
        if (opts->appearance == LENS_CHECKBOX_RADIO) {
            if (!*value) {
                *value = true;
                r.changed = true;
            }
        } else {
            *value = !*value;
            r.changed = true;
        }
    }

    bool on = value && *value;
    if (on) {
        r.state |= LENS_STATE_ACTIVE;
        if (opts->appearance == LENS_CHECKBOX_RADIO)
            r.state |= LENS_STATE_SELECTED;
    }

    uint32_t sem_flags = (on ? LENS_A11Y_CHECKED : 0) | (r.focused ? LENS_A11Y_FOCUSED : 0) |
                         (disabled ? LENS_A11Y_DISABLED : 0);
    lens_role role =
        (opts->appearance == LENS_CHECKBOX_RADIO) ? LENS_ROLE_RADIO : LENS_ROLE_CHECKBOX;
    lensi_node_semantics(ui, n, role, label, NULL, sem_flags);

    float dt = ui->input.dt_seconds;
    if (!disabled) {
        n->hover_t = lensi_approach(ui, n->hover_t, r.hovered ? 1.0f : 0.0f, dt, 12.0f);
        n->active_t =
            lensi_approach(ui, n->active_t, (ui->active_id == id || on) ? 1.0f : 0.0f, dt, 18.0f);
    }

    lensi_skin_emit(ui, n,
                    &(lens_widget_record){
                        .kind = LENS_WIDGET_CHECKBOX,
                        .state = r.state,
                        .bounds = {0, 0, w, h},
                        .last_bounds = n->prev_rect,
                        .style = lensi_style_resolve(ui, &eff, t, r.state),
                        .style_fields = eff.fields,
                        .hover_t = n->hover_t,
                        .active_t = n->active_t,
                        .content =
                            {
                                .label = label,
                                .text = tm,
                                .appearance = opts->appearance,
                            },
                    });

    ui->last_response = r;
    return r;
}
