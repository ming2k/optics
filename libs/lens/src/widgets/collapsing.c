/* collapsing.c — expandable header panel (ADR-0008). */

#include "../internal.h"

bool lens_collapsing(lens *ui, const char *label) {
    const lens_theme *t = &ui->theme;
    bool disabled = ui->next_disabled;
    ui->next_disabled = false;
    ui->next_error = false; /* drain so it never leaks to a later widget */
    lens_id id = lensi_gen_widget_id(ui, label);
    lens_node *n = lensi_store_touch(ui, id);
    if (!n)
        return false;
    lensi_link_child(ui, n);

    float label_size = t->font_size * 0.88f;
    lens_text_metrics tm = lensi_text_measure_label(ui, label, label_size, 700.0f);
    float arrow = tm.height;
    float h = tm.height + 2.0f * t->padding;
    float w = 2.0f * t->padding + arrow + 6.0f + tm.width;
    if (n->fixed_w > 0)
        w = n->fixed_w;
    if (n->fixed_h > 0)
        h = n->fixed_h;
    n->measured = (flux_point){w, h};

    /* When expanded the container's prev_rect covers the whole body;
     * limit hit-testing to the header row so nested widgets can
     * receive input. */
    flux_rect saved_rect = n->prev_rect;
    if (n->has_prev)
        n->prev_rect.h = h;
    lens_response r = lensi_interact(ui, n, true, disabled);
    n->prev_rect = saved_rect;

    bool *expanded = (bool *)lens_node_state(n, sizeof(bool));
    if (r.clicked && expanded)
        *expanded = !*expanded;
    bool open = expanded && *expanded;
    uint32_t sem_flags = (open ? LENS_A11Y_EXPANDED : 0) | (r.focused ? LENS_A11Y_FOCUSED : 0) |
                         (disabled ? LENS_A11Y_DISABLED : 0);
    lensi_node_semantics(ui, n, LENS_ROLE_DISCLOSURE, label, NULL, sem_flags);

    float dt = ui->input.dt_seconds;
    if (!disabled) {
        n->hover_t = lensi_approach(ui, n->hover_t, r.hovered ? 1.f : 0.f, dt, 12.f);
        n->active_t = lensi_approach(ui, n->active_t, (ui->active_id == id) ? 1.f : 0.f, dt, 18.f);
    }

    /* header background */
    flux_color bg =
        disabled ? t->color_bg : lensi_lerp_color(t->color_bg, t->color_hover, n->hover_t);
    lensi_drawlist_push(
        ui, n,
        (lens_draw_cmd){
            .kind = LENS_DRAW_RECT, .rel = {0, 0, 0, h}, .color = bg, .radius = t->corner_radius});

    /* SVG-derived chevron disclosure indicator. */
    float arrow_x = t->padding;
    float arrow_y = (h - arrow) * 0.5f;
    flux_color arrow_color =
        disabled ? t->color_disabled : (open ? t->color_accent : t->color_border);
    lensi_drawlist_push(
        ui, n,
        (lens_draw_cmd){.kind = LENS_DRAW_ICON,
                        .rel = {arrow_x + 2.0f, arrow_y + 2.0f, arrow - 4.0f, arrow - 4.0f},
                        .color = arrow_color,
                        .width = 2.0f * ((arrow - 4.0f) / 24.0f),
                        .icon_id = open ? LENS_ICON_CHEVRON_DOWN : LENS_ICON_CHEVRON_RIGHT});

    /* label */
    lensi_drawlist_push(ui, n,
                        (lens_draw_cmd){.kind = LENS_DRAW_TEXT,
                                        .rel = {arrow_x + arrow + 6.0f, t->padding, 0, 0},
                                        .color = t->color_fg,
                                        .text = label,
                                        .text_size = label_size,
                                        .text_weight = 700.0f});

    if (open) {
        n->is_container = true;
        n->axis = LENS_COLUMN;
        n->gap = t->gap;
        n->pad = t->padding;
        lensi_open_container_push(ui, n);

        /* Reserve header height with an invisible spacer child so body
         * widgets start below the header instead of overlapping it. */
        lens_id sid = lensi_gen_widget_id(ui, "##hdr");
        lens_node *spacer = lensi_store_touch(ui, sid);
        if (spacer) {
            lensi_link_child(ui, spacer);
            spacer->is_container = false;
            spacer->measured = (flux_point){0.0f, h};
            spacer->fixed_h = h;
        }
    }

    ui->last_response = r;
    return open;
}

lens_response lens_collapsing_ex(lens *ui, lens_collapsing_opts o) {
    lensi_apply_box(ui, o.box);
    bool scoped = o.box.id && o.box.id[0];
    if (scoped)
        lens_push_id(ui, o.box.id);
    lens_collapsing(ui, o.label ? o.label : "");
    if (scoped)
        lens_pop_id(ui);
    if (o.box.tooltip)
        lensi_tooltip(ui, o.box.tooltip);
    return ui->last_response;
}

void lens_collapsing_set_open(lens *ui, const char *label, bool open) {
    lens_id id = lensi_gen_widget_id(ui, label);
    lens_node *n = lensi_store_touch(ui, id);
    if (!n)
        return;
    /* Only seed the state if the node has never been rendered (no
     * prev_rect). After the first render, lens's retained state holds
     * the user's toggle and we must not clobber it. This lets hosts
     * call set_open every frame without worrying about overriding
     * the user's choices — it's a true "default", applied once on
     * first appearance and never again. */
    if (n->has_prev)
        return;
    bool *expanded = (bool *)lens_node_state(n, sizeof(bool));
    *expanded = open;
}
