/* input.c — interaction resolved against last frame's geometry (ADR-0029). */

#include "../internal.h"

/* Append a focusable id to this frame's tab order (arena-backed). */
static void tab_push(lens *ui, lens_id id) {
    if (ui->tab_count == ui->tab_cap) {
        uint32_t nc = ui->tab_cap ? ui->tab_cap * 2 : 16;
        lens_id *na = flux_arena_alloc(&ui->arena, nc * sizeof *na);
        if (!na) {
            lensi_set_overflow(ui);
            return;
        }
        if (ui->tab_order)
            memcpy(na, ui->tab_order, ui->tab_count * sizeof *na);
        ui->tab_order = na;
        ui->tab_cap = nc;
    }
    ui->tab_order[ui->tab_count++] = id;
}

/* True if `n` is itself, or sits beneath, an overlay layer (ADR-0037).
 * Used to skip the popup eclipse for the overlay's own contents. */
static bool node_inside_overlay(const lens_node *n) {
    for (const lens_node *p = n; p; p = p->parent)
        if (p->is_overlay)
            return true;
    return false;
}

bool lensi_widget_eclipsed(const lens *ui, const lens_node *n) {
    if (!ui || !n)
        return false;
    return !node_inside_overlay(n) && lensi_point_in_floating_layer(ui, ui->input.cursor);
}

bool lensi_point_clipped_by_scroll(const lens_node *n, flux_point p) {
    for (const lens_node *a = n ? n->parent : NULL; a; a = a->parent) {
        if (!a->is_scroll || !a->has_prev)
            continue;
        /* Mirror the render clip (replay.c): prev_rect inset by pad,
         * minus the scrollbar gutter on the cross axis. */
        float pad = a->pad;
        float viewport_w = a->prev_rect.w - 2.0f * pad - a->scroll_gutter;
        flux_rect viewport = {a->prev_rect.x + pad, a->prev_rect.y + pad, viewport_w,
                              a->prev_rect.h - 2.0f * pad};
        if (!lensi_point_in(p, viewport))
            return true;
    }
    return false;
}

/* Compute hover/press/click/focus for a widget using its prev_rect
 * (last frame's final_rect). New widgets (no prev_rect) report nothing
 * this frame — the documented one-frame latency. */
lens_response lensi_interact(lens *ui, lens_node *n, bool focusable, bool disabled) {
    lens_response r = {0};
    r.id = n->id;
    r.rect = n->prev_rect;

    if (disabled)
        return r;

    if (focusable)
        tab_push(ui, n->id);

    bool inside = n->has_prev && lensi_point_in(ui->input.cursor, n->prev_rect) &&
                  !lensi_point_clipped_by_scroll(n, ui->input.cursor);
    /* Eclipse: a floating layer (overlay or persistent panel) above
     * (last-frame geometry) swallows hover/press for any base widget
     * under it (ADR-0037). The layer's own contents are exempt. */
    if (inside && lensi_widget_eclipsed(ui, n))
        inside = false;
    if (inside) {
        r.hovered = true;
        ui->hot_id = n->id; /* later widgets (drawn on top) win */
        if (focusable)
            ui->cursor_hint = LENS_CURSOR_POINTER;
    }

    const int L = LENS_MOUSE_LEFT;
    if (ui->active_id == n->id) {
        r.pressed = ui->input.mouse_down[L];
        if (ui->input.mouse_released[L]) {
            if (inside)
                r.clicked = true;
            ui->active_id = 0;
        }
    } else if (inside && ui->input.mouse_pressed[L]) {
        ui->active_id = n->id;
        if (focusable) {
            ui->focused_id = n->id;
            ui->click_hit_focusable = true;
        }
        r.pressed = true;
    }

    /* Right / middle button: simple click, no drag capture (v0.1).
     * The application reads these from lens_get_response() for context
     * menus and auxiliary actions. */
    const int R = LENS_MOUSE_RIGHT;
    if (inside && ui->input.mouse_released[R])
        r.right_clicked = true;

    const int M = LENS_MOUSE_MIDDLE;
    if (inside && ui->input.mouse_released[M])
        r.middle_clicked = true;

    r.focused = focusable && ui->focused_id == n->id;
    return r;
}
