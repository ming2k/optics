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

/* The band a widget effectively lives in: the band of its nearest ABS
 * ancestor (itself included), else BASE for ordinary flow widgets
 * (ADR-0060). Also reports whether that ancestor is a hit-transparent
 * BACKDROP node (non-interactive by default; ADR-0060 item 7). */
static lens_band node_effective_band(const lens_node *n, bool *hit_transparent,
                                     const lens_node **abs_ancestor) {
    for (const lens_node *p = n; p; p = p->parent) {
        if (p->place == LENS_PLACE_ABS) {
            *hit_transparent = p->band == LENS_BAND_BACKDROP && !p->interactive;
            *abs_ancestor = p;
            return p->band;
        }
    }
    *hit_transparent = false;
    *abs_ancestor = NULL;
    return LENS_BAND_BASE;
}

/* Occlusion IS the hit-test order (ADR-0060 item 4): the global emission
 * order, reversed. A widget is occluded when the cursor sits inside a node
 * that emits ABOVE it — a strictly higher band, or the SAME band with a
 * strictly greater registration index (intra-band z is sibling insertion
 * order; a submenu registered after its parent menu covers the item it
 * overlaps, and the parent never covers the submenu back). Geometry comes
 * from the per-band prev lists (last frame's buckets, carried across the
 * arena reset), so a popup occludes no matter where in this frame's build
 * order it re-registers. */
bool lensi_widget_occluded(const lens *ui, const lens_node *n) {
    if (!ui || !n)
        return false;
    bool hit_transparent = false;
    const lens_node *ancestor = NULL;
    lens_band band = node_effective_band(n, &hit_transparent, &ancestor);
    if (hit_transparent)
        return true; /* a default BACKDROP subtree swallows no hits at all */

    /* Same-band order threshold: only nodes registered strictly AFTER the
     * widget's ABS ancestor occlude it; the ancestor's own index never
     * occludes its contents. When the ancestor is absent from the prev
     * list (first frame it appears, or prev-list truncation) treat it as
     * LAST: nothing in the band is newer, so nothing same-band occludes —
     * matching the one-frame latency every hit-test already has. */
    uint32_t same_band_start = 0;
    if (ancestor) {
        uint32_t index = ui->prev_band_counts[band]; /* fallback: "last" */
        for (uint32_t i = 0; i < ui->prev_band_counts[band]; i++) {
            if (ui->prev_band_ids[band][i] == ancestor->id) {
                index = i;
                break;
            }
        }
        same_band_start = index + 1;
    }

    for (lens_band above = band; above < LENS_BAND_COUNT; above++) {
        uint32_t start = (above == band) ? same_band_start : 0;
        for (uint32_t i = start; i < ui->prev_band_counts[above]; i++) {
            lens_node *m = lensi_store_find(ui, ui->prev_band_ids[above][i]);
            if (!m || !m->has_prev)
                continue;
            if (m->band == LENS_BAND_BACKDROP && !m->interactive)
                continue;
            if (lensi_point_in(ui->input.cursor, m->prev_rect))
                return true;
        }
    }
    return false;
}

bool lensi_point_clipped_by_scroll(const lens_node *n, flux_point p) {
    for (const lens_node *a = n ? n->parent : NULL; a; a = a->parent) {
        /* An ABS node escapes every ancestor clip (ADR-0060): scroll
         * viewports above the nearest ABS ancestor do not clip it, so the
         * walk stops there. Scroll containers *inside* the placed subtree
         * (a dropdown's option list) still clip their own children. */
        if (a->place == LENS_PLACE_ABS)
            return false;
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
