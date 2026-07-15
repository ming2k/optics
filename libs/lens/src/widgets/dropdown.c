/* dropdown.c — select widget with overlay item list (ADR-0014). */

#include "../internal.h"
#include <stdio.h>
#include <string.h>

static bool nearest_scroll_bounds(lens_node *n, flux_rect *out) {
    for (lens_node *parent = n ? n->parent : NULL; parent; parent = parent->parent) {
        if (parent->is_scroll && parent->has_prev && parent->prev_rect.w > 0.0f &&
            parent->prev_rect.h > 0.0f) {
            if (out)
                *out = parent->prev_rect;
            return true;
        }
    }
    return false;
}

bool lens_dropdown(lens *ui, const char *label, int *selected, const char **items, int count) {
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

    /* Derive a separate id for the overlay so it doesn't share the same
     * node as the trigger button (which would corrupt prev_rect). */
    char ov_label[64];
    int nwritten = snprintf(ov_label, sizeof(ov_label), "%s##ov", label);
    if (nwritten < 0 || (size_t)nwritten >= sizeof(ov_label)) {
        size_t l = strlen(label);
        if (l > sizeof(ov_label) - 5)
            l = sizeof(ov_label) - 5;
        memcpy(ov_label, label, l);
        memcpy(ov_label + l, "##ov", 5);
    }

    bool open = lens_overlay_is_open(ui, ov_label);
    /* A popup is anchored to content coordinates. Close it at the start of a
     * scroll gesture so the list never lags behind or crosses the owner's
     * viewport while the trigger is moving. */
    if (open && (fabsf(ui->input.scroll_x) > 0.0001f || fabsf(ui->input.scroll_y) > 0.0001f)) {
        lens_overlay_close(ui, ov_label);
        open = false;
    }
    lens_response r = lensi_interact(ui, n, true, disabled);
    if (r.clicked) {
        if (open) {
            lens_overlay_close(ui, ov_label);
            open = false;
        } else {
            lens_overlay_open(ui, ov_label);
            open = true;
        }
    }

    const char *preview =
        (selected && *selected >= 0 && *selected < count) ? items[*selected] : label;

    lens_text_metrics tm = lensi_text_measure_label(ui, preview, t->font_size, 0.0f);
    float icon_size = t->font_size;
    float icon_gap = 8.0f;
    float content_w = tm.width + icon_gap + icon_size;
    float natural_h = fmaxf(tm.height, icon_size) + 2.0f * t->padding;
    /* A select trigger must not accept a box hint that clips its label or
     * disclosure icon. Width remains host-controlled; height has a semantic
     * minimum derived from the current theme and text metrics. */
    if (n->fixed_h > 0.0f && n->fixed_h < natural_h)
        n->fixed_h = natural_h;
    float w = (n->fixed_w > 0) ? n->fixed_w : content_w + 2.0f * t->padding;
    float h = (n->fixed_h > 0) ? fmaxf(n->fixed_h, natural_h) : natural_h;
    n->measured = (flux_point){w, h};

    float dt = ui->input.dt_seconds;
    if (!disabled) {
        n->hover_t = lensi_approach(ui, n->hover_t, r.hovered ? 1.f : 0.f, dt, 12.f);
        n->active_t = lensi_approach(ui, n->active_t, (ui->active_id == id) ? 1.f : 0.f, dt, 18.f);
    }

    float emphasis = (open || r.focused) ? 0.72f : n->hover_t;
    flux_color bg = disabled ? t->color_disabled
                             : lensi_lerp_color(t->color_bg, t->color_hover, emphasis);

    lensi_drawlist_push(
        ui, n,
        (lens_draw_cmd){
            .kind = LENS_DRAW_RECT, .rel = {0, 0, 0, 0}, .color = bg, .radius = t->corner_radius});

    lensi_drawlist_push(ui, n,
                        (lens_draw_cmd){.kind = LENS_DRAW_TEXT,
                                        .rel = {t->padding, (h - tm.height) * 0.5f, 0, 0},
                                        .color = t->color_fg,
                                        .text = preview,
                                        .text_size = t->font_size});

    float icon_y = (h - icon_size) * 0.5f;
    lensi_drawlist_push(
        ui, n,
        (lens_draw_cmd){.kind = LENS_DRAW_ICON,
                        .rel = {-t->padding, icon_y, icon_size, icon_size},
                        .color = (open || r.focused) ? t->color_accent : t->color_fg,
                        .width = 1.8f * (icon_size / 24.0f),
                        .icon_id = open ? LENS_ICON_CHEVRON_UP : LENS_ICON_CHEVRON_DOWN});

    lensi_drawlist_push(ui, n,
                        (lens_draw_cmd){.kind = LENS_DRAW_BORDER,
                                        .rel = {0, 0, 0, 0},
                                        .color = t->color_border,
                                        .radius = t->corner_radius,
                                        .width = t->border_width});

    uint32_t sem_flags = (r.focused ? LENS_A11Y_FOCUSED : 0) |
                         (disabled ? LENS_A11Y_DISABLED : 0) |
                         (open ? LENS_A11Y_EXPANDED : 0);
    lensi_node_semantics(ui, n, LENS_ROLE_BUTTON, label, preview, sem_flags);

    bool changed = false;

    if (open) {
        /* Keyboard navigation inside the open dropdown */
        for (uint32_t i = 0; i < ui->input.key_count; i++) {
            const lens_key_event *k = &ui->input.keys[i];
            if (!k->pressed)
                continue;
            if (k->key == LENS_KEY_DOWN) {
                if (selected && *selected + 1 < count) {
                    *selected = *selected + 1;
                    changed = true;
                }
            } else if (k->key == LENS_KEY_UP) {
                if (selected && *selected > 0) {
                    *selected = *selected - 1;
                    changed = true;
                }
            } else if (k->key == LENS_KEY_ESCAPE) {
                lens_overlay_close(ui, ov_label);
            }
        }

        flux_rect anchor = n->has_prev ? n->prev_rect : (flux_rect){0, 0, w, h};
        /* Match the popup width to the trigger's on-screen width (anchor.w),
         * not the trigger's intrinsic text width: a host that stretches the
         * dropdown to fill its row should get a full-width menu, not one
         * shrink-wrapped to "17 ▼". */
        float popup_w = anchor.w;
        flux_rect owner_bounds = {0, 0, 0, 0};
        bool has_owner_bounds = nearest_scroll_bounds(n, &owner_bounds);
        /* Use the opaque theme background instead of color_hover. Hover can
         * legitimately be translucent, but a floating option surface must
         * never reveal or visually merge with content behind it. */
        if (lens_overlay_begin(ui, ov_label, anchor,
                               (lens_overlay_opts){.pad = t->padding,
                                                   .gap = 2.0f,
                                                   .bg = t->color_bg | 0xff000000u,
                                                   .border = t->color_border,
                                                   .border_width = t->border_width,
                                                   .radius = t->corner_radius,
                                                   .min_width = popup_w,
                                                   .cross = LENS_STRETCH})) {
            if (has_owner_bounds)
                lensi_overlay_constrain_current(ui, owner_bounds);
            /* Flat selectable rows, not filled accent buttons: a column of
             * lens_button reads as a stack of bordered pills, whereas a menu
             * wants one flat list with the current value highlighted. */
            for (int i = 0; i < count; i++) {
                bool is_sel = selected && i == *selected;
                if (lens_selectable(ui, items[i], is_sel)) {
                    if (selected) {
                        *selected = i;
                        changed = true;
                    }
                    lens_overlay_close(ui, ov_label);
                }
            }
            lens_overlay_end(ui);
        }
    }

    ui->last_response = r;
    return changed;
}

lens_response lens_dropdown_ex(lens *ui, lens_dropdown_opts o) {
    lensi_apply_box(ui, o.box);
    bool scoped = o.box.id && o.box.id[0];
    if (scoped)
        lens_push_id(ui, o.box.id);
    lens_dropdown(ui, o.label ? o.label : "", o.selected, o.items, o.count);
    if (scoped)
        lens_pop_id(ui);
    if (o.box.tooltip)
        lensi_tooltip(ui, o.box.tooltip);
    return ui->last_response;
}
