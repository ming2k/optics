/* selectable.c — selectable list/tree row item */

#include "../internal.h"

optics_ui_response optics_ui_selectable(optics_ui *ui, const optics_ui_selectable_opts *opts) {
    optics_ui_selectable_opts default_opts = {};
    if (!opts)
        opts = &default_opts;

    lensi_apply_box(ui, opts->box);
    const optics_ui_theme *t = &ui->theme;
    bool disabled = ui->next_disabled;
    for (optics_ui_node *parent = lensi_open_container(ui); parent; parent = parent->parent)
        disabled = disabled || parent->box_disabled;
    ui->next_disabled = false;
    ui->next_error = false;

    optics_ui_style eff = lensi_style_effective(ui);
    const char *label = opts->label ? opts->label : "";
    optics_ui_id id =
        opts->box.id ? lensi_gen_widget_id(ui, opts->box.id) : lensi_gen_widget_id(ui, label);
    optics_ui_node *n = lensi_store_touch(ui, id);
    if (!n)
        return (optics_ui_response){};

    lensi_link_child(ui, n);
    lensi_node_box(ui, n, &opts->box);
    n->is_container = false;

    float font_size = lensi_style_font_size(ui, &eff, t);
    float padding = lensi_style_padding(&eff, t);
    optics_ui_text_metrics tm = lensi_text_measure_label(ui, label, font_size, 0.0f);

    optics_ui_response r = lensi_interact(ui, n, true, disabled);
    if (opts->selected)
        r.state |= OPTICS_UI_STATE_SELECTED;

    optics_ui_style_resolved rs = lensi_style_resolve(ui, &eff, t, r.state);
    float resolved_icon_size = opts->icon_size > 0.0f ? opts->icon_size : rs.icon_size;
    bool has_icon = opts->icon != 0 && lensi_icon_valid((int32_t)opts->icon);
    float icon_size = has_icon ? resolved_icon_size : 0.0f;
    float icon_gap = has_icon ? (rs.gap > 0.0f ? rs.gap : 8.0f) : 0.0f;
    float content_w = icon_size + icon_gap + tm.width;
    float w = (n->fixed_w > 0) ? n->fixed_w : content_w + 2.0f * padding;
    float h = (n->fixed_h > 0) ? n->fixed_h : font_size + 2.0f * padding;
    n->measured = (optics_gfx_point){w, h};

    uint32_t sem_flags = (r.focused ? OPTICS_UI_A11Y_FOCUSED : 0) |
                         (disabled ? OPTICS_UI_A11Y_DISABLED : 0) |
                         (opts->selected ? OPTICS_UI_A11Y_CHECKED : 0);
    lensi_node_semantics(ui, n, OPTICS_UI_ROLE_BUTTON, label, nullptr, sem_flags);

    if (!disabled) {
        n->hover_t = r.hovered ? 1.0f : 0.0f;
        if (r.hovered && !opts->pointer_cursor) {
            ui->cursor_hint = OPTICS_UI_CURSOR_DEFAULT;
        }
    }

    lensi_skin_emit(ui, n,
                    &(optics_ui_widget_record){
                        .kind = OPTICS_UI_WIDGET_SELECTABLE,
                        .state = r.state,
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
                                .glyph_size = icon_size,
                            },
                    });

    ui->last_response = r;
    return r;
}
