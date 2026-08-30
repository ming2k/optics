/* button.c — unified button widget */

#include "../internal.h"

lens_response lens_button(lens *ui, const lens_button_opts *opts) {
    lens_button_opts default_opts = {0};
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

    float glyph_size = font_size;
    bool has_icon = opts->icon != LENS_ICON_INVALID && opts->icon != 0;
    bool has_image = opts->image != NULL;
    bool has_label = label[0] != '\0';

    float w = 0;
    float h = 0;
    if (n->fixed_w > 0) {
        w = n->fixed_w;
    } else {
        if (opts->variant == LENS_BUTTON_LINK) {
            w = tm.width;
        } else if (has_icon && has_label) {
            w = tm.width + glyph_size + 6.0f + 2.0f * padding;
        } else if (has_icon || has_image) {
            w = glyph_size + 2.0f * padding;
        } else {
            w = tm.width + 2.0f * padding;
        }
    }

    if (n->fixed_h > 0) {
        h = n->fixed_h;
    } else {
        if (opts->variant == LENS_BUTTON_LINK) {
            h = tm.height + 4.0f;
        } else {
            h = font_size + 2.0f * padding;
        }
    }
    n->measured = (flux_point){w, h};

    lens_response r = lensi_interact(ui, n, true, disabled);
    if (opts->mouse_button == LENS_MOUSE_RIGHT) {
        r.clicked = r.right_clicked;
    } else if (opts->mouse_button == LENS_MOUSE_MIDDLE) {
        r.clicked = r.middle_clicked;
    }

    uint32_t sem_flags = (r.focused ? LENS_A11Y_FOCUSED : 0) | (disabled ? LENS_A11Y_DISABLED : 0);
    lens_role role = (opts->variant == LENS_BUTTON_LINK) ? LENS_ROLE_LINK : LENS_ROLE_BUTTON;
    lensi_node_semantics(ui, n, role, label, NULL, sem_flags);

    float dt = ui->input.dt_seconds;
    if (!disabled) {
        n->hover_t = lensi_approach(
            ui, n->hover_t,
            (r.hovered || (opts->variant == LENS_BUTTON_LINK && r.focused)) ? 1.0f : 0.0f, dt,
            12.0f);
        n->active_t = lensi_approach(
            ui, n->active_t, (ui->active_id == id || opts->active) ? 1.0f : 0.0f, dt, 18.0f);
    }

    lens_style_resolved rs = lensi_style_resolve(ui, &eff, t, r.state);

    lensi_skin_emit(ui, n,
                    &(lens_widget_record){
                        .kind = LENS_WIDGET_BUTTON,
                        .state = r.state | (opts->active ? LENS_STATE_ACTIVE : 0),
                        .bounds = {0, 0, w, h},
                        .last_bounds = n->prev_rect,
                        .style = rs,
                        .style_fields = eff.fields,
                        .hover_t = n->hover_t,
                        .active_t = n->active_t,
                        .content =
                            {
                                .label = label,
                                .text = tm,
                                .icon = opts->icon,
                                .glyph_size = glyph_size,
                                .image = opts->image,
                                .variant = opts->variant,
                            },
                    });

    ui->last_response = r;
    return r;
}
