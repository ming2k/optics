/* dropdown.c — select widget with a placed popup item list (ADR-0060). */

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
    lens_style eff = lensi_style_effective(ui);
    float font_size = lensi_style_font_size(&eff, t);
    float padding = lensi_style_padding(&eff, t);

    lens_id id = lensi_gen_widget_id(ui, label);
    lens_node *n = lensi_store_touch(ui, id);
    if (!n)
        return false;
    lensi_link_child(ui, n);
    n->is_container = false;

    /* Derive a separate id for the popup so it doesn't share the same
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

    bool open = lens_place_is_open(ui, ov_label);
    /* A popup is anchored to content coordinates. A wheel gesture away from
     * the popup scrolls the owner, so close the popup before its anchor can
     * leave the viewport; a wheel over the popup itself drives the popup's
     * own list and must not close it. */
    if (open && (fabsf(ui->input.scroll_x) > 0.0001f || fabsf(ui->input.scroll_y) > 0.0001f ||
                 fabsf(ui->input.scroll_pixels_x) > 0.0001f ||
                 fabsf(ui->input.scroll_pixels_y) > 0.0001f)) {
        lens_node *ov = lensi_store_find(ui, lens_current_id(ui, ov_label));
        bool over_popup = ov && ov->has_prev && lensi_point_in(ui->input.cursor, ov->prev_rect);
        if (!over_popup) {
            lens_place_close(ui, ov_label);
            open = false;
        }
    }
    lens_response r = lensi_interact(ui, n, true, disabled);
    lens_style_resolved rs = lensi_style_resolve(&eff, t, r.state);
    bool just_opened = false;
    if (r.clicked) {
        if (open) {
            lens_place_close(ui, ov_label);
            open = false;
        } else {
            lens_place_open(ui, ov_label);
            open = true;
            just_opened = true;
        }
    }

    const char *preview =
        (selected && *selected >= 0 && *selected < count) ? items[*selected] : label;

    lens_text_metrics tm = lensi_text_measure_label(ui, preview, font_size, 0.0f);
    float icon_size = font_size;
    float icon_gap = 8.0f;
    float content_w = tm.width + icon_gap + icon_size;
    float natural_h = fmaxf(tm.height, icon_size) + 2.0f * padding;
    /* A select trigger must not accept a box hint that clips its label or
     * disclosure icon. Width remains host-controlled; height has a semantic
     * minimum derived from the current theme and text metrics. */
    if (n->fixed_h > 0.0f && n->fixed_h < natural_h)
        n->fixed_h = natural_h;
    float w = (n->fixed_w > 0) ? n->fixed_w : content_w + 2.0f * padding;
    float h = (n->fixed_h > 0) ? fmaxf(n->fixed_h, natural_h) : natural_h;
    n->measured = (flux_point){w, h};

    float dt = ui->input.dt_seconds;
    if (!disabled) {
        n->hover_t = lensi_approach(ui, n->hover_t, r.hovered ? 1.f : 0.f, dt, 12.f);
        n->active_t = lensi_approach(ui, n->active_t, (ui->active_id == id) ? 1.f : 0.f, dt, 18.f);
    }

    /* emit — through the replaceable skin (ADR-0059). The popup (placement,
     * option list, dismissal) stays below in the widget: place machinery
     * and cascade-styled containers, not chrome. */
    lensi_skin_emit(ui, n,
                    &(lens_widget_record){
                        .kind = LENS_WIDGET_DROPDOWN,
                        .state = r.state,
                        .bounds = {0, 0, w, h},
                        .last_bounds = n->prev_rect,
                        .style = rs,
                        .style_fields = eff.fields,
                        .hover_t = n->hover_t,
                        .active_t = n->active_t,
                        .content = {.label = preview,
                                    .text = tm,
                                    .icon = open ? LENS_ICON_CHEVRON_UP : LENS_ICON_CHEVRON_DOWN,
                                    .popup_open = open},
                    });

    uint32_t sem_flags = (r.focused ? LENS_A11Y_FOCUSED : 0) | (disabled ? LENS_A11Y_DISABLED : 0) |
                         (open ? LENS_A11Y_EXPANDED : 0);
    lensi_node_semantics(ui, n, LENS_ROLE_BUTTON, label, preview, sem_flags);

    bool changed = false;

    if (open) {
        /* Keyboard navigation inside the open dropdown. Escape is NOT
         * handled here: the central dismissal pass owns the open-set
         * (ADR-0060/C1) — a widget-side close would double-close. Arrow
         * keys a menu already consumed this frame are skipped. */
        bool kbd_nav = false;
        for (uint32_t i = 0; i < ui->input.key_count; i++) {
            const lens_key_event *k = &ui->input.keys[i];
            if (!k->pressed || ui->key_consumed[i])
                continue;
            if (k->key == LENS_KEY_DOWN) {
                if (selected && *selected + 1 < count) {
                    *selected = *selected + 1;
                    changed = true;
                    kbd_nav = true;
                }
            } else if (k->key == LENS_KEY_UP) {
                if (selected && *selected > 0) {
                    *selected = *selected - 1;
                    changed = true;
                    kbd_nav = true;
                }
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

        /* The placement area the ANCHORED resolve will use: the owner's
         * scroll viewport clamped to the display, else the whole display. */
        float dw = ui->input.display_size.x;
        float dh = ui->input.display_size.y;
        flux_rect area = {0.0f, 0.0f, dw, dh};
        if (has_owner_bounds) {
            float right = dw > 0.0f ? fminf(dw, owner_bounds.x + owner_bounds.w)
                                    : owner_bounds.x + owner_bounds.w;
            float bottom = dh > 0.0f ? fminf(dh, owner_bounds.y + owner_bounds.h)
                                     : owner_bounds.y + owner_bounds.h;
            area.x = fmaxf(0.0f, owner_bounds.x);
            area.y = fmaxf(0.0f, owner_bounds.y);
            area.w = fmaxf(0.0f, right - area.x);
            area.h = fmaxf(0.0f, bottom - area.y);
        }

        /* Height budget: never taller than the roomier side of the trigger
         * (so the capped popup always opens clear of it — below when it
         * fits, flipped above otherwise), and never taller than a peek of
         * ~7 rows so a long list scrolls instead of blanketing the owner.
         * Row height mirrors lens_selectable. */
        const float edge_margin = 4.0f;
        float room_below = area.y + area.h - (anchor.y + anchor.h) - edge_margin;
        float room_above = anchor.y - area.y - edge_margin;
        float row_h = font_size + 2.0f * padding;
        float list_gap = 2.0f;
        float peek = 7.0f * row_h + 6.0f * list_gap + 0.5f * row_h;
        float budget = fmaxf(room_below, room_above);
        if (budget > peek)
            budget = peek;
        float list_max_h = budget - 2.0f * padding; /* popup pad */
        if (list_max_h < row_h)
            list_max_h = row_h; /* always show at least one row */

        /* Use the opaque theme background instead of color_hover. Hover can
         * legitimately be translucent, but a floating option surface must
         * never reveal or visually merge with content behind it. The popup
         * is constrained to the owner's scroll viewport so it escapes the
         * ordinary layout flow without crossing the inspector that owns it. */
        if (lens_place_begin(
                ui, ov_label,
                (lens_place_opts){
                    .band = LENS_BAND_POPUP,
                    .mode = LENS_PLACE_ANCHORED,
                    .rect = anchor,
                    .bounds = has_owner_bounds ? owner_bounds : (flux_rect){0, 0, 0, 0},
                    .transient = true,
                    .layout =
                        {
                            .pad = padding,
                            .gap = list_gap,
                            .bg = rs.bg | 0xff000000u,
                            .border = rs.border,
                            .border_width = rs.border_width,
                            .radius = rs.corner_radius,
                            .min_width = popup_w,
                            .cross = LENS_STRETCH,
                        },
                })) {
            /* Flat selectable rows, not filled accent buttons: a column of
             * lens_button reads as a stack of bordered pills, whereas a menu
             * wants one flat list with the current value highlighted. The
             * list scrolls once it outgrows the height budget. */
            lens_scroll_begin(ui, "##list");
            lens_node *list = lensi_open_container(ui);
            if (list) {
                list->max_h = list_max_h;
                list->gap = list_gap; /* scroll defaults to theme gap */
            }
            for (int i = 0; i < count; i++) {
                bool is_sel = selected && i == *selected;
                /* Items with duplicate labels must still get distinct nodes;
                 * scope each row by its index (same-label siblings would
                 * otherwise collapse into one). */
                lens_push_id_int(ui, i);
                bool clicked = lens_selectable(ui, items[i], is_sel);
                lens_pop_id(ui);
                if (clicked) {
                    if (selected) {
                        *selected = i;
                        changed = true;
                    }
                    lens_place_close(ui, ov_label);
                }
            }
            /* Keep the selected row in view: on open, and after keyboard
             * navigation. Manual wheel scrolling is left untouched. */
            if (list && selected && *selected >= 0 && *selected < count &&
                (just_opened || kbd_nav)) {
                lens_scroll_state *ss =
                    (lens_scroll_state *)lens_node_state(list, sizeof(lens_scroll_state));
                if (ss) {
                    float viewport_h =
                        fminf((float)count * row_h + (float)(count - 1) * list_gap, list_max_h);
                    float row_top = (float)*selected * (row_h + list_gap);
                    if (just_opened)
                        ss->offset_y = 0.0f;
                    if (row_top < ss->offset_y)
                        ss->offset_y = row_top;
                    else if (row_top + row_h > ss->offset_y + viewport_h)
                        ss->offset_y = row_top + row_h - viewport_h;
                    list->scroll_y = ss->offset_y;
                }
            }
            lens_scroll_end(ui);
            lens_place_end(ui);
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
