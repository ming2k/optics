/* table.c — virtualized data grid (ADR-0019).
 *
 * A scroll-area-backed table that builds only the visible window of rows.
 * The full row_count drives the scrollbar; only rows intersecting the
 * viewport are drawn (as positioned text), so cost is O(visible) regardless
 * of row_count. Selection persists per-row in the retained store. */

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
} lens_table_state;

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

    lens_id fid = lensi_gen_widget_id(ui, id);
    lens_node *n = lensi_store_touch(ui, fid);
    if (!n)
        return result;
    lensi_link_child(ui, n);

    float row_h = opts.row_height > 0 ? opts.row_height : (t->font_size + 2.0f * t->padding);
    float header_h = opts.show_header ? (t->font_size + 2.0f * t->padding) : 0.0f;

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

    /* Wheel routing: innermost scroll container under cursor. A floating
     * layer above the table eclipses it (same rule as lensi_interact). */
    bool eclipsed = n->has_prev && lensi_widget_eclipsed(ui, n);
    if (n->has_prev && !eclipsed && lensi_point_in(ui->input.cursor, n->prev_rect))
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
    n->scroll_y = off;

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
        } else if (!eclipsed && lensi_point_in(ui->input.cursor, thumb_rect) &&
                   ui->input.mouse_pressed[L]) {
            ui->active_id = n->id;
            st->dragging = true;
            st->drag_start_offset = st->offset_y;
            st->drag_start_y = ui->input.cursor.y;
            ui->input.mouse_pressed[L] = false;
        } else if (!eclipsed && lensi_point_in(ui->input.cursor, track_rect) &&
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
                       (!eclipsed && lensi_point_in(ui->input.cursor, track_rect));
        off = st->offset_y;
        n->scroll_y = off;
    } else if (st) {
        st->thumb_y = 0.0f;
        st->thumb_h = 0.0f;
        st->track_len = 0.0f;
        st->dragging = false;
        st->hovering = false;
        if (ui->active_id == n->id)
            ui->active_id = 0;
    }

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
    bool hovering_row = opts.selectable && n->has_prev && !eclipsed && !clipped_out &&
                        lensi_point_in(ui->input.cursor, n->prev_rect) &&
                        ui->input.cursor.x < n->prev_rect.x + view_w &&
                        hovered_row_y >= 0.0f && hovered_row_y < body_h;
    if (hovering_row)
        ui->cursor_hint = LENS_CURSOR_POINTER;
    if (opts.selectable && st && n->has_prev && !eclipsed && !clipped_out &&
        ui->input.mouse_pressed[LENS_MOUSE_LEFT] &&
        lensi_point_in(ui->input.cursor, n->prev_rect)) {
        float ly = ui->input.cursor.y - n->prev_rect.y + off - header_h;
        if (ly >= 0.0f && ly < body_h) {
            int r = (int)(ly / row_h);
            if (r >= 0 && r < row_count) {
                st->selected = r;
                result.selected = r;
                result.selection_changed = true;
                clicked_row = true;
            }
        }
    }
    if (st)
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
    float cx = 0;
    for (int c = 0; c < col_count && c < 32; c++) {
        if (col_x[c] <= 0)
            col_x[c] = flex_w;
        col_x[c] = cx; /* now holds the x offset */
        cx += (c + 1 < col_count && cols[c].width <= 0)
                  ? flex_w
                  : (cols[c].width > 0 ? cols[c].width : flex_w);
    }
    /* recompute properly: col_x[c] = start x of column c */
    cx = 0;
    for (int c = 0; c < col_count && c < 32; c++) {
        col_x[c] = cx;
        cx += (cols[c].width > 0 ? cols[c].width : flex_w);
    }

    /* ---- draw: background, header, rows ---- */
    lensi_drawlist_push(ui, n, (lens_draw_cmd){.kind = LENS_DRAW_CLIP_PUSH, .rel = {0, 0, 0, 0}});

    /* Header. */
    if (opts.show_header) {
        lensi_drawlist_push(ui, n,
                            (lens_draw_cmd){.kind = LENS_DRAW_RECT,
                                            .rel = {0, 0, 0, header_h},
                                            .color = t->color_hover,
                                            .radius = 0.0f});
        for (int c = 0; c < col_count && c < 32; c++) {
            if (!cols[c].title)
                continue;
            lensi_drawlist_push(ui, n,
                                (lens_draw_cmd){.kind = LENS_DRAW_TEXT,
                                                .rel = {col_x[c] + t->padding,
                                                        (header_h - t->font_size) * 0.5f, 0, 0},
                                                .color = t->color_fg,
                                                .text = cols[c].title,
                                                .text_size = t->font_size,
                                                .text_weight = t->font_weight_bold});
        }
    }

    /* Rows scroll below a fixed header. Give the body its own clip so rows
     * moving out at the top cannot paint across the column titles. */
    float body_y = header_h;
    lensi_drawlist_push(ui, n,
                        (lens_draw_cmd){.kind = LENS_DRAW_CLIP_PUSH,
                                        .rel = {0, body_y, view_w, body_h}});

    /* Visible rows. */
    for (int r = first; r < last; r++) {
        float ry = header_h + (float)r * row_h - off;
        bool sel = st && r == st->selected;
        if (sel) {
            lensi_drawlist_push(ui, n,
                                (lens_draw_cmd){.kind = LENS_DRAW_RECT,
                                                .rel = {0, ry, view_w, row_h},
                                                .color = t->color_active,
                                                .radius = 0.0f});
        } else if (r % 2 == 1 && opts.zebra) {
            lensi_drawlist_push(ui, n,
                                (lens_draw_cmd){.kind = LENS_DRAW_RECT,
                                                .rel = {0, ry, view_w, row_h},
                                                .color = lensi_color_alpha(t->color_hover, 40),
                                                .radius = 0.0f});
        }
        for (int c = 0; c < col_count && c < 32; c++) {
            const char *txt = cell_at(cell, user, r, c);
            if (!txt || !txt[0])
                continue;
            lens_align al = cols[c].align;
            float cw = cols[c].width > 0 ? cols[c].width : flex_w;
            float tx = col_x[c] + t->padding;
            if (al == LENS_END || al == LENS_STRETCH) {
                lens_text_metrics tm = lensi_text_measure_label(ui, txt, t->font_size, 0.0f);
                tx = col_x[c] + cw - tm.width - t->padding;
            } else if (al == LENS_CENTER) {
                lens_text_metrics tm = lensi_text_measure_label(ui, txt, t->font_size, 0.0f);
                tx = col_x[c] + (cw - tm.width) * 0.5f;
            }
            /* Clip each cell independently. Without this, a long title can
             * paint through artist/album/duration columns even though the
             * table itself is clipped to the viewport. */
            lensi_drawlist_push(ui, n,
                                (lens_draw_cmd){.kind = LENS_DRAW_CLIP_PUSH,
                                                .rel = {col_x[c], ry, cw, row_h}});
            lensi_drawlist_push(
                ui, n,
                (lens_draw_cmd){.kind = LENS_DRAW_TEXT,
                                .rel = {tx, ry + (row_h - t->font_size) * 0.5f, 0, 0},
                                .color = sel ? t->color_accent : t->color_fg,
                                .text = txt,
                                .text_size = t->font_size});
            lensi_drawlist_push(ui, n,
                                (lens_draw_cmd){.kind = LENS_DRAW_CLIP_POP,
                                                .rel = {col_x[c], ry, cw, row_h}});
        }
    }

    lensi_drawlist_push(ui, n,
                        (lens_draw_cmd){.kind = LENS_DRAW_CLIP_POP,
                                        .rel = {0, body_y, view_w, body_h}});

    lensi_drawlist_push(ui, n, (lens_draw_cmd){.kind = LENS_DRAW_CLIP_POP, .rel = {0, 0, 0, 0}});

    /* Scrollbar thumb (drawn after the clip so it sits above content). */
    if (st && n->has_prev && scrollable) {
        if (t->color_scrollbar_track & 0xff000000u) {
            lensi_drawlist_push(ui, n,
                                (lens_draw_cmd){.kind = LENS_DRAW_RECT,
                                                .rel = {vw - sb_w, header_h, sb_w, body_h},
                                                .color = t->color_scrollbar_track,
                                                .radius = t->scrollbar_radius});
        }
        bool hov = st->hovering;
        flux_color tc = st->dragging ? t->color_scrollbar_thumb_active
                        : hov        ? t->color_scrollbar_thumb_hover
                                     : t->color_scrollbar_thumb;
        float local_y = st->thumb_y - n->prev_rect.y;
        lensi_drawlist_push(ui, n,
                            (lens_draw_cmd){.kind = LENS_DRAW_RECT,
                                            .rel = {vw - sb_w, local_y, sb_w, st->thumb_h},
                                            .color = tc,
                                            .radius = t->scrollbar_radius});
    }

    /* a11y */
    lensi_node_semantics(ui, n, LENS_ROLE_SCROLLAREA, id, NULL, 0);

    ui->last_response = (lens_response){.id = n->id, .rect = n->prev_rect};
    result.clicked = clicked_row;
    return result;
}
