/* place.c — absolute placement, z bands, and the transient open-set
 * (ADR-0060).
 *
 * One tree: a placed container keeps its parent chain and sibling
 * sequence position, but escapes its parent's layout flow and clip and
 * is emitted in a closed z band (LENS_BAND_*). Placement itself — the
 * EXACT / ANCHORED / CENTERED modes — is resolved by the arrange pass
 * (layout/solve.c); this file owns:
 *
 *   - the public lens_place_begin/end container pair, which links an ABS
 *     node into the tree and stages its placement metadata;
 *   - the band bucketing walk (lensi_place_bucket), the single choke
 *     point that defines the global emission order consumed by render
 *     (replay.c) and, reversed, by hit-testing (input.c);
 *   - the transient open-set: retained per id, orthogonal to stacking.
 *     Escape closes the top dismissable transient; a mouse press outside
 *     its last-frame rect closes it, subject to a same-frame-open grace.
 *     Non-transient placed nodes (chrome, backdrops) never enter the
 *     table and are never dismissed.
 *
 * ADR-0037's parallel overlay_layers[] roots and the eclipse mechanism
 * are gone: occlusion *is* the band-reversed hit-test order, and damage,
 * a11y, scrollbar chrome, and GC all flow through the one tree. */

#include "../internal.h"

/* ---- open-set helpers --------------------------------------------- */
/* Tracked only for transients (dismissal + is_open queries); persistent
 * placed nodes never enter this table. */

static void open_id(lens *ui, lens_id id, bool dismissable); /* fwd */

static int find_open_slot(const lens *ui, lens_id id) {
    for (uint32_t i = 0; i < ui->open_transient_count; i++)
        if (ui->open_transients[i].id == id)
            return (int)i;
    return -1;
}

bool lensi_place_is_open_id(const lens *ui, lens_id id) {
    return ui && find_open_slot(ui, id) >= 0;
}

/* Open a transient by a pre-computed lens_id, without the id-generation
 * side effect of lens_place_open. Used by menu.c (ADR-0040), which has
 * already derived the trigger id and must not perturb the sibling seq. */
void lensi_place_open_id_pub(lens *ui, lens_id id, bool dismissable) {
    if (!ui)
        return;
    open_id(ui, id, dismissable);
}

static void open_id(lens *ui, lens_id id, bool dismissable) {
    if (find_open_slot(ui, id) >= 0)
        return;
    if (ui->open_transient_count >= LENSI_TRANSIENT_MAX) {
        /* drop the oldest to make room; a noisy transient set is a bug we
         * surface via behaviour rather than a crash */
        for (uint32_t i = 0; i + 1 < ui->open_transient_count; i++)
            ui->open_transients[i] = ui->open_transients[i + 1];
        ui->open_transient_count--;
    }
    ui->open_transients[ui->open_transient_count++] = (struct lens_transient_slot){
        .id = id,
        .opened_frame = ui->frame,
        .dismissable = dismissable,
    };
}

static void close_id(lens *ui, lens_id id) {
    int i = find_open_slot(ui, id);
    if (i < 0)
        return;
    for (uint32_t j = (uint32_t)i; j + 1 < ui->open_transient_count; j++)
        ui->open_transients[j] = ui->open_transients[j + 1];
    ui->open_transient_count--;
}

/* ---- public open/close/is_open/hovered ---------------------------- */

void lens_place_open(lens *ui, const char *id) {
    if (!ui || !id)
        return;
    open_id(ui, lensi_gen_widget_id(ui, id), true);
}

void lens_place_close(lens *ui, const char *id) {
    if (!ui || !id)
        return;
    close_id(ui, lensi_gen_widget_id(ui, id));
}

bool lens_place_is_open(const lens *ui, const char *id) {
    if (!ui || !id)
        return false;
    /* lens_current_id derives the same id lens_place_begin used, without
     * touching any build-side counter — a pure query. */
    return lensi_place_is_open_id(ui, lens_current_id(ui, id));
}

bool lens_place_hovered(const lens *ui, const char *id) {
    if (!ui || !id)
        return false;
    lens_id place_id = lens_current_id(ui, id);
    if (!lensi_place_is_open_id(ui, place_id))
        return false;
    lens_node *n = lensi_store_find(ui, place_id);
    return n && n->has_prev && lensi_point_in(ui->input.cursor, n->prev_rect);
}

/* ---- place begin / end -------------------------------------------- */

bool lens_place_begin(lens *ui, const char *id_str, lens_place_opts opts) {
    if (!ui || !id_str)
        return false;
    lens_id id = lensi_gen_widget_id(ui, id_str);
    if (opts.transient && !lensi_place_is_open_id(ui, id))
        return false;

    lens_node *n = lensi_store_touch(ui, id);
    if (!n)
        return false;

    /* Link into the current container BEFORE pushing: a placed node keeps
     * its parent chain and sibling sequence position in the one tree
     * (ADR-0060) even though it escapes the parent's flow and clip. Only
     * container sub-roots may be ABS — this begin is the single place that
     * sets LENS_PLACE_ABS, so leaf widgets cannot place. */
    lensi_link_child(ui, n);

    n->is_container = true;
    n->place = LENS_PLACE_ABS;
    /* Band validation. Out-of-range falls back to POPUP. LENS_BAND_BASE is
     * reserved for FLOW content (ADR-0060 item 2): an ABS node asking for
     * BASE is clamped to CHROME — it would render above the base tree, and
     * the "what paints above also hit-tests above" invariant requires it to
     * occlude like one. */
    lens_band band = (opts.band >= LENS_BAND_BACKDROP && opts.band < LENS_BAND_COUNT)
                         ? opts.band
                         : LENS_BAND_POPUP;
    if (band == LENS_BAND_BASE)
        band = LENS_BAND_CHROME;
    n->band = band;
    n->mode = (opts.mode >= LENS_PLACE_EXACT && opts.mode <= LENS_PLACE_CENTERED)
                  ? opts.mode
                  : LENS_PLACE_EXACT;
    n->place_rect = opts.rect;
    if (opts.bounds.w > 0.0f && opts.bounds.h > 0.0f) {
        n->place_bounds = opts.bounds;
        n->has_place_bounds = true;
    }
    n->transient = opts.transient;
    n->interactive = opts.interactive;

    /* The subtree's internal flexbox (same contract as open_flex). */
    n->axis = LENS_COLUMN;
    n->gap = opts.layout.gap;
    n->pad = opts.layout.pad;
    n->cross = opts.layout.cross;
    if (opts.layout.box.flex != 0)
        n->flex_grow = opts.layout.box.flex;
    if (opts.layout.min_width > 0)
        n->min_w = opts.layout.min_width;
    if (opts.layout.max_width > 0)
        n->max_w = opts.layout.max_width;
    if (opts.layout.min_height > 0)
        n->min_h = opts.layout.min_height;
    if (opts.layout.max_height > 0)
        n->max_h = opts.layout.max_height;

    /* Fixed size: box.width/height win; else min_width fixes the width (the
     * popup contract — a dropdown's menu matches its trigger). For EXACT
     * the rect is additionally a minimum extent: content may grow beyond
     * it, but an empty paint-only node (scrims, dim backdrops, selection
     * borders) must still cover the caller-supplied rectangle. */
    float fw = opts.layout.box.width > 0.0f      ? opts.layout.box.width
               : opts.layout.min_width > 0.0f    ? opts.layout.min_width
                                                 : 0.0f;
    float fh = opts.layout.box.height > 0.0f ? opts.layout.box.height : 0.0f;
    if (n->mode == LENS_PLACE_EXACT) {
        if (opts.rect.w > fw)
            fw = opts.rect.w;
        if (opts.rect.h > fh)
            fh = opts.rect.h;
    }
    if (fw > 0.0f)
        n->fixed_w = fw;
    if (fh > 0.0f)
        n->fixed_h = fh;

    /* Surface fill + border, resolved at replay against final_rect. */
    if ((opts.layout.bg >> 24) != 0) {
        lensi_drawlist_push(ui, n,
                            (lens_draw_cmd){.kind = LENS_DRAW_RECT,
                                            .rel = {0, 0, 0, 0},
                                            .color = opts.layout.bg,
                                            .radius = opts.layout.radius});
    }
    if ((opts.layout.border >> 24) != 0 && opts.layout.border_width > 0.0f) {
        lensi_drawlist_push(ui, n,
                            (lens_draw_cmd){.kind = LENS_DRAW_BORDER,
                                            .rel = {0, 0, 0, 0},
                                            .color = opts.layout.border,
                                            .radius = opts.layout.radius,
                                            .width = opts.layout.border_width});
    }

    lensi_open_container_push(ui, n);
    return true;
}

void lens_place_end(lens *ui) {
    if (ui)
        lensi_open_container_pop(ui);
}

/* ---- band bucketing: the single emission-order choke point -------- */

static void band_push(lens *ui, lens_band band, lens_node *n) {
    if (ui->band_counts[band] == ui->band_caps[band]) {
        uint32_t nc = ui->band_caps[band] ? ui->band_caps[band] * 2 : 4;
        lens_node **na = flux_arena_alloc(&ui->arena, nc * sizeof *na);
        if (!na) {
            lensi_set_overflow(ui);
            return;
        }
        if (ui->bands[band])
            memcpy(na, ui->bands[band], ui->band_counts[band] * sizeof *ui->bands[band]);
        ui->bands[band] = na;
        ui->band_caps[band] = nc;
    }
    ui->bands[band][ui->band_counts[band]++] = n;
}

static void band_collect(lens *ui, lens_node *n) {
    for (lens_node *c = n->first_child; c; c = c->next_sibling) {
        if (c->place == LENS_PLACE_ABS && c->band >= LENS_BAND_BACKDROP &&
            c->band < LENS_BAND_COUNT)
            band_push(ui, c->band, c);
        /* Recurse into ABS nodes as well: a nested placed node (a submenu
         * inside its parent menu) registers in its own band. Tree pre-order
         * within a band = registration order (ADR-0060). */
        band_collect(ui, c);
    }
}

void lensi_place_bucket(lens *ui) {
    if (ui && ui->root)
        band_collect(ui, ui->root);
}

/* ---- prev-frame snapshot for band-ordered hit-testing -------------- */

void lensi_place_snapshot_prev(lens *ui) {
    if (!ui)
        return;
    for (uint32_t b = 0; b < (uint32_t)LENS_BAND_COUNT; b++) {
        uint32_t count = 0;
        for (uint32_t i = 0; i < ui->band_counts[b]; i++) {
            if (count >= LENSI_BAND_PREV_MAX) {
                /* A noisier band than the prev-list budget would silently
                 * drop occlusion coverage — surface it, never truncate
                 * quietly. */
                lensi_set_overflow(ui);
                break;
            }
            if (ui->bands[b][i])
                ui->prev_band_ids[b][count++] = ui->bands[b][i]->id;
        }
        ui->prev_band_counts[b] = count;
    }
}

/* ---- dismissal: Escape + click-outside (transients only) ----------- */
/* Persistent placed nodes are never dismissed; they are not in the
 * open_transients[] slot table, so this pass leaves them alone. */

void lensi_place_dismiss(lens *ui) {
    if (!ui || !ui->open_transient_count)
        return;

    /* Escape closes the top dismissable transient. A key a widget already
     * consumed this frame (key_consumed) is not seen here — the open-set
     * has exactly one writer for dismissal, and double-handling (a widget
     * closing its own popup AND this pass closing another) is a bug we
     * structurally prevent. Key repeats are not filtered: the OS repeat
     * rate applies, matching text-editing keys (ADR-0029). */
    bool esc = false;
    for (uint32_t i = 0; i < ui->input.key_count; i++) {
        if (ui->input.keys[i].key == LENS_KEY_ESCAPE && ui->input.keys[i].pressed &&
            !ui->key_consumed[i]) {
            esc = true;
            break;
        }
    }
    if (esc) {
        /* Close the top *dismissable* transient (ADR-0039: a modal pinned
         * with dismissable=false stops Escape here). */
        for (int i = (int)ui->open_transient_count - 1; i >= 0; i--) {
            if (ui->open_transients[i].dismissable) {
                for (uint32_t j = (uint32_t)i; j + 1 < ui->open_transient_count; j++)
                    ui->open_transients[j] = ui->open_transients[j + 1];
                ui->open_transient_count--;
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
    for (int i = (int)ui->open_transient_count - 1; i >= 0; i--) {
        struct lens_transient_slot *slot = &ui->open_transients[i];
        if (slot->opened_frame >= ui->frame)
            continue; /* same-frame grace */
        if (!slot->dismissable)
            continue; /* modal-pinned (ADR-0039) */
        lens_node *n = lensi_store_find(ui, slot->id);
        bool hit_node = n && n->has_prev && lensi_point_in(cur, n->prev_rect);
        /* The anchor is part of the popup interaction. Without this, pressing
         * an open dropdown trigger dismisses on press and the trigger reopens
         * the same popup on release. For ANCHORED nodes place_rect IS the
         * anchor; for the other modes it is degenerate or covered by the
         * node rect, so the check is harmless. */
        bool hit_anchor = n && lensi_point_in(cur, n->place_rect);
        if (!hit_node && !hit_anchor) {
            for (uint32_t j = (uint32_t)i; j + 1 < ui->open_transient_count; j++)
                ui->open_transients[j] = ui->open_transients[j + 1];
            ui->open_transient_count--;
        }
    }
}
