/* overlay.c — floating layers with z-order and dismissal (ADR-0037).
 *
 * Two sibling primitives share the positioning + rendering machinery in
 * this file:
 *
 *   - Overlays (`lens_overlay_begin`): transient popups — dropdowns,
 *     menus, tooltips, modals. Open state is retained per id; each
 *     frame, lens_overlay_begin enters its body iff the id is currently
 *     open. Dismissal runs at lens_end: Escape closes the top open
 *     overlay; a mouse press outside an open overlay's last-frame rect
 *     closes it, subject to a same-frame-open grace. Placement drops the
 *     layer below its anchor and flips above when out of room.
 *
 *   - Panels (`lens_layer_begin`): persistent chrome — docks, status
 *     bars, notification stacks, per-window title bars. Always rendered,
 *     never dismissed. Placed exactly at the supplied rect (clamped to
 *     the display), no below-anchor drop or flip.
 *
 * Both register a sub-root in `overlay_layers[]`, which the layout pass
 * (lensi_overlay_layout) places and the render pass (lensi_overlay_render)
 * draws above the base tree. Both also eclipse base widgets underneath,
 * so a click on the dock (or a popup) does not also activate the window
 * below it. */

#include "../internal.h"

/* ---- open-set helpers --------------------------------------------- */
/* Tracked only for overlays (dismissal + is_open queries); panels never
 * enter this table. */

static void open_id(lens *ui, lens_id id, bool dismissable); /* fwd */

static int find_open_slot(const lens *ui, lens_id id) {
    for (uint32_t i = 0; i < ui->open_overlay_count; i++)
        if (ui->open_overlays[i].id == id)
            return (int)i;
    return -1;
}

bool lensi_overlay_is_open_id(const lens *ui, lens_id id) {
    return ui && find_open_slot(ui, id) >= 0;
}

/* Open an overlay by a pre-computed lens_id, without the id-generation
 * side effect of lens_overlay_open. Used by menu.c (ADR-0040), which has
 * already derived the trigger id and must not perturb the sibling seq. */
void lensi_overlay_open_id_pub(lens *ui, lens_id id, bool dismissable) {
    if (!ui)
        return;
    open_id(ui, id, dismissable);
}

static void open_id(lens *ui, lens_id id, bool dismissable) {
    if (find_open_slot(ui, id) >= 0)
        return;
    if (ui->open_overlay_count >= LENSI_OVERLAY_MAX) {
        /* drop the oldest to make room; a noisy overlay set is a bug we
         * surface via behaviour rather than a crash */
        for (uint32_t i = 0; i + 1 < ui->open_overlay_count; i++)
            ui->open_overlays[i] = ui->open_overlays[i + 1];
        ui->open_overlay_count--;
    }
    ui->open_overlays[ui->open_overlay_count++] = (struct lens_overlay_slot){
        .id = id,
        .opened_frame = ui->frame,
        .dismissable = dismissable,
    };
}

static void close_id(lens *ui, lens_id id) {
    int i = find_open_slot(ui, id);
    if (i < 0)
        return;
    for (uint32_t j = (uint32_t)i; j + 1 < ui->open_overlay_count; j++)
        ui->open_overlays[j] = ui->open_overlays[j + 1];
    ui->open_overlay_count--;
}

/* ---- public open/close/is_open ------------------------------------ */

void lens_overlay_open(lens *ui, const char *id) {
    if (!ui || !id)
        return;
    open_id(ui, lensi_gen_widget_id(ui, id), true);
}

void lens_overlay_close(lens *ui, const char *id) {
    if (!ui || !id)
        return;
    close_id(ui, lensi_gen_widget_id(ui, id));
}

bool lens_overlay_is_open(const lens *ui, const char *id) {
    if (!ui || !id)
        return false;
    /* gen_widget_id mutates the sibling-seq counter; compute the id
     * inside the current scope without that side-effect by using
     * lens_current_id, which is stable per call. */
    return lensi_overlay_is_open_id(ui, lens_current_id(ui, id));
}

bool lens_overlay_hovered(const lens *ui, const char *id) {
    if (!ui || !id)
        return false;
    lens_id overlay_id = lens_current_id(ui, id);
    if (!lensi_overlay_is_open_id(ui, overlay_id))
        return false;
    lens_node *n = lensi_store_find(ui, overlay_id);
    return n && n->has_prev && lensi_point_in(ui->input.cursor, n->prev_rect);
}

/* ---- shared sub-root registration --------------------------------- */
/* Stages a floating layer node: container flags, anchor, fixed_w from
 * min_width, and registers it in this frame's overlay_layers[] so the
 * layout and render passes pick it up. Returns the staged node, or NULL
 * on arena overflow. Caller has already verified the enter condition
 * (open state for overlays; always for panels). */
static lens_node *register_floating_layer(lens *ui, lens_id id, flux_rect anchor,
                                          lens_overlay_opts opts) {
    lens_node *n = lensi_store_touch(ui, id);
    if (!n)
        return NULL;

    n->is_container = true;
    n->is_overlay = true;
    n->axis = LENS_COLUMN;
    n->gap = opts.gap;
    n->pad = opts.pad;
    n->cross = opts.cross;
    n->overlay_anchor = anchor;
    if (opts.min_width > 0)
        n->fixed_w = opts.min_width;

    /* register this frame's active layer (arena-grown) */
    if (ui->overlay_layer_count == ui->overlay_layer_cap) {
        uint32_t nc = ui->overlay_layer_cap ? ui->overlay_layer_cap * 2 : 4;
        lens_node **na = flux_arena_alloc(&ui->arena, nc * sizeof *na);
        if (!na) {
            ui->overflow = true;
            return NULL;
        }
        if (ui->overlay_layers)
            memcpy(na, ui->overlay_layers, ui->overlay_layer_count * sizeof *ui->overlay_layers);
        ui->overlay_layers = na;
        ui->overlay_layer_cap = nc;
    }
    ui->overlay_layers[ui->overlay_layer_count++] = n;
    return n;
}

/* Push the background/border draw cmds that frame every floating layer. */
static void layer_paint_bg(lens *ui, lens_node *n, lens_overlay_opts opts) {
    lensi_drawlist_push(
        ui, n,
        (lens_draw_cmd){
            .kind = LENS_DRAW_RECT, .rel = {0, 0, 0, 0}, .color = opts.bg, .radius = opts.radius});
    if ((opts.border >> 24) && opts.border_width > 0.0f) {
        lensi_drawlist_push(ui, n,
                            (lens_draw_cmd){.kind = LENS_DRAW_BORDER,
                                            .rel = {0, 0, 0, 0},
                                            .color = opts.border,
                                            .radius = opts.radius,
                                            .width = opts.border_width});
    }
}

/* ---- overlay begin / end ------------------------------------------ */

bool lens_overlay_begin(lens *ui, const char *id_str, flux_rect anchor, lens_overlay_opts opts) {
    if (!ui || !id_str)
        return false;
    lens_id id = lensi_gen_widget_id(ui, id_str);
    if (!lensi_overlay_is_open_id(ui, id))
        return false;

    lens_node *n = register_floating_layer(ui, id, anchor, opts);
    if (!n)
        return false;
    n->is_panel_layer = false;

    /* Push as a sub-root. Do not link as a child of the current container
     * — the layer escapes the normal layout flow (ADR-0037). */
    lensi_open_container_push(ui, n);
    layer_paint_bg(ui, n, opts);
    ui->last_node = n; /* lens_a11y target */
    return true;
}

void lens_overlay_end(lens *ui) {
    if (ui)
        lensi_open_container_pop(ui);
}

/* Constrain the overlay currently being built to its owner's visible region.
 * Dropdowns use the nearest scroll viewport so a portal-style popup can still
 * escape ordinary layout flow without crossing the inspector that owns it. */
void lensi_overlay_constrain_current(lens *ui, flux_rect bounds) {
    lens_node *n = ui ? lensi_open_container(ui) : NULL;
    if (!n || !n->is_overlay || n->is_panel_layer || bounds.w <= 0.0f || bounds.h <= 0.0f)
        return;
    n->overlay_bounds = bounds;
    n->has_overlay_bounds = true;
}

/* ---- panel begin / end -------------------------------------------- */
/* The persistent-chrome sibling. Always renders; placed at the exact
 * rect (clamped on screen); never dismissed. See file header. */

bool lens_layer_begin(lens *ui, const char *id_str, flux_rect rect, lens_overlay_opts opts) {
    if (!ui || !id_str)
        return false;
    lens_id id = lensi_gen_widget_id(ui, id_str);

    lens_node *n = register_floating_layer(ui, id, rect, opts);
    if (!n)
        return false;
    n->is_panel_layer = true;
    /* A persistent layer's rectangle is both its placement anchor and its
     * minimum extent. Content may grow beyond it, but an empty paint-only
     * layer (scrims, selection borders, highlights) must still cover the
     * caller-supplied rectangle. Keep popup overlays content-sized: their
     * anchor describes the owner widget rather than the popup itself. */
    if (rect.w > n->fixed_w)
        n->fixed_w = rect.w;
    if (rect.h > n->fixed_h)
        n->fixed_h = rect.h;

    lensi_open_container_push(ui, n);
    layer_paint_bg(ui, n, opts);
    ui->last_node = n;
    return true;
}

void lens_layer_end(lens *ui) {
    if (ui)
        lensi_open_container_pop(ui);
}

/* ---- layout placement --------------------------------------------- */
/* Overlays drop below the anchor and flip above when out of room (popup
 * behaviour). Panels place at the exact anchor rect (chrome behaviour).
 * Both clamp onto the display. */

void lensi_overlay_layout(lens *ui) {
    if (!ui)
        return;
    float dw = ui->input.display_size.x;
    float dh = ui->input.display_size.y;
    for (uint32_t i = 0; i < ui->overlay_layer_count; i++) {
        lens_node *n = ui->overlay_layers[i];
        if (!n)
            continue;

        /* First pass: arrange at the anchor's left edge with the layer's
         * measured size, then read the solved size for placement. */
        flux_rect probe = {n->overlay_anchor.x, n->overlay_anchor.y, 0, 0};
        lensi_layout_subtree(n, probe);
        float w = n->measured.x;
        float h = n->measured.y;

        float area_x = 0.0f;
        float area_y = 0.0f;
        float area_w = dw;
        float area_h = dh;
        if (n->has_overlay_bounds && !n->is_centered && !n->is_panel_layer) {
            float right = n->overlay_bounds.x + n->overlay_bounds.w;
            float bottom = n->overlay_bounds.y + n->overlay_bounds.h;
            area_x = fmaxf(0.0f, n->overlay_bounds.x);
            area_y = fmaxf(0.0f, n->overlay_bounds.y);
            right = dw > 0.0f ? fminf(dw, right) : right;
            bottom = dh > 0.0f ? fminf(dh, bottom) : bottom;
            area_w = fmaxf(0.0f, right - area_x);
            area_h = fmaxf(0.0f, bottom - area_y);
        }

        float x, y;
        if (n->is_centered) {
            /* Modal content: center on the display (ADR-0039). */
            x = (dw > w) ? (dw - w) * 0.5f : 0;
            y = (dh > h) ? (dh - h) * 0.5f : 0;
        } else if (n->is_panel_layer) {
            /* Persistent chrome: top-left at the anchor, no drop or flip. */
            x = n->overlay_anchor.x;
            y = n->overlay_anchor.y;
        } else {
            /* Popup overlay: drop below the anchor, flip above if it would
             * overflow the owner's visible boundary, then clamp within it. */
            x = n->overlay_anchor.x;
            y = n->overlay_anchor.y + n->overlay_anchor.h; /* below */
            float area_bottom = area_y + area_h;
            if (area_h > 0.0f && y + h > area_bottom) {
                float up = n->overlay_anchor.y - h; /* flip above */
                if (up >= area_y)
                    y = up;
                else
                    y = (area_h > h) ? area_bottom - h : area_y;
            }
        }
        float area_right = area_x + area_w;
        float area_bottom = area_y + area_h;
        if (area_w > 0.0f && x + w > area_right)
            x = area_right - w;
        if (x < area_x)
            x = area_x;
        if (area_h > 0.0f && y + h > area_bottom)
            y = area_bottom - h;
        if (y < area_y)
            y = area_y;

        lensi_layout_subtree(n, (flux_rect){x, y, w, h});
        /* Sub-roots skip lensi_layout_solve, so clamp their scrolls (a
         * dropdown's option list) explicitly now that placement is final —
         * otherwise wheel/thumb offsets drift past the content bounds. */
        lensi_scroll_clamp_subtree(n);
    }
}

/* ---- render: emit each layer after the base tree ------------------ */

/* Reuses lensi_render_node from replay.c so the draw-list emission has
 * a single source of truth; overlays just walk additional sub-roots. */
void lensi_overlay_render(lens *ui, flux_canvas *canvas) {
    if (!ui || !canvas)
        return;
    flux_rect no_clip = {-1e6f, -1e6f, 2e6f, 2e6f};
    for (uint32_t i = 0; i < ui->overlay_layer_count; i++) {
        if (ui->overlay_layers[i]) {
            lens_node *n = ui->overlay_layers[i];
            flux_rect clip = n->has_overlay_bounds ? n->overlay_bounds : no_clip;
            lensi_render_node(ui, canvas, n, clip);
        }
    }
}

/* ---- eclipse: cursor over any rendered floating layer ------------- */
/* Walks this frame's layer array (overlays + panels) so a click on the
 * dock or a popup eclipses the base widget under it. Uses prev_rect
 * (last frame's geometry) for hit-testing. */

bool lensi_point_in_floating_layer(const lens *ui, flux_point p) {
    if (!ui)
        return false;
    /* Iterate LAST frame's floating layers (carried across the arena
     * reset as ids): a popup eclipses base widgets no matter where in
     * this frame's build order the layer re-registers. */
    for (uint32_t i = 0; i < ui->prev_overlay_layer_count; i++) {
        lens_node *n = lensi_store_find(ui, ui->prev_overlay_layer_ids[i]);
        if (n && n->has_prev && lensi_point_in(p, n->prev_rect))
            return true;
    }
    return false;
}

/* ---- dismissal: Escape + click-outside (overlays only) ------------ */
/* Panels are persistent and never dismissed; they are not in the
 * open_overlays[] slot table, so this pass leaves them alone. */

void lensi_overlay_dismiss(lens *ui) {
    if (!ui || !ui->open_overlay_count)
        return;

    bool esc = false;
    for (uint32_t i = 0; i < ui->input.key_count; i++) {
        if (ui->input.keys[i].key == LENS_KEY_ESCAPE && ui->input.keys[i].pressed &&
            !ui->input.keys[i].repeat) {
            esc = true;
            break;
        }
    }
    if (esc) {
        /* Close the top *dismissable* overlay (ADR-0039: a modal pinned
         * with dismissable=false stops Escape here). */
        for (int i = (int)ui->open_overlay_count - 1; i >= 0; i--) {
            if (ui->open_overlays[i].dismissable) {
                for (uint32_t j = (uint32_t)i; j + 1 < ui->open_overlay_count; j++)
                    ui->open_overlays[j] = ui->open_overlays[j + 1];
                ui->open_overlay_count--;
                return;
            }
        }
        return;
    }

    bool press = ui->input.mouse_pressed[LENS_MOUSE_LEFT] ||
                 ui->input.mouse_pressed[LENS_MOUSE_RIGHT] ||
                 ui->input.mouse_pressed[LENS_MOUSE_MIDDLE];
    if (!press)
        return;

    flux_point cur = ui->input.cursor;
    for (int i = (int)ui->open_overlay_count - 1; i >= 0; i--) {
        struct lens_overlay_slot *slot = &ui->open_overlays[i];
        if (slot->opened_frame >= ui->frame)
            continue; /* same-frame grace */
        if (!slot->dismissable)
            continue; /* modal-pinned (ADR-0039) */
        lens_node *ov = lensi_store_find(ui, slot->id);
        bool hit_layer = ov && ov->has_prev && lensi_point_in(cur, ov->prev_rect);
        /* The anchor is part of the popup interaction. Without this, pressing
         * an open dropdown trigger dismisses on press and the trigger reopens
         * the same overlay on release. */
        bool hit_anchor = ov && lensi_point_in(cur, ov->overlay_anchor);
        bool hit = hit_layer || hit_anchor;
        if (!hit) {
            for (uint32_t j = (uint32_t)i; j + 1 < ui->open_overlay_count; j++)
                ui->open_overlays[j] = ui->open_overlays[j + 1];
            ui->open_overlay_count--;
        }
    }
}

/* ---- accessor for the a11y walk ----------------------------------- */

lens_node **lensi_overlay_layers(const lens *ui, uint32_t *out_count) {
    if (out_count)
        *out_count = ui ? ui->overlay_layer_count : 0;
    return ui ? (lens_node **)ui->overlay_layers : NULL;
}
