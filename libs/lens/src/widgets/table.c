/* table.c — virtualized data grid (ADR-0042).
 *
 * A scroll-area-backed table that builds only the visible window of rows.
 * The full row_count drives the scrollbar; only rows intersecting the
 * viewport are drawn (as positioned text), so cost is O(visible) regardless
 * of row_count. Selection persists per-row in the retained store.
 * ADR-0066 adds a keyboard cursor (host-ownable), per-cell icons, and a
 * host-owned selection pull callback — all opt-in through lens_table_opts. */

#include "../internal.h"
#include <stdio.h>

typedef struct {
    float offset_y;         /* scroll offset */
    float thumb_y, thumb_h; /* scrollbar geometry (set at draw) */
    float track_len;
    float scroll_range;
    bool dragging;
    bool hovering;
    bool initialized;
    float drag_start_offset, drag_start_y;
    int selected; /* -1 = none */
    int cursor;   /* retained keyboard cursor row, -1 = none (ADR-0066) */
    /* The effective cursor reported at the end of the previous frame.
     * Comparing against it catches host-driven jumps through opts.cursor
     * (search-as-you-type), not just keyboard moves. */
    int reported_cursor;
    /* lens_node_state keys a node's allocation by BYTE SIZE, and the
     * layout clamp pass (solve.c) plus the scrollbar skin request a
     * lens_scroll_state on every is_scroll node — the table is one.
     * Only a size mismatch keeps those writes from aliasing this
     * struct; the assert below pins the distinction. If the two layouts
     * ever collide, grow this pad. */
    float size_guard;
} lens_table_state;

static_assert(sizeof(lens_table_state) != sizeof(lens_scroll_state),
              "lens_table_state must not alias lens_scroll_state: lens_node_state keys "
              "per-node state by byte size");

/* ------------------------------------------------------------------ */
/*  Cell text measurement helper                                       */
/* ------------------------------------------------------------------ */

static const char *cell_at(lens_table_cell_fn fn, void *user, int r, int c) {
    return fn ? fn(user, r, c) : "";
}

/* ------------------------------------------------------------------ */
/*  Table                                                              */
/* ------------------------------------------------------------------ */

lens_table_result lens_table(lens *ui, const char *id, const lens_table_column *cols, int col_count,
                             int row_count, lens_table_cell_fn cell, void *user,
                             lens_table_opts opts) {
    const lens_theme *t = &ui->theme;
    lens_table_result result = {0};
    result.cursor = -1;
    result.clicked_row = -1;
    lens_style eff = lensi_style_effective(ui);
    float font_size = lensi_style_font_size(&eff, t);
    float padding = lensi_style_padding(&eff, t);
    lens_style_resolved rs = lensi_style_resolve(&eff, t, 0);

    lens_id fid = lensi_gen_widget_id(ui, id);
    lens_node *n = lensi_store_touch(ui, fid);
    if (!n)
        return result;
    lensi_link_child(ui, n);

    /* The grid is capped at 32 columns (the fixed col_x scratch); a caller
     * beyond that is a bug we surface via the overflow flag rather than a
     * silent out-of-bounds read. */
    if (col_count > 32) {
        col_count = 32;
        lensi_set_overflow(ui);
    }

    float row_h = opts.row_height > 0 ? opts.row_height : (font_size + 2.0f * padding);
    float header_h = opts.show_header ? (font_size + 2.0f * padding) : 0.0f;

    n->is_container = false; /* the table is a leaf that draws its own grid */
    n->is_scroll = true;     /* claim scroll-wheel routing */

    /* Size: fill given space; the caller sizes the table via lens_size/flex. */
    float vw = n->fixed_w > 0 ? n->fixed_w : 0.0f;
    float vh = n->fixed_h > 0 ? n->fixed_h : 0.0f;
    if (n->has_prev) {
        if (vw <= 0)
            vw = n->prev_rect.w;
        if (vh <= 0)
            vh = n->prev_rect.h;
    }
    n->measured = (flux_point){vw > 0 ? vw : 200.0f, vh > 0 ? vh : 200.0f};

    lens_table_state *st = lens_node_state(n, sizeof *st);
    /* The store zeroes new state, but row zero is a valid selection. Keep an
     * explicit initialization bit instead of inferring freshness from the
     * scrollbar geometry (tables without overflow never have a thumb). */
    if (st && !st->initialized) {
        st->selected = -1;
        st->cursor = -1;
        st->reported_cursor = -1;
        st->initialized = true;
    }

    /* The header is fixed chrome, not part of the scroll viewport. Rows,
     * scrollbar geometry, and scroll range all use the body rectangle. */
    float rows_h = (float)row_count * row_h;
    float body_h = vh - header_h;
    if (body_h < 0.0f)
        body_h = 0.0f;
    float sb_w = t->scrollbar_width;
    bool scrollable = body_h > 0.0f && rows_h > body_h;
    float view_w = vw - (scrollable ? sb_w : 0.0f);
    if (view_w < 0.0f)
        view_w = 0.0f;

    /* Wheel routing: innermost scroll container under cursor. A placed
     * node in a higher band above the table occludes it (same rule as
     * lensi_interact). */
    bool occluded = n->has_prev && lensi_widget_occluded(ui, n);
    if (n->has_prev && !occluded && lensi_point_in(ui->input.cursor, n->prev_rect))
        ui->scroll_hot_id = n->id;

    /* Apply wheel scroll if this is the hot scroll target. */
    if (st && ui->scroll_hot_id == n->id) {
        st->offset_y -= ui->input.scroll_y * 40.0f + ui->input.scroll_pixels_y;
        ui->input.scroll_y = 0.0f;
        ui->input.scroll_pixels_y = 0.0f;
        ui->scroll_hot_id = 0;
    }

    /* Clamp offset to [0, scroll_range]. */
    float scroll_range = rows_h - body_h;
    if (scroll_range < 0)
        scroll_range = 0;
    if (st) {
        if (st->offset_y < 0)
            st->offset_y = 0;
        if (st->offset_y > scroll_range)
            st->offset_y = scroll_range;
        st->scroll_range = scroll_range;
    }
    float off = st ? st->offset_y : 0.0f;
    /* The table renders rows from st->offset_y and never reads n->scroll_y;
     * leaving it 0 keeps the generic scroll-clamp pass from shifting the
     * a11y row children linked below (they are FLOW but layout-inert). */
    n->scroll_y = 0.0f;

    /* Scrollbar thumb geometry + drag (mirrors scroll.c). */
    if (st && n->has_prev && scrollable) {
        float thumb_h = (body_h / rows_h) * body_h;
        if (thumb_h < t->scrollbar_min_thumb_h)
            thumb_h = t->scrollbar_min_thumb_h;
        if (thumb_h > body_h)
            thumb_h = body_h;
        float track_len = body_h - thumb_h;
        float track_y = n->prev_rect.y + header_h;
        float thumb_y = track_y + (scroll_range > 0 ? (off / scroll_range) * track_len : 0);
        st->thumb_y = thumb_y;
        st->thumb_h = thumb_h;
        st->track_len = track_len;

        const int L = LENS_MOUSE_LEFT;
        flux_rect thumb_rect = {n->prev_rect.x + vw - sb_w, thumb_y, sb_w, thumb_h};
        flux_rect track_rect = {n->prev_rect.x + vw - sb_w, track_y, sb_w, body_h};
        if (ui->active_id == n->id) {
            if (ui->input.mouse_down[L] && st->dragging) {
                float dy = ui->input.cursor.y - st->drag_start_y;
                st->offset_y =
                    st->drag_start_offset + (track_len > 0 ? dy * scroll_range / track_len : 0);
            }
            if (ui->input.mouse_released[L]) {
                st->dragging = false;
                ui->active_id = 0;
            }
        } else if (!occluded && lensi_point_in(ui->input.cursor, thumb_rect) &&
                   ui->input.mouse_pressed[L]) {
            ui->active_id = n->id;
            st->dragging = true;
            st->drag_start_offset = st->offset_y;
            st->drag_start_y = ui->input.cursor.y;
            ui->input.mouse_pressed[L] = false;
        } else if (!occluded && lensi_point_in(ui->input.cursor, track_rect) &&
                   ui->input.mouse_pressed[L]) {
            float page = body_h * 0.9f;
            st->offset_y += ui->input.cursor.y < thumb_y ? -page : page;
            ui->input.mouse_pressed[L] = false;
        }
        if (st->offset_y < 0.0f)
            st->offset_y = 0.0f;
        if (st->offset_y > scroll_range)
            st->offset_y = scroll_range;
        st->hovering = st->dragging ||
                       (!occluded && lensi_point_in(ui->input.cursor, track_rect));
        off = st->offset_y;
        n->scroll_y = 0.0f;
    } else if (st) {
        st->thumb_y = 0.0f;
        st->thumb_h = 0.0f;
        st->track_len = 0.0f;
        st->dragging = false;
        st->hovering = false;
        if (ui->active_id == n->id)
            ui->active_id = 0;
    }

    /* ---- keyboard cursor (ADR-0066) ----
     * The cursor is a row INDEX owned by the host (opts.cursor) or by the
     * retained node state. Clamping against the live row_count runs every
     * frame: a host cursor beyond a shrunk model pulls back into range and
     * reports cursor_changed once (the write-back re-seeds the host). */
    int cursor = opts.cursor ? *opts.cursor : (st ? st->cursor : -1);
    int cursor_start = cursor;
    if (cursor >= row_count)
        cursor = row_count - 1;
    if (cursor < -1)
        cursor = -1;

    /* Key loop, before lensi_interact: arrows/Home/End move the cursor
     * (from -1, Down lands on the first row, Up on the last; the ends
     * clamp), Return activates a valid cursor row. Every handled key is
     * consumed so the central activation in lensi_interact cannot
     * double-fire and a later widget never sees the table's navigation.
     * Space is deliberately NOT handled: it stays available to the host
     * (search-as-you-type into names with spaces, Ctrl+Space toggles). */
    if (opts.keyboard && ui->focused_id == n->id) {
        for (uint32_t i = 0; i < ui->input.key_count; i++) {
            const lens_key_event *k = &ui->input.keys[i];
            if (!k->pressed || ui->key_consumed[i])
                continue;
            int next = cursor;
            if (k->key == LENS_KEY_DOWN)
                next = cursor < 0 ? 0 : (cursor + 1 < row_count ? cursor + 1 : cursor);
            else if (k->key == LENS_KEY_UP)
                next = cursor < 0 ? row_count - 1 : (cursor > 0 ? cursor - 1 : cursor);
            else if (k->key == LENS_KEY_HOME)
                next = row_count > 0 ? 0 : -1;
            else if (k->key == LENS_KEY_END)
                next = row_count - 1;
            else if (k->key == LENS_KEY_RETURN) {
                if (cursor >= 0)
                    result.activated = true;
                ui->key_consumed[i] = 1;
                continue;
            } else
                continue;
            ui->key_consumed[i] = 1;
            if (next != cursor)
                cursor = next;
        }
    }

    /* Keep the cursor row visible whenever it moved for any reason — a
     * keyboard move above or a host rewrite through opts.cursor (e.g.
     * search-as-you-type jumping the cursor): minimal scroll so
     * [cursor*row_h, (cursor+1)*row_h) intersects the body viewport. A
     * static cursor never touches the offset, so this never fights wheel
     * or thumb scrolling. */
    if (st && cursor != st->reported_cursor && cursor >= 0) {
        float row_top = (float)cursor * row_h;
        if (row_top < st->offset_y)
            st->offset_y = row_top;
        else if (row_top + row_h > st->offset_y + body_h)
            st->offset_y = row_top + row_h - body_h;
        if (st->offset_y < 0.0f)
            st->offset_y = 0.0f;
        if (st->offset_y > scroll_range)
            st->offset_y = scroll_range;
        off = st->offset_y;
    }
    if (st)
        st->reported_cursor = cursor;

    result.cursor = cursor;
    result.cursor_changed = cursor != cursor_start;
    if (opts.cursor) {
        if (result.cursor_changed)
            *opts.cursor = cursor;
    } else if (st) {
        st->cursor = cursor;
    }

    /* Join the tab order (selectable tables only) — after the scrollbar
     * block so a thumb press still claims active_id first. The table keeps
     * its own row hit-test, so r.clicked is honored only for the a11y
     * DoAction path (the pending id is captured before lensi_interact
     * consumes it); a mouse click or central Return/Space press reported
     * through r.clicked is deliberately ignored here. */
    bool a11y_fire = ui->a11y_activate_id == n->id;
    lens_response resp = lensi_interact(ui, n, opts.selectable, false);
    if (a11y_fire && resp.clicked)
        result.activated = true;

    /* ---- compute the visible row window ---- */
    int first = (int)(off / row_h);
    if (first < 0)
        first = 0;
    int last = (int)((off + body_h) / row_h) + 1;
    if (last > row_count)
        last = row_count;

    /* ---- row interaction (click selects) ----
     * Rows below the visible body are scrolled out of view: the table's
     * render clips them to the body, and hit-testing must clip the same
     * way — plus any scroll ancestor's viewport — or folded rows keep
     * reacting through the widgets painted over them. */
    bool clipped_out = lensi_point_clipped_by_scroll(n, ui->input.cursor);
    bool clicked_row = false;
    float hovered_row_y = ui->input.cursor.y - n->prev_rect.y + off - header_h;
    bool hovering_row = opts.selectable && n->has_prev && !occluded && !clipped_out &&
                        lensi_point_in(ui->input.cursor, n->prev_rect) &&
                        ui->input.cursor.x < n->prev_rect.x + view_w &&
                        hovered_row_y >= 0.0f && hovered_row_y < body_h;
    if (hovering_row)
        ui->cursor_hint = LENS_CURSOR_POINTER;
    if (opts.selectable && st && n->has_prev && !occluded && !clipped_out &&
        ui->input.mouse_pressed[LENS_MOUSE_LEFT] &&
        lensi_point_in(ui->input.cursor, n->prev_rect)) {
        float ly = ui->input.cursor.y - n->prev_rect.y + off - header_h;
        if (ly >= 0.0f && ly < body_h) {
            int r = (int)(ly / row_h);
            if (r >= 0 && r < row_count) {
                result.clicked_row = r;
                clicked_row = true;
                /* Host-owned selection (ADR-0066): the click is reported
                 * only; the retained store stays out of it. */
                if (!opts.selected_fn) {
                    st->selected = r;
                    result.selected = r;
                    result.selection_changed = true;
                }
            }
        }
    }
    if (opts.selected_fn)
        result.selected = -1; /* the host owns the selection set */
    else if (st)
        result.selected = st->selected;

    /* ---- column x positions ---- */
    /* Resolve column widths: fixed > 0, else equal share of the remainder. */
    float col_x[32];
    float used = 0;
    int flex_cols = 0;
    for (int c = 0; c < col_count && c < 32; c++) {
        col_x[c] = cols[c].width;
        if (cols[c].width > 0)
            used += cols[c].width;
        else
            flex_cols++;
    }
    float flex_w = flex_cols > 0 ? (view_w - used) / flex_cols : 0;
    /* col_x[c] = start x offset of column c. */
    float cx = 0;
    for (int c = 0; c < col_count && c < 32; c++) {
        col_x[c] = cx;
        cx += (cols[c].width > 0 ? cols[c].width : flex_w);
    }

    /* ---- skin record precompute (visible window only; ADR-0042) ------- */
    lens_grid_column *cols_out = flux_arena_alloc(&ui->arena, (size_t)col_count * sizeof *cols_out);
    lens_grid_row *rows_out =
        flux_arena_alloc(&ui->arena, (size_t)(last > first ? last - first : 1) * sizeof *rows_out);
    if (!cols_out || !rows_out) {
        lensi_set_overflow(ui);
    } else {
        for (int c = 0; c < col_count && c < 32; c++) {
            cols_out[c] = (lens_grid_column){
                .title = cols[c].title,
                .x = col_x[c],
                .w = cols[c].width > 0 ? cols[c].width : flex_w,
                .align = cols[c].align,
            };
        }
        int out_r = 0;
        for (int r = first; r < last; r++) {
            float ry = header_h + (float)r * row_h - off;
            bool sel = opts.selected_fn ? opts.selected_fn(user, r)
                                        : (st && r == st->selected);
            bool cur = r == cursor;
            const char **cells =
                flux_arena_alloc(&ui->arena, (size_t)col_count * sizeof *cells);
            float *cell_x = flux_arena_alloc(&ui->arena, (size_t)col_count * sizeof *cell_x);
            /* Icon ids in an arena array parallel to cells (ADR-0066);
             * allocated only when an icon callback was supplied. */
            lens_icon_id *icons =
                opts.icon_fn
                    ? flux_arena_alloc(&ui->arena, (size_t)col_count * sizeof *icons)
                    : NULL;
            if (!cells || !cell_x || (opts.icon_fn && !icons)) {
                lensi_set_overflow(ui);
                break;
            }
            for (int c = 0; c < col_count && c < 32; c++) {
                const char *txt = cell_at(cell, user, r, c);
                /* The callback owns its buffer only until the next call
                 * (a reused scratch is the binding norm), but the skin
                 * reads the cell grid after the whole row loop — copy each
                 * run into the per-frame arena, the same lifetime the
                 * drawlist's text copy already guarantees (drawlist.c).
                 * NULL/"" stay as-is: no text drawn, nothing copied. */
                if (txt && txt[0]) {
                    size_t len = strlen(txt) + 1;
                    char *copy = flux_arena_alloc(&ui->arena, len);
                    if (copy) {
                        memcpy(copy, txt, len);
                        txt = copy;
                    } else {
                        lensi_set_overflow(ui);
                        txt = NULL;
                    }
                }
                cells[c] = txt;
                if (icons)
                    icons[c] = opts.icon_fn(user, r, c);
                float cw = cols[c].width > 0 ? cols[c].width : flex_w;
                float tx = col_x[c] + padding;
                /* A start-aligned cell with a valid icon indents its text
                 * past the glyph box (icon_size = font_size, gap 8 — the
                 * selectable precedent); the skin draws the glyph at the
                 * pre-shift x. Other alignments resolve from the column
                 * edge and carry no icon. */
                if (icons && cols[c].align == LENS_START &&
                    lensi_icon_valid((int32_t)icons[c]))
                    tx += font_size + 8.0f;
                if (txt && txt[0]) {
                    if (cols[c].align == LENS_END || cols[c].align == LENS_STRETCH) {
                        lens_text_metrics tm = lensi_text_measure_label(ui, txt, font_size, 0.0f);
                        tx = col_x[c] + cw - tm.width - padding;
                    } else if (cols[c].align == LENS_CENTER) {
                        lens_text_metrics tm = lensi_text_measure_label(ui, txt, font_size, 0.0f);
                        tx = col_x[c] + (cw - tm.width) * 0.5f;
                    }
                }
                cell_x[c] = tx;
            }
            rows_out[out_r++] = (lens_grid_row){
                .cells = cells,
                .cell_x = cell_x,
                .icons = icons,
                .y = ry,
                .index = r,
                .state = (sel ? LENS_STATE_SELECTED : 0) | (cur ? LENS_STATE_FOCUSED : 0),
            };

            /* a11y row mapping (ADR-0035): each visible row is a
             * non-interactive child node carrying ROLE_ROW so the walk
             * exposes the grid structure with real bounds. The table is a
             * leaf — these children are linked manually, take no part in
             * layout or hit-testing, and keep a one-frame-latent rect like
             * every hit-test geometry (ADR-0029). */
            lens_id row_id = lensi_hash(&r, sizeof r, fid);
            lens_node *row_node = lensi_store_touch(ui, row_id);
            if (row_node) {
                row_node->parent = n;
                row_node->next_sibling = NULL;
                if (n->last_child)
                    n->last_child->next_sibling = row_node;
                else
                    n->first_child = row_node;
                n->last_child = row_node;
                n->child_count++;
                n->child_hash =
                    n->child_hash * 31 + (uint32_t)(row_id ^ (row_id >> 32));
                row_node->final_rect =
                    (flux_rect){n->prev_rect.x, n->prev_rect.y + ry, view_w, row_h};
                lensi_node_semantics(ui, row_node, LENS_ROLE_ROW,
                                     cells[0] && cells[0][0] ? cells[0] : NULL, NULL,
                                     (sel ? LENS_A11Y_SELECTED : 0) |
                                         (cur ? LENS_A11Y_FOCUSED : 0));
            }
        }

        flux_rect sb_track = {vw - sb_w, header_h, sb_w, body_h};
        flux_rect sb_thumb = {vw - sb_w, st ? st->thumb_y - n->prev_rect.y : 0.0f, sb_w,
                              st ? st->thumb_h : 0.0f};
        uint32_t sb_state = (st && st->hovering ? LENS_STATE_HOVERED : 0) |
                            (st && st->dragging ? LENS_STATE_DRAGGED : 0);

        /* emit — through the replaceable skin (ADR-0059) */
        lensi_skin_emit(ui, n,
                        &(lens_widget_record){
                            .kind = LENS_WIDGET_TABLE,
                            .state = 0,
                            .bounds = {0, 0, vw, vh},
                            .last_bounds = n->prev_rect,
                            .style = rs,
                            .style_fields = eff.fields,
                            .content = {.columns = cols_out,
                                        .column_count = col_count,
                                        .rows = rows_out,
                                        .row_count = out_r,
                                        .header_height = opts.show_header ? header_h : 0.0f,
                                        .row_height = row_h,
                                        .view_width = view_w,
                                        .zebra = opts.zebra,
                                        .scrollbar_track = sb_track,
                                        .scrollbar_thumb = sb_thumb,
                                        .has_scrollbar = st && n->has_prev && scrollable,
                                        .scrollbar_state = sb_state},
                        });
    }

    /* a11y */
    lensi_node_semantics(ui, n, LENS_ROLE_TABLE, id, NULL, 0);

    ui->last_response = (lens_response){.id = n->id, .rect = n->prev_rect};
    result.clicked = clicked_row;
    return result;
}
