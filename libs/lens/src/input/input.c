/* input.c — interaction resolved against last frame's geometry (ADR-0029). */

#include "../internal.h"

/* Append a focusable id to this frame's tab order (arena-backed).
 * tab_cap survives the per-frame reset (see lens_begin), so the first
 * push of a frame sizes the allocation at last frame's high-water in one
 * step instead of re-walking the doubling chain. */
static void tab_push(lens *ui, lens_id id) {
    if (!ui->tab_order || ui->tab_count == ui->tab_cap) {
        uint32_t nc = ui->tab_cap ? ui->tab_cap : 16;
        if (ui->tab_order && ui->tab_count == nc)
            nc = nc * 2;
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

bool lensi_point_clipped_by_scroll(const lens_node *n, flux_point p) {
    return lensi_snapshot_point_clipped(n, p);
}

/* Compute hover/press/click/focus for a widget using its prev_rect
 * (last frame's final_rect). New widgets (no prev_rect) report nothing
 * this frame — the documented one-frame latency. Also produces the
 * interaction-owned LENS_STATE_* bits (ADR-0058); the widget ORs in the
 * bits only it knows (SELECTED/ACTIVE/DRAGGED) before publishing the
 * response. */
lens_response lensi_interact(lens *ui, lens_node *n, bool focusable, bool disabled) {
    lens_response r = {0};
    r.id = n->id;
    r.rect = n->prev_rect;

    if (disabled) {
        r.state = LENS_STATE_DISABLED;
        return r;
    }

    if (focusable)
        tab_push(ui, n->id);

    /* Assistive-technology activation (ADR-0062): the host's AT bridge
     * asked for this node by id. Fires through the same response path as
     * pointer/keyboard activation, single-shot, regardless of pointer
     * position or occlusion (AT users navigate the semantic tree, not
     * pixels). Focus moves as with a pointer press; the modality is not
     * keyboard traversal, so no focus ring (same as lens_set_focus). The
     * disabled early-return above already blocked disabled nodes. */
    if (ui->a11y_activate_id == n->id) {
        ui->a11y_activate_id = 0; /* consumed: one request fires once */
        if (focusable) {
            r.clicked = true;
            ui->focused_id = n->id;
            ui->click_hit_focusable = true;
            ui->focus_visible = false;
        }
    }

    bool inside = n->has_prev && lensi_point_in(ui->input.cursor, n->prev_rect) &&
                  !lensi_point_clipped_by_scroll(n, ui->input.cursor);
    /* Occlusion: a node in a strictly higher band (last-frame geometry)
     * swallows hover/press for any widget beneath it (ADR-0060). */
    if (inside && lensi_widget_occluded(ui, n))
        inside = false;
    if (inside) {
        r.hovered = true;
        r.state |= LENS_STATE_HOVERED;
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
            /* Pointer focus is not keyboard navigation (ADR-0058). */
            ui->focus_visible = false;
        }
        r.pressed = true;
    }
    if (r.pressed)
        r.state |= LENS_STATE_PRESSED;

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
    if (r.focused) {
        r.state |= LENS_STATE_FOCUSED;
        /* The ring shows for keyboard-traversed focus only (ADR-0058). */
        if (ui->focus_visible)
            r.state |= LENS_STATE_FOCUS_VISIBLE;

        /* Central keyboard activation (ADR-0029): focused + Return/Space
         * clicks. This is the ONLY site — per-widget key loops would
         * double-fire. The key is marked consumed so no later central
         * consumer re-reads it this frame. */
        if (!disabled) {
            for (uint32_t i = 0; i < ui->input.key_count; i++) {
                const lens_key_event *k = &ui->input.keys[i];
                if (!k->pressed || ui->key_consumed[i])
                    continue;
                if (k->key == LENS_KEY_RETURN || k->key == ' ') {
                    r.clicked = true;
                    ui->key_consumed[i] = 1;
                }
            }
        }
    }
    return r;
}

void lens_consume_key(lens *ui, int32_t key) {
    if (!ui)
        return;
    for (uint32_t i = 0; i < ui->input.key_count; i++) {
        if (ui->input.keys[i].key == key) {
            ui->key_consumed[i] = 1;
        }
    }
}
