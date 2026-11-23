/* button.c — unified button widget */

#include "../internal.h"

optics_ui_response optics_ui_button(optics_ui *ui, const optics_ui_button_opts *opts) {
    optics_ui_button_opts default_opts = {};
    if (!opts)
        opts = &default_opts;

    optics_ui__apply_box(ui, opts->box);
    const optics_ui_theme *t = &ui->theme;
    bool disabled = ui->next_disabled;
    for (optics_ui_node *parent = optics_ui__open_container(ui); parent; parent = parent->parent)
        disabled = disabled || parent->box_disabled;
    ui->next_disabled = false;
    ui->next_error = false;

    optics_ui_style eff = optics_ui__style_effective(ui);
    const char *label = opts->label ? opts->label : "";
    optics_ui_id id = opts->box.id ? optics_ui__gen_widget_id(ui, opts->box.id)
                                   : optics_ui__gen_widget_id(ui, label);
    optics_ui_node *n = optics_ui__store_touch(ui, id);
    if (!n)
        return (optics_ui_response){};

    optics_ui__link_child(ui, n);
    optics_ui__node_box(ui, n, &opts->box);
    n->is_container = false;

    float font_size = optics_ui__style_font_size(ui, &eff, t);
    float padding = optics_ui__style_padding(&eff, t);
    optics_ui_text_metrics tm = {};
    if (label[0])
        tm = optics_ui__text_measure_label(ui, label, font_size, 0.0f);

    float glyph_size = font_size;
    bool has_icon = opts->icon != OPTICS_UI_ICON_INVALID && opts->icon != 0;
    bool has_image = opts->image != nullptr;
    bool has_label = label[0] != '\0';

    float w = 0;
    float h = 0;
    if (n->fixed_w > 0) {
        w = n->fixed_w;
    } else {
        if (opts->variant == OPTICS_UI_BUTTON_LINK) {
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
        if (opts->variant == OPTICS_UI_BUTTON_LINK) {
            h = tm.height + 4.0f;
        } else {
            h = font_size + 2.0f * padding;
        }
    }
    n->measured = (optics_gfx_point){w, h};

    optics_ui_response r = optics_ui__interact(ui, n, true, disabled);
    if (opts->mouse_button == OPTICS_UI_MOUSE_RIGHT) {
        r.clicked = r.right_clicked;
    } else if (opts->mouse_button == OPTICS_UI_MOUSE_MIDDLE) {
        r.clicked = r.middle_clicked;
    }

    uint32_t sem_flags =
        (r.focused ? OPTICS_UI_A11Y_FOCUSED : 0) | (disabled ? OPTICS_UI_A11Y_DISABLED : 0);
    optics_ui_role role =
        (opts->variant == OPTICS_UI_BUTTON_LINK) ? OPTICS_UI_ROLE_LINK : OPTICS_UI_ROLE_BUTTON;
    optics_ui__node_semantics(ui, n, role, label, nullptr, sem_flags);

    float dt = ui->input.dt_seconds;
    if (!disabled) {
        n->hover_t = optics_ui__approach(
            ui, n->hover_t,
            (r.hovered || (opts->variant == OPTICS_UI_BUTTON_LINK && r.focused)) ? 1.0f : 0.0f, dt,
            12.0f);
        n->active_t = optics_ui__approach(
            ui, n->active_t, (ui->active_id == id || opts->active) ? 1.0f : 0.0f, dt, 18.0f);
    }

    optics_ui_style_resolved rs = optics_ui__style_resolve(ui, &eff, t, r.state);

    optics_ui__skin_emit(ui, n,
                         &(optics_ui_widget_record){
                             .kind = OPTICS_UI_WIDGET_BUTTON,
                             .state = r.state | (opts->active ? OPTICS_UI_STATE_ACTIVE : 0),
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

    if (opts->active)
        r.state |= OPTICS_UI_STATE_ACTIVE;
    ui->last_response = r;
    return r;
}
