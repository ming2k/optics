/* textarea.c — multi-line text input with selection, copy/paste, mouse drag. */

#include "../internal.h"

#define LENS_TEXTAREA_LINE_HEIGHT 1.4f
#define LENS_TEXTAREA_SCROLL_SPEED 40.0f

/* ------------------------------------------------------------------ */
/*  Persistent state                                                   */
/* ------------------------------------------------------------------ */

typedef struct lens_textarea_state {
    uint32_t cursor;
    uint32_t sel_anchor;
    float scroll_y;
} lens_textarea_state;

static inline bool sel_active(const lens_textarea_state *ts) {
    return ts->cursor != ts->sel_anchor;
}

static inline uint32_t sel_lo(const lens_textarea_state *ts) {
    return ts->cursor < ts->sel_anchor ? ts->cursor : ts->sel_anchor;
}

static inline uint32_t sel_hi(const lens_textarea_state *ts) {
    return ts->cursor < ts->sel_anchor ? ts->sel_anchor : ts->cursor;
}

static inline void sel_clear(lens_textarea_state *ts) {
    ts->sel_anchor = ts->cursor;
}

/* ------------------------------------------------------------------ */
/*  UTF-8 helpers                                                      */
/* ------------------------------------------------------------------ */

static size_t utf8_next(const char *s, size_t len, size_t pos) {
    if (pos >= len)
        return pos;
    pos++;
    while (pos < len && ((unsigned char)s[pos] & 0xC0) == 0x80)
        pos++;
    return pos;
}

static size_t utf8_prev(const char *s, size_t pos) {
    if (pos == 0)
        return 0;
    pos--;
    while (pos > 0 && ((unsigned char)s[pos] & 0xC0) == 0x80)
        pos--;
    return pos;
}

/* ------------------------------------------------------------------ */
/*  Line helpers                                                       */
/* ------------------------------------------------------------------ */

static int count_lines(const char *buf) {
    int lines = 1;
    for (size_t i = 0; buf && buf[i]; i++)
        if (buf[i] == '\n')
            lines++;
    return lines;
}

static void find_line(const char *buf, size_t cursor, size_t *line_start, int *line_idx) {
    size_t start = 0;
    int idx = 0;
    for (size_t i = 0; i < cursor && buf[i]; i++) {
        if (buf[i] == '\n') {
            start = i + 1;
            idx++;
        }
    }
    *line_start = start;
    *line_idx = idx;
}

static size_t line_start_by_index(const char *buf, int target_idx) {
    size_t start = 0;
    int idx = 0;
    for (size_t i = 0; buf[i] && idx < target_idx; i++) {
        if (buf[i] == '\n') {
            start = i + 1;
            idx++;
        }
    }
    return start;
}

static size_t line_length(const char *buf, size_t start) {
    size_t end = start;
    while (buf[end] && buf[end] != '\n')
        end++;
    return end - start;
}

/* ------------------------------------------------------------------ */
/*  Width helper                                                       */
/* ------------------------------------------------------------------ */

static float prefix_width(lens *ui, const char *s, size_t len, float size_px) {
    if (len == 0 || !s)
        return 0.0f;
    if (len < 256) {
        char tmp[256];
        memcpy(tmp, s, len);
        tmp[len] = '\0';
        return lensi_text_measure_label(ui, tmp, size_px, 0.0f).width;
    }
    char *tmp = flux_arena_alloc(&ui->arena, len + 1);
    if (!tmp)
        return 0.0f;
    memcpy(tmp, s, len);
    tmp[len] = '\0';
    return lensi_text_measure_label(ui, tmp, size_px, 0.0f).width;
}

/* NUL-terminated copy of one line (a BiDi paragraph unit), so per-line caret
 * mapping sees the whole line's bidi context, not a truncated prefix. */
static const char *line_cstr(lens *ui, const char *buf, size_t start, size_t llen,
                             char stack[256]) {
    if (llen < 256) {
        memcpy(stack, buf + start, llen);
        stack[llen] = '\0';
        return stack;
    }
    char *h = flux_arena_alloc(&ui->arena, llen + 1);
    if (!h)
        return NULL;
    memcpy(h, buf + start, llen);
    h[llen] = '\0';
    return h;
}

/* Visual caret x within the cursor's line, logical px from the line origin. */
static float caret_line_x(lens *ui, const char *buf, size_t cursor, float size_px) {
    size_t start;
    int idx;
    find_line(buf, cursor, &start, &idx);
    size_t llen = line_length(buf, start);
    char stack[256];
    const char *line = line_cstr(ui, buf, start, llen, stack);
    if (!line)
        return 0.0f;
    return lensi_text_caret_x(ui, line, cursor - start, size_px, 0.0f);
}

static void delete_range(char *buf, size_t *len, uint32_t lo, uint32_t hi) {
    if (lo >= hi || lo >= *len)
        return;
    if (hi > *len)
        hi = (uint32_t)*len;
    memmove(buf + lo, buf + hi, *len - hi + 1);
    *len -= (hi - lo);
}

/* ------------------------------------------------------------------ */
/*  Mouse position → cursor                                            */
/* ------------------------------------------------------------------ */

static size_t mouse_to_cursor(lens *ui, const char *buf, size_t len, lens_node *n, float padding,
                              float scroll_y, flux_point cursor, float line_h) {
    float local_x = cursor.x - n->prev_rect.x - padding;
    float local_y = cursor.y - n->prev_rect.y - padding + scroll_y;

    /* Find target line */
    int target_line = (int)(local_y / line_h);
    if (target_line < 0)
        target_line = 0;
    int total_lines = count_lines(buf);
    if (target_line >= total_lines)
        target_line = total_lines - 1;

    size_t start = line_start_by_index(buf, target_line);
    size_t llen = line_length(buf, start);

    /* Closest byte within the line, BiDi-aware. */
    char stack[256];
    const char *line = line_cstr(ui, buf, start, llen, stack);
    if (!line)
        return start;
    return start + lensi_text_caret_byte(ui, line, local_x, ui->theme.font_size, 0.0f);
}

/* ------------------------------------------------------------------ */
/*  Selection draw helpers                                             */
/* ------------------------------------------------------------------ */

/* Draw selection highlight for a single line segment. */
static void draw_sel_highlight(lens *ui, lens_node *n, const lens_theme *t, const char *buf,
                               size_t line_start, size_t line_len, uint32_t sel_lo, uint32_t sel_hi,
                               float line_y, float line_h) {
    size_t line_end = line_start + line_len;
    if (sel_hi <= line_start || sel_lo >= line_end)
        return;

    size_t seg_start = sel_lo > line_start ? sel_lo - line_start : 0;
    size_t seg_end = sel_hi < line_end ? sel_hi - line_start : line_len;

    char stack[256];
    const char *line = line_cstr(ui, buf, line_start, line_len, stack);
    if (!line)
        return;

    /* One rect for LTR; several where the selection crosses a direction
     * boundary within the line. */
    lens_text_xrange rects[8];
    int nr = lensi_text_sel_rects(ui, line, seg_start, seg_end, t->font_size, 0.0f, rects, 8);
    for (int i = 0; i < nr; i++) {
        lensi_drawlist_push(ui, n,
                            (lens_draw_cmd){.kind = LENS_DRAW_RECT,
                                            .rel = {t->padding + rects[i].x0, line_y,
                                                    rects[i].x1 - rects[i].x0, line_h},
                                            .color = lensi_color_alpha(t->color_accent, 0x40),
                                            .radius = 1.0f});
    }
}

/* ------------------------------------------------------------------ */
/*  Widget                                                             */
/* ------------------------------------------------------------------ */

bool lens_textarea(lens *ui, const char *label, char *buf, size_t buf_cap, float min_h) {
    const lens_theme *t = &ui->theme;
    bool disabled = ui->next_disabled;
    bool error = ui->next_error;
    const char *placeholder = ui->next_placeholder;
    ui->next_disabled = false;
    ui->next_error = false;
    ui->next_placeholder = NULL;

    lens_id id = lensi_gen_widget_id(ui, label);
    lens_node *n = lensi_store_touch(ui, id);
    if (!n)
        return false;
    lensi_link_child(ui, n);
    n->is_container = false;

    lens_textarea_state *ts = lens_node_state(n, sizeof *ts);
    if (!ts)
        return false;

    size_t len = (buf && buf_cap) ? strlen(buf) : 0;
    if (ts->cursor > len)
        ts->cursor = (uint32_t)len;
    if (ts->sel_anchor > len)
        ts->sel_anchor = (uint32_t)len;

    float line_h = t->font_size * LENS_TEXTAREA_LINE_HEIGHT;
    int lines = (buf && len) ? count_lines(buf) : 1;
    float text_h = lines * line_h;

    float w = (n->fixed_w > 0) ? n->fixed_w : 240.0f;
    float h = (n->fixed_h > 0) ? n->fixed_h : text_h + 2.0f * t->padding;
    if (h < min_h)
        h = min_h;
    n->measured = (flux_point){w, h};

    lens_response r = lensi_interact(ui, n, true, disabled);
    if (r.hovered)
        ui->cursor_hint = LENS_CURSOR_TEXT;
    bool changed = false;

    /* IME manages its own cursor during preedit; clear our selection. */
    if (r.focused && buf && ui->input.preedit_utf8[0])
        sel_clear(ts);

    /* ---- Keyboard handling --------------------------------------- */
    if (r.focused && buf && buf_cap > 1 && !disabled) {
        bool shift = (ui->input.mods & LENS_MOD_SHIFT) != 0;
        bool ctrl = (ui->input.mods & LENS_MOD_CTRL) != 0;

        /* Paste */
        uint32_t paste_len = 0;
        const char *paste = lensi_take_paste(ui, &paste_len);
        if (paste && paste_len) {
            if (sel_active(ts)) {
                delete_range(buf, &len, sel_lo(ts), sel_hi(ts));
                ts->cursor = sel_lo(ts);
                sel_clear(ts);
                changed = true;
            }
            size_t room = buf_cap - 1 > len ? buf_cap - 1 - len : 0;
            size_t to_insert = paste_len < room ? paste_len : room;
            if (to_insert) {
                memmove(buf + ts->cursor + to_insert, buf + ts->cursor, len - ts->cursor + 1);
                memcpy(buf + ts->cursor, paste, to_insert);
                ts->cursor += (uint32_t)to_insert;
                len += to_insert;
                changed = true;
            }
            sel_clear(ts); /* caret follows the insert; no lingering selection */
        }

        /* Keys */
        for (uint32_t i = 0; i < ui->input.key_count; i++) {
            const lens_key_event *k = &ui->input.keys[i];
            if (!k->pressed)
                continue;

            switch (k->key) {
            case LENS_KEY_BACKSPACE:
                if (sel_active(ts)) {
                    delete_range(buf, &len, sel_lo(ts), sel_hi(ts));
                    ts->cursor = sel_lo(ts);
                    sel_clear(ts);
                    changed = true;
                } else if (ts->cursor > 0) {
                    size_t prev = utf8_prev(buf, ts->cursor);
                    memmove(buf + prev, buf + ts->cursor, len - ts->cursor + 1);
                    ts->cursor = (uint32_t)prev;
                    changed = true;
                }
                break;
            case LENS_KEY_DELETE:
                if (sel_active(ts)) {
                    delete_range(buf, &len, sel_lo(ts), sel_hi(ts));
                    ts->cursor = sel_lo(ts);
                    sel_clear(ts);
                    changed = true;
                } else if (ts->cursor < len) {
                    size_t next = utf8_next(buf, len, ts->cursor);
                    memmove(buf + ts->cursor, buf + next, len - next + 1);
                    changed = true;
                }
                break;
            case LENS_KEY_LEFT: {
                /* Visual move within the line; at the line's visual edge fall
                 * back to a logical step across the newline. */
                size_t start;
                int idx;
                find_line(buf, ts->cursor, &start, &idx);
                size_t llen = line_length(buf, start);
                char stack[256];
                const char *line = line_cstr(ui, buf, start, llen, stack);
                size_t col = ts->cursor - start;
                size_t nv =
                    line ? lensi_text_caret_visual(ui, line, col, false, t->font_size, 0.0f) : col;
                if (nv != col)
                    ts->cursor = (uint32_t)(start + nv);
                else if (ts->cursor > 0)
                    ts->cursor = (uint32_t)utf8_prev(buf, ts->cursor);
                if (!shift)
                    sel_clear(ts);
                break;
            }
            case LENS_KEY_RIGHT: {
                size_t start;
                int idx;
                find_line(buf, ts->cursor, &start, &idx);
                size_t llen = line_length(buf, start);
                char stack[256];
                const char *line = line_cstr(ui, buf, start, llen, stack);
                size_t col = ts->cursor - start;
                size_t nv =
                    line ? lensi_text_caret_visual(ui, line, col, true, t->font_size, 0.0f) : col;
                if (nv != col)
                    ts->cursor = (uint32_t)(start + nv);
                else if (ts->cursor < len)
                    ts->cursor = (uint32_t)utf8_next(buf, len, ts->cursor);
                if (!shift)
                    sel_clear(ts);
                break;
            }
            case LENS_KEY_HOME: {
                size_t start;
                int idx;
                find_line(buf, ts->cursor, &start, &idx);
                ts->cursor = (uint32_t)start;
                if (!shift)
                    sel_clear(ts);
                break;
            }
            case LENS_KEY_END: {
                size_t start;
                int idx;
                find_line(buf, ts->cursor, &start, &idx);
                ts->cursor = (uint32_t)(start + line_length(buf, start));
                if (!shift)
                    sel_clear(ts);
                break;
            }
            case LENS_KEY_UP: {
                /* Preserve the caret's visual x across the line change, so it
                 * tracks the column under proportional and BiDi text. */
                size_t start;
                int idx;
                find_line(buf, ts->cursor, &start, &idx);
                if (idx > 0) {
                    float x = caret_line_x(ui, buf, ts->cursor, t->font_size);
                    size_t prev_start = line_start_by_index(buf, idx - 1);
                    size_t prev_len = line_length(buf, prev_start);
                    char stack[256];
                    const char *pl = line_cstr(ui, buf, prev_start, prev_len, stack);
                    size_t col = pl ? lensi_text_caret_byte(ui, pl, x, t->font_size, 0.0f) : 0;
                    ts->cursor = (uint32_t)(prev_start + col);
                }
                if (!shift)
                    sel_clear(ts);
                break;
            }
            case LENS_KEY_DOWN: {
                size_t start;
                int idx;
                find_line(buf, ts->cursor, &start, &idx);
                int total = count_lines(buf);
                if (idx < total - 1) {
                    float x = caret_line_x(ui, buf, ts->cursor, t->font_size);
                    size_t next_start = line_start_by_index(buf, idx + 1);
                    size_t next_len = line_length(buf, next_start);
                    char stack[256];
                    const char *nl = line_cstr(ui, buf, next_start, next_len, stack);
                    size_t col = nl ? lensi_text_caret_byte(ui, nl, x, t->font_size, 0.0f) : 0;
                    ts->cursor = (uint32_t)(next_start + col);
                }
                if (!shift)
                    sel_clear(ts);
                break;
            }
            case LENS_KEY_RETURN:
                if (sel_active(ts)) {
                    delete_range(buf, &len, sel_lo(ts), sel_hi(ts));
                    ts->cursor = sel_lo(ts);
                    sel_clear(ts);
                    changed = true;
                }
                if (len < buf_cap - 1) {
                    memmove(buf + ts->cursor + 1, buf + ts->cursor, len - ts->cursor + 1);
                    buf[ts->cursor] = '\n';
                    ts->cursor++;
                    changed = true;
                }
                break;
            }

            if (ctrl) {
                switch (k->key) {
                case 'a':
                case 'A':
                    ts->sel_anchor = 0;
                    ts->cursor = (uint32_t)len;
                    break;
                case 'c':
                case 'C':
                    if (sel_active(ts) && ui->clipboard.set_text) {
                        lens_copy(ui, buf + sel_lo(ts), sel_hi(ts) - sel_lo(ts));
                    }
                    break;
                case 'x':
                case 'X':
                    if (sel_active(ts)) {
                        if (ui->clipboard.set_text) {
                            lens_copy(ui, buf + sel_lo(ts), sel_hi(ts) - sel_lo(ts));
                        }
                        delete_range(buf, &len, sel_lo(ts), sel_hi(ts));
                        ts->cursor = sel_lo(ts);
                        sel_clear(ts);
                        changed = true;
                    }
                    break;
                case 'v':
                case 'V':
                    lens_request_paste(ui);
                    break;
                }
            }
        }

        /* Committed text */
        if (ui->input.text_utf8[0]) {
            if (sel_active(ts)) {
                delete_range(buf, &len, sel_lo(ts), sel_hi(ts));
                ts->cursor = sel_lo(ts);
                sel_clear(ts);
                changed = true;
            }
            size_t tlen = strlen(ui->input.text_utf8);
            size_t room = buf_cap - 1 > len ? buf_cap - 1 - len : 0;
            size_t to_insert = tlen < room ? tlen : room;
            if (to_insert) {
                memmove(buf + ts->cursor + to_insert, buf + ts->cursor, len - ts->cursor + 1);
                memcpy(buf + ts->cursor, ui->input.text_utf8, to_insert);
                ts->cursor += (uint32_t)to_insert;
                len += to_insert;
                changed = true;
            }
            sel_clear(ts); /* a freshly typed char is not left selected */
        }
    }

    /* Mouse drag selection */
    if (!disabled && buf) {
        if (ui->input.mouse_pressed[LENS_MOUSE_LEFT] && r.hovered) {
            ts->cursor = (uint32_t)mouse_to_cursor(ui, buf, len, n, t->padding, ts->scroll_y,
                                                   ui->input.cursor, line_h);
            sel_clear(ts);
        }
        if (ui->active_id == id && ui->input.mouse_down[LENS_MOUSE_LEFT]) {
            ts->cursor = (uint32_t)mouse_to_cursor(ui, buf, len, n, t->padding, ts->scroll_y,
                                                   ui->input.cursor, line_h);
        }
        if (ui->active_id == id && !ui->input.mouse_down[LENS_MOUSE_LEFT]) {
            ui->active_id = 0;
            if (ts->cursor == ts->sel_anchor)
                sel_clear(ts);
        }
    }

    if (changed) {
        len = (buf && buf_cap) ? strlen(buf) : 0;
        lines = count_lines(buf);
        text_h = lines * line_h;
    }

    /* ---- Scroll clamping ------------------------------------------ */
    float max_scroll = text_h + 2.0f * t->padding - h;
    if (max_scroll < 0)
        max_scroll = 0;

    if (r.focused && buf) {
        size_t start;
        int idx;
        find_line(buf, ts->cursor, &start, &idx);
        float cursor_y = t->padding + idx * line_h;
        float cursor_bot = cursor_y + line_h;

        if (cursor_y < ts->scroll_y + t->padding)
            ts->scroll_y = cursor_y - t->padding;
        if (cursor_bot > ts->scroll_y + h - t->padding)
            ts->scroll_y = cursor_bot - h + t->padding;
    }

    if (ts->scroll_y < 0)
        ts->scroll_y = 0;
    if (ts->scroll_y > max_scroll)
        ts->scroll_y = max_scroll;

    /* Wheel scroll */
    if (r.hovered && !disabled) {
        ts->scroll_y -= ui->input.scroll_y * LENS_TEXTAREA_SCROLL_SPEED;
        if (ts->scroll_y < 0)
            ts->scroll_y = 0;
        if (ts->scroll_y > max_scroll)
            ts->scroll_y = max_scroll;
    }

    bool has_preedit = r.focused && buf && buf_cap > 1 && ui->input.preedit_utf8[0];

    /* ---- Drawing -------------------------------------------------- */
    uint32_t bg = (r.hovered && !disabled) ? t->color_hover : t->color_bg;
    lensi_drawlist_push(
        ui, n,
        (lens_draw_cmd){
            .kind = LENS_DRAW_RECT, .rel = {0, 0, 0, 0}, .color = bg, .radius = t->corner_radius});

    /* Selection highlight (behind text) */
    if (r.focused && sel_active(ts) && buf && len && !has_preedit) {
        const char *p = buf;
        int line_idx = 0;
        size_t pos = 0;
        while (p && *p) {
            const char *end = strchr(p, '\n');
            size_t llen = end ? (size_t)(end - p) : strlen(p);
            float line_y = t->padding + line_idx * line_h - ts->scroll_y;
            draw_sel_highlight(ui, n, t, buf, pos, llen, sel_lo(ts), sel_hi(ts), line_y, line_h);
            if (!end)
                break;
            p = end + 1;
            pos = (size_t)(p - buf);
            line_idx++;
        }
    }

    /* Text lines */
    size_t cursor_line_start = 0;
    int cursor_line_idx = 0;
    if (has_preedit)
        find_line(buf, ts->cursor, &cursor_line_start, &cursor_line_idx);

    if (buf) {
        const char *p = buf;
        int line_idx = 0;
        while (p && *p) {
            const char *end = strchr(p, '\n');
            size_t llen = end ? (size_t)(end - p) : strlen(p);

            if (has_preedit && line_idx == cursor_line_idx) {
                size_t prefix_len = ts->cursor - cursor_line_start;
                if (prefix_len > llen)
                    prefix_len = llen;
                size_t suffix_len = llen - prefix_len;
                const char *pe = ui->input.preedit_utf8;
                size_t pe_len = strlen(pe);
                size_t display_len = prefix_len + pe_len + suffix_len;
                char *display = flux_arena_alloc(&ui->arena, display_len + 1);
                if (display) {
                    if (prefix_len)
                        memcpy(display, p, prefix_len);
                    if (pe_len)
                        memcpy(display + prefix_len, pe, pe_len);
                    if (suffix_len)
                        memcpy(display + prefix_len + pe_len, p + prefix_len, suffix_len);
                    display[display_len] = '\0';
                    lensi_drawlist_push(
                        ui, n,
                        (lens_draw_cmd){.kind = LENS_DRAW_TEXT,
                                        .rel = {t->padding,
                                                t->padding + line_idx * line_h - ts->scroll_y, 0,
                                                0},
                                        .color = t->color_fg,
                                        .text = display,
                                        .text_size = t->font_size});
                }

                float prefix_w = prefix_width(ui, p, prefix_len, t->font_size);
                float pe_w = prefix_width(ui, pe, pe_len, t->font_size);
                float line_y = t->padding + line_idx * line_h - ts->scroll_y;
                lensi_drawlist_push(ui, n,
                                    (lens_draw_cmd){.kind = LENS_DRAW_RECT,
                                                    .rel = {t->padding + prefix_w,
                                                            line_y + line_h - 1.0f, pe_w, 1.0f},
                                                    .color = t->color_accent});
            } else if (llen > 0) {
                char *line = flux_arena_alloc(&ui->arena, llen + 1);
                if (line) {
                    memcpy(line, p, llen);
                    line[llen] = '\0';
                    lensi_drawlist_push(
                        ui, n,
                        (lens_draw_cmd){.kind = LENS_DRAW_TEXT,
                                        .rel = {t->padding,
                                                t->padding + line_idx * line_h - ts->scroll_y, 0,
                                                0},
                                        .color = t->color_fg,
                                        .text = line,
                                        .text_size = t->font_size});
                }
            }

            if (!end)
                break;
            p = end + 1;
            line_idx++;
        }
    }

    /* Preedit on empty buffer */
    if (has_preedit && (!buf || !buf[0])) {
        float line_y = t->padding - ts->scroll_y;
        const char *pe = ui->input.preedit_utf8;
        lensi_drawlist_push(ui, n,
                            (lens_draw_cmd){.kind = LENS_DRAW_TEXT,
                                            .rel = {t->padding, line_y, 0, 0},
                                            .color = t->color_fg,
                                            .text = pe,
                                            .text_size = t->font_size});
        float pe_w = prefix_width(ui, pe, strlen(pe), t->font_size);
        lensi_drawlist_push(ui, n,
                            (lens_draw_cmd){.kind = LENS_DRAW_RECT,
                                            .rel = {t->padding, line_y + line_h - 1.0f, pe_w, 1.0f},
                                            .color = t->color_accent});
    }

    /* Placeholder */
    if ((!buf || !buf[0]) && !r.focused && placeholder) {
        lensi_drawlist_push(ui, n,
                            (lens_draw_cmd){.kind = LENS_DRAW_TEXT,
                                            .rel = {t->padding, t->padding - ts->scroll_y, 0, 0},
                                            .color = t->color_disabled,
                                            .text = placeholder,
                                            .text_size = t->font_size});
    }

    /* Caret */
    if (!disabled && r.focused && buf) {
        size_t start;
        int idx;
        find_line(buf, ts->cursor, &start, &idx);

        float pw = caret_line_x(ui, buf, ts->cursor, t->font_size);
        if (has_preedit) {
            pw += prefix_width(ui, ui->input.preedit_utf8, ui->input.preedit_cursor, t->font_size);
        }

        float cx = t->padding + pw;
        float cy = t->padding + idx * line_h - ts->scroll_y;

        lensi_drawlist_push(ui, n,
                            (lens_draw_cmd){.kind = LENS_DRAW_RECT,
                                            .rel = {cx - 1.0f, cy, 2.0f, line_h},
                                            .color = t->color_accent,
                                            .radius = 1.0f});
    }

    /* Border */
    uint32_t border_color =
        error ? t->color_error : ((r.focused && !disabled) ? t->color_accent : t->color_border);
    if (disabled)
        border_color = t->color_disabled;
    lensi_drawlist_push(ui, n,
                        (lens_draw_cmd){.kind = LENS_DRAW_BORDER,
                                        .rel = {0, 0, 0, 0},
                                        .color = border_color,
                                        .width = t->border_width,
                                        .radius = t->corner_radius});

    /* IME caret rect */
    if (!disabled && r.focused && buf && buf_cap > 1) {
        size_t start;
        int idx;
        find_line(buf, ts->cursor, &start, &idx);

        float pw = caret_line_x(ui, buf, ts->cursor, t->font_size);
        if (has_preedit) {
            pw += prefix_width(ui, ui->input.preedit_utf8, ui->input.preedit_cursor, t->font_size);
        }

        lensi_set_caret_rect(ui, (flux_rect){
                                     n->prev_rect.x + t->padding + pw,
                                     n->prev_rect.y + t->padding + idx * line_h - ts->scroll_y,
                                     2.0f,
                                     line_h,
                                 });
    }

    /* Semantics */
    uint32_t sem_flags = (r.focused ? LENS_A11Y_FOCUSED : 0) | (disabled ? LENS_A11Y_DISABLED : 0);
    lensi_node_semantics(ui, n, LENS_ROLE_TEXTAREA, label, buf, sem_flags);

    ui->last_response = r;
    return changed;
}

lens_response lens_textarea_ex(lens *ui, lens_textarea_opts o) {
    lensi_apply_box(ui, o.box);
    if (o.placeholder)
        lensi_set_placeholder(ui, o.placeholder);
    bool scoped = o.box.id && o.box.id[0];
    if (scoped)
        lens_push_id(ui, o.box.id);
    lens_textarea(ui, o.label ? o.label : "", o.buf, o.buf_cap, o.min_height);
    if (scoped)
        lens_pop_id(ui);
    if (o.box.tooltip)
        lensi_tooltip(ui, o.box.tooltip);
    return ui->last_response;
}
