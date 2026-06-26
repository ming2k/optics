/* checkbox.c — boolean toggle with a label (ADR-0008). */

#include "../internal.h"

bool lens_checkbox(lens *ui, const char *label, bool *value) {
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
    /* Box is a font-size square so it stays visible even with no caption
     * (e.g. "##id" labels used for form layout). */
    float box = t->font_size;
    float line_h = tm.height > box ? tm.height : box;
    float h = line_h + 2.0f * t->padding;
    float w = 2.0f * t->padding + box + (tm.width > 0 ? 6.0f + tm.width : 0.0f);
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
    if (!disabled) {
        n->hover_t = lensi_approach(ui, n->hover_t, r.hovered ? 1.f : 0.f, dt, 12.f);
        n->active_t = lensi_approach(ui, n->active_t, (ui->active_id == id) ? 1.f : 0.f, dt, 18.f);
    }

    float box_y = (n->measured.y - box) * 0.5f;

    flux_color box_border =
        disabled ? t->color_disabled
                 : (on ? t->color_accent
                       : lensi_lerp_color(t->color_border, t->color_hover, n->hover_t));

    /* box background — always fill so the border has something to contrast
     * against, especially on dark cards in light mode. */
    lensi_drawlist_push(ui, n,
                        (lens_draw_cmd){.kind = LENS_DRAW_RECT,
                                        .rel = {t->padding, box_y, box, box},
                                        .color = t->color_bg,
                                        .radius = 3.0f});

    if (on)
        lensi_drawlist_push(
            ui, n,
            (lens_draw_cmd){.kind = LENS_DRAW_RECT,
                            .rel = {t->padding + 3.0f, box_y + 3.0f, box - 6.0f, box - 6.0f},
                            .color = disabled ? t->color_disabled : t->color_accent,
                            .radius = 2.0f});

    /* box border */
    lensi_drawlist_push(ui, n,
                        (lens_draw_cmd){.kind = LENS_DRAW_BORDER,
                                        .rel = {t->padding, box_y, box, box},
                                        .color = box_border,
                                        .radius = 3.0f,
                                        .width = t->border_width});

    lensi_drawlist_push(ui, n,
                        (lens_draw_cmd){.kind = LENS_DRAW_TEXT,
                                        .rel = {t->padding + box + 6.0f, t->padding, 0, 0},
                                        .color = t->color_fg,
                                        .text = label,
                                        .text_size = t->font_size});

    ui->last_response = r;
    return r.changed;
}

lens_response lens_checkbox_ex(lens *ui, lens_checkbox_opts o) {
    lensi_apply_box(ui, o.box);
    bool scoped = o.box.id && o.box.id[0];
    if (scoped)
        lens_push_id(ui, o.box.id);
    lens_checkbox(ui, o.label ? o.label : "", o.value);
    if (scoped)
        lens_pop_id(ui);
    if (o.box.tooltip)
        lensi_tooltip(ui, o.box.tooltip);
    return ui->last_response;
}
