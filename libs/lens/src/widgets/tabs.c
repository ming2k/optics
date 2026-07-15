/* tabs.c — horizontal tab bar (ADR-0008). */

#include "../internal.h"

typedef struct lens_tabs_state {
    int *active;
    int next_index;
} lens_tabs_state;

bool lens_tabs_begin(lens *ui, const char *id, int *active_tab) {
    const lens_theme *t = &ui->theme;
    lens_id fid = lensi_gen_widget_id(ui, id);
    lens_node *n = lensi_store_touch(ui, fid);
    if (!n)
        return false;
    lensi_link_child(ui, n);
    n->is_container = true;
    n->axis = LENS_ROW;
    /* The strip itself owns no inset: tab hit targets must occupy the full
     * solved strip height. Padding belongs inside each tab. Keeping it on the
     * parent used to make a host-specified strip height shorter than its
     * children, separating text, focus outline, and underline. */
    n->gap = t->gap * 0.5f;
    n->pad = 0.0f;

    lens_tabs_state *ts = lens_node_state(n, sizeof *ts);
    if (ts) {
        ts->active = active_tab;
        ts->next_index = 0;
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

    lens_id id = lensi_gen_widget_id(ui, label);
    lens_node *n = lensi_store_touch(ui, id);
    if (!n)
        return false;
    lensi_link_child(ui, n);
    n->is_container = false;

    lens_text_metrics tm = lensi_text_measure_label(ui, label, t->font_size, 0.0f);
    float w = (n->fixed_w > 0) ? n->fixed_w : tm.width + 2.0f * t->padding;
    float natural_h = tm.height + 2.0f * t->padding;
    float h = (n->fixed_h > 0) ? fmaxf(n->fixed_h, natural_h) : natural_h;
    n->measured = (flux_point){w, h};

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

    uint32_t sem_flags = (r.focused ? LENS_A11Y_FOCUSED : 0) |
                         (disabled ? LENS_A11Y_DISABLED : 0) |
                         (active ? LENS_A11Y_CHECKED : 0);
    lensi_node_semantics(ui, n, LENS_ROLE_RADIO, label, NULL, sem_flags);

    flux_color bg = active ? lensi_lerp_color(t->color_bg, t->color_hover, 0.5f)
                           : lensi_lerp_color(t->color_bg, t->color_hover, n->hover_t);

    lensi_drawlist_push(
        ui, n,
        (lens_draw_cmd){
            .kind = LENS_DRAW_RECT, .rel = {0, 0, 0, 0}, .color = bg, .radius = t->corner_radius});

    lensi_drawlist_push(ui, n,
                        (lens_draw_cmd){.kind = LENS_DRAW_TEXT,
                                        .rel = {t->padding, (h - tm.height) * 0.5f, -1.0f, 0},
                                        .color = active ? t->color_accent : t->color_fg,
                                        .text = label,
                                        .text_size = t->font_size});

    /* The underline is both selection and keyboard-focus feedback. It keeps
     * the control legible without drawing a detached outline around the
     * previous-frame hit rectangle. */
    if (active || r.focused)
        lensi_drawlist_push(ui, n,
                            (lens_draw_cmd){.kind = LENS_DRAW_RECT,
                                            .rel = {0, n->measured.y - 2.0f, 0, 2.0f},
                                            .color = t->color_accent,
                                            .radius = 1.0f});

    ui->last_response = r;
    return changed;
}

void lens_tabs_end(lens *ui) {
    lens_node *tabs = lensi_open_container(ui);
    if (tabs) {
        float minimum_h = 0.0f;
        for (lens_node *child = tabs->first_child; child; child = child->next_sibling) {
            if (child->measured.y > minimum_h)
                minimum_h = child->measured.y;
        }
        minimum_h += 2.0f * tabs->pad;
        /* Keep a host height hint when it is useful, but never let it clip
         * the tab label or detach the selection indicator from the hit area. */
        if (tabs->fixed_h > 0.0f && tabs->fixed_h < minimum_h)
            tabs->fixed_h = minimum_h;
    }
    lensi_open_container_pop(ui);
}
