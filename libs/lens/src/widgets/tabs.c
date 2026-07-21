/* tabs.c — horizontal tab bar (ADR-0031). */

#include "../internal.h"

typedef struct lens_tabs_state {
    int *active;
    int next_index;
    lens_tabs_opts opts;
    lens_node *active_node;
    int drawn_active_index;
    float indicator_left;
    float indicator_right;
    float indicator_left_velocity;
    float indicator_right_velocity;
    int indicator_index;
    int indicator_direction;
    bool indicator_seeded;
} lens_tabs_state;

typedef struct lens_tab_item_state {
    float indicator_width;
} lens_tab_item_state;

static bool tab_style_valid(lens_tab_style style) {
    return style == LENS_TAB_STYLE_STANDARD || style == LENS_TAB_STYLE_CONNECTED ||
           style == LENS_TAB_STYLE_INDICATOR;
}

static void tab_indicator_spring(float *position, float *velocity, float target, float stiffness,
                                 float damping, float dt) {
    *velocity += (target - *position) * stiffness * dt;
    *velocity *= expf(-damping * dt);
    *position += *velocity * dt;
}

bool lens_tabs_begin(lens *ui, const char *id, int *active_tab) {
    return lens_tabs_begin_ex(ui, id, active_tab,
                              (lens_tabs_opts){.style = LENS_TAB_STYLE_STANDARD});
}

bool lens_tabs_begin_ex(lens *ui, const char *id, int *active_tab, lens_tabs_opts opts) {
    const lens_theme *t = &ui->theme;
    if (!tab_style_valid(opts.style))
        opts.style = LENS_TAB_STYLE_STANDARD;

    lens_id fid = lensi_gen_widget_id(ui, id);
    lens_node *n = lensi_store_touch(ui, fid);
    if (!n)
        return false;
    lensi_link_child(ui, n);
    n->is_container = true;
    n->axis = LENS_ROW;

    if (opts.style == LENS_TAB_STYLE_CONNECTED) {
        float radius = opts.radius > 0.0f ? opts.radius : t->corner_radius;
        float connector = opts.connector_size > 0.0f ? opts.connector_size
                                                     : fminf(12.0f, fmaxf(6.0f, radius * 0.75f));
        n->gap = 0.0f;
        n->pad = fmaxf(3.0f, t->border_width * 2.0f);

        flux_color rail = opts.rail_color ? opts.rail_color : t->color_bg;
        lensi_drawlist_push(ui, n,
                            (lens_draw_cmd){.kind = LENS_DRAW_RECT,
                                            .rel = {0, 0, 0, 0},
                                            .color = rail,
                                            .radius = radius * 1.25f});

        /* Resolve style defaults once so every tab in the strip shares the
         * same geometry even if callers reuse a zero-initialised descriptor. */
        opts.radius = radius;
        opts.connector_size = connector;
        opts.rail_color = rail;
        if (!opts.active_color)
            opts.active_color = t->color_active;
    } else if (opts.style == LENS_TAB_STYLE_INDICATOR) {
        n->gap = 0.0f;
        n->pad = 0.0f;
        n->cross = LENS_START;
        if (opts.radius <= 0.0f)
            opts.radius = t->corner_radius;
        if (!opts.indicator_color)
            opts.indicator_color = t->color_accent;
        if (opts.indicator_thickness <= 0.0f)
            opts.indicator_thickness = 3.0f;
        if (opts.indicator_gap <= 0.0f)
            opts.indicator_gap = 2.0f;
        if (opts.indicator_padding <= 0.0f)
            opts.indicator_padding = fmaxf(8.0f, t->padding * 0.75f);
        if (opts.rail_color)
            lensi_drawlist_push(ui, n,
                                (lens_draw_cmd){.kind = LENS_DRAW_RECT,
                                                .rel = {0, 0, 0, 0},
                                                .color = opts.rail_color,
                                                .radius = opts.radius});
    } else {
        /* Standard tabs remain the library default and preserve their compact
         * independent hit targets. Presentation variants are opt-in. */
        n->gap = t->gap * 0.5f;
        n->pad = 0.0f;
    }
    if (!opts.hover_color)
        opts.hover_color = t->color_hover;

    lens_tabs_state *ts = lens_node_state(n, sizeof *ts);
    if (ts) {
        if (ts->opts.style != opts.style) {
            ts->indicator_seeded = false;
            ts->indicator_left_velocity = 0.0f;
            ts->indicator_right_velocity = 0.0f;
            ts->indicator_index = -1;
            ts->indicator_direction = 0;
        }
        ts->active = active_tab;
        ts->next_index = 0;
        ts->opts = opts;
        ts->active_node = NULL;
        ts->drawn_active_index = -1;
    }
    lensi_open_container_push(ui, n);
    return true;
}

bool lens_tab(lens *ui, const char *label) {
    const lens_theme *t = &ui->theme;
    bool disabled = ui->next_disabled;
    ui->next_disabled = false;
    ui->next_error = false; /* drain so it never leaks to a later widget */

    lens_node *parent = lensi_open_container(ui);
    lens_tabs_state *ts = parent ? lens_node_state(parent, sizeof *ts) : NULL;
    int index = ts ? ts->next_index++ : 0;
    bool active = ts && ts->active && *ts->active == index;
    lens_tab_style style = ts ? ts->opts.style : LENS_TAB_STYLE_STANDARD;

    lens_id id = lensi_gen_widget_id(ui, label);
    lens_node *n = lensi_store_touch(ui, id);
    if (!n)
        return false;
    lensi_link_child(ui, n);
    n->is_container = false;

    lens_text_metrics tm = lensi_text_measure_label(ui, label, t->font_size, 0.0f);
    float w = (n->fixed_w > 0) ? n->fixed_w : tm.width + 2.0f * t->padding;
    float vertical_padding =
        style == LENS_TAB_STYLE_INDICATOR ? fmaxf(4.0f, t->padding * 0.42f) : t->padding;
    float natural_h = tm.height + 2.0f * vertical_padding;
    float h = (n->fixed_h > 0) ? fmaxf(n->fixed_h, natural_h) : natural_h;
    n->measured = (flux_point){w, h};
    if (style == LENS_TAB_STYLE_INDICATOR) {
        lens_tab_item_state *item = lens_node_state(n, sizeof *item);
        if (item)
            item->indicator_width = fminf(w, tm.width + 2.0f * ts->opts.indicator_padding);
    }

    lens_response r = lensi_interact(ui, n, true, disabled);
    bool changed = false;
    if (r.clicked && ts && ts->active) {
        *ts->active = index;
        changed = true;
    }
    if (r.focused && !disabled) {
        for (uint32_t i = 0; i < ui->input.key_count; i++) {
            int k = ui->input.keys[i].key;
            if (ui->input.keys[i].pressed && (k == LENS_KEY_RETURN || k == ' ')) {
                if (ts && ts->active) {
                    *ts->active = index;
                    changed = true;
                }
            }
        }
    }

    float dt = ui->input.dt_seconds;
    if (!disabled)
        n->hover_t = lensi_approach(ui, n->hover_t, r.hovered ? 1.f : 0.f, dt, 12.f);

    uint32_t sem_flags = (r.focused ? LENS_A11Y_FOCUSED : 0) | (disabled ? LENS_A11Y_DISABLED : 0) |
                         (active ? LENS_A11Y_CHECKED : 0);
    lensi_node_semantics(ui, n, LENS_ROLE_RADIO, label, NULL, sem_flags);

    flux_color fg = disabled ? t->color_disabled : t->color_fg;
    if (style == LENS_TAB_STYLE_INDICATOR) {
        if (!disabled && (n->hover_t > 0.0f || r.focused)) {
            float emphasis = r.focused ? 1.0f : n->hover_t;
            flux_color hover = lensi_lerp_color(
                flux_color_rgba(0, 0, 0, 0), ts ? ts->opts.hover_color : t->color_hover, emphasis);
            lensi_drawlist_push(ui, n,
                                (lens_draw_cmd){.kind = LENS_DRAW_RECT,
                                                .rel = {2.0f, 1.0f, 0, 0},
                                                .color = hover,
                                                .radius = ts ? ts->opts.radius : t->corner_radius});
        }
    } else if (style == LENS_TAB_STYLE_CONNECTED) {
        if (active && ts) {
            uint32_t connectors = index > 0 ? LENSI_TAB_CONNECT_LEFT : 0;
            connectors |= LENSI_TAB_CONNECT_RIGHT; /* cleared at end for the last tab */
            lensi_drawlist_push(ui, n,
                                (lens_draw_cmd){.kind = LENS_DRAW_CONNECTED_TAB,
                                                .rel = {0, 0, 0, 0},
                                                .color = ts->opts.active_color,
                                                .radius = ts->opts.radius,
                                                .width = ts->opts.connector_size,
                                                .text_size = parent ? parent->pad : 0.0f,
                                                .flags = connectors});
            ts->active_node = n;
            ts->drawn_active_index = index;
            fg = disabled ? t->color_disabled
                          : lensi_lerp_color(t->color_fg, t->color_accent, 0.35f);
        } else if (!disabled && (n->hover_t > 0.0f || r.focused)) {
            float emphasis = r.focused ? 1.0f : n->hover_t;
            flux_color hover = lensi_lerp_color(
                flux_color_rgba(0, 0, 0, 0), ts ? ts->opts.hover_color : t->color_hover, emphasis);
            lensi_drawlist_push(ui, n,
                                (lens_draw_cmd){.kind = LENS_DRAW_RECT,
                                                .rel = {0, 0, 0, 0},
                                                .color = hover,
                                                .radius = ts ? ts->opts.radius : t->corner_radius});
        }
    } else {
        flux_color bg = active ? lensi_lerp_color(t->color_bg, t->color_hover, 0.5f)
                               : lensi_lerp_color(t->color_bg, t->color_hover, n->hover_t);
        lensi_drawlist_push(ui, n,
                            (lens_draw_cmd){.kind = LENS_DRAW_RECT,
                                            .rel = {0, 0, 0, 0},
                                            .color = bg,
                                            .radius = t->corner_radius});
        if (active)
            fg = t->color_accent;
        if (active || r.focused)
            lensi_drawlist_push(ui, n,
                                (lens_draw_cmd){.kind = LENS_DRAW_RECT,
                                                .rel = {0, n->measured.y - 2.0f, 0, 2.0f},
                                                .color = t->color_accent,
                                                .radius = 1.0f});
    }

    lensi_drawlist_push(ui, n,
                        (lens_draw_cmd){.kind = LENS_DRAW_TEXT,
                                        .rel = {t->padding, (h - tm.height) * 0.5f, -1.0f, 0},
                                        .color = fg,
                                        .text = label,
                                        .text_size = t->font_size});

    ui->last_response = r;
    return changed;
}

void lens_tabs_end(lens *ui) {
    lens_node *tabs = lensi_open_container(ui);
    if (tabs) {
        lens_tabs_state *ts = lens_node_state(tabs, sizeof *ts);
        if (ts && ts->opts.equal_width && tabs->first_child) {
            /* Equal width is a strip-level layout policy, not a visual style.
             * Normalize the intrinsic bases before the row solver distributes
             * its remaining space; equal flex weights then produce equal hit
             * targets even when labels have different lengths. */
            float equal_base = 0.0f;
            for (lens_node *child = tabs->first_child; child; child = child->next_sibling) {
                float base = child->fixed_w > 0.0f ? child->fixed_w : child->measured.x;
                equal_base = fmaxf(equal_base, base);
            }
            for (lens_node *child = tabs->first_child; child; child = child->next_sibling) {
                child->measured.x = equal_base;
                child->flex_grow = 1.0f;
            }
        }

        if (ts && ts->opts.style == LENS_TAB_STYLE_CONNECTED && ts->active_node && ts->active &&
            ts->drawn_active_index == ts->next_index - 1) {
            for (uint32_t i = 0; i < ts->active_node->cmd_count; i++) {
                lens_draw_cmd *cmd = &ts->active_node->cmds[i];
                if (cmd->kind == LENS_DRAW_CONNECTED_TAB)
                    cmd->flags &= ~LENSI_TAB_CONNECT_RIGHT;
            }
        }

        if (ts && ts->opts.style == LENS_TAB_STYLE_INDICATOR && ts->active && ts->next_index > 0) {
            int selected_index = *ts->active;
            if (selected_index < 0)
                selected_index = 0;
            if (selected_index >= ts->next_index)
                selected_index = ts->next_index - 1;

            lens_node *first = tabs->first_child;
            lens_node *selected = first;
            for (int i = 0; selected && i < selected_index; i++) {
                selected = selected->next_sibling;
            }

            if (selected && first && selected->has_prev && first->has_prev) {
                /* Child rectangles are stable relative to one another even
                 * while an ancestor is moving or entering. Deriving the local
                 * tab box from the first item avoids applying screen-space
                 * offsets a second time at replay. */
                float tab_left = tabs->pad + selected->prev_rect.x - first->prev_rect.x;
                float tab_width = selected->prev_rect.w;
                lens_tab_item_state *item = lens_node_state(selected, sizeof *item);
                float indicator_content_width = item ? item->indicator_width : tab_width;
                indicator_content_width =
                    fminf(fmaxf(indicator_content_width, ts->opts.indicator_thickness), tab_width);
                float target_left = tab_left + (tab_width - indicator_content_width) * 0.5f;
                float target_right = target_left + indicator_content_width;

                float known_width = tabs->has_prev ? tabs->prev_rect.w : tabs->fixed_w;
                if (known_width > 0.0f) {
                    target_left = fminf(fmaxf(target_left, 0.0f), known_width);
                    target_right = fminf(fmaxf(target_right, target_left), known_width);
                }

                if (!ts->indicator_seeded) {
                    ts->indicator_left = target_left;
                    ts->indicator_right = target_right;
                    ts->indicator_left_velocity = 0.0f;
                    ts->indicator_right_velocity = 0.0f;
                    ts->indicator_index = selected_index;
                    ts->indicator_direction = 0;
                    ts->indicator_seeded = true;
                } else {
                    if (selected_index != ts->indicator_index) {
                        ts->indicator_direction = selected_index > ts->indicator_index ? 1 : -1;
                        ts->indicator_index = selected_index;
                        ui->anim_pending = true;
                    }

                    float dt = fminf(fmaxf(ui->input.dt_seconds, 0.0f), 1.0f / 30.0f);
                    float left_stiffness = ts->indicator_direction > 0 ? 220.0f : 480.0f;
                    float right_stiffness = ts->indicator_direction < 0 ? 220.0f : 480.0f;
                    float left_damping = ts->indicator_direction > 0 ? 18.0f : 24.0f;
                    float right_damping = ts->indicator_direction < 0 ? 18.0f : 24.0f;
                    tab_indicator_spring(&ts->indicator_left, &ts->indicator_left_velocity,
                                         target_left, left_stiffness, left_damping, dt);
                    tab_indicator_spring(&ts->indicator_right, &ts->indicator_right_velocity,
                                         target_right, right_stiffness, right_damping, dt);

                    bool moving = fabsf(ts->indicator_left - target_left) > 0.05f ||
                                  fabsf(ts->indicator_right - target_right) > 0.05f ||
                                  fabsf(ts->indicator_left_velocity) > 0.05f ||
                                  fabsf(ts->indicator_right_velocity) > 0.05f;
                    if (moving) {
                        ui->anim_pending = true;
                    } else {
                        ts->indicator_left = target_left;
                        ts->indicator_right = target_right;
                        ts->indicator_left_velocity = 0.0f;
                        ts->indicator_right_velocity = 0.0f;
                        ts->indicator_direction = 0;
                    }
                }

                float indicator_width =
                    fmaxf(ts->opts.indicator_thickness, ts->indicator_right - ts->indicator_left);
                lensi_drawlist_push(
                    ui, tabs,
                    (lens_draw_cmd){.kind = LENS_DRAW_TAB_INDICATOR,
                                    .rel = {ts->indicator_left, 0, indicator_width, 0},
                                    .color = ts->opts.indicator_color,
                                    .width = ts->opts.indicator_thickness});
            } else {
                /* Layout owns the equal/flex widths. Skip the first unresolved
                 * frame instead of guessing a target that could flash outside
                 * the selected tab; request the geometry-ready next frame. */
                ui->anim_pending = true;
            }
        }

        float minimum_h = 0.0f;
        for (lens_node *child = tabs->first_child; child; child = child->next_sibling) {
            if (child->measured.y > minimum_h)
                minimum_h = child->measured.y;
        }
        if (ts && ts->opts.style == LENS_TAB_STYLE_INDICATOR)
            minimum_h += ts->opts.indicator_gap + ts->opts.indicator_thickness;
        minimum_h += 2.0f * tabs->pad;
        if (ts && ts->opts.style == LENS_TAB_STYLE_INDICATOR && tabs->min_h < minimum_h)
            tabs->min_h = minimum_h;
        /* Keep a host height hint when it is useful, but never let it clip
         * the tab label or detach the selection indicator from the hit area. */
        if (tabs->fixed_h > 0.0f && tabs->fixed_h < minimum_h)
            tabs->fixed_h = minimum_h;
    }
    lensi_open_container_pop(ui);
}
