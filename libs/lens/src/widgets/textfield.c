/* textfield.c — single-line text input widget with selection (ADR-0008). */

#include "../internal.h"
#include <math.h>

/* ------------------------------------------------------------------ */
/*  UTF-8 helpers                                                      */
/* ------------------------------------------------------------------ */

static size_t utf8_char_len(const char *s, size_t pos) {
    unsigned char c = (unsigned char)s[pos];
    if ((c & 0x80) == 0)
        return 1;
    if ((c & 0xe0) == 0xc0)
        return 2;
    if ((c & 0xf0) == 0xe0)
        return 3;
    if ((c & 0xf8) == 0xf0)
        return 4;
    return 1;
}

static size_t utf8_prev(const char *s, size_t pos) {
    if (pos == 0)
        return 0;
    size_t p = pos - 1;
    while (p > 0 && ((unsigned char)s[p] & 0xc0) == 0x80)
        p--;
    return p;
}

static size_t utf8_next(const char *s, size_t len, size_t pos) {
    if (pos >= len)
        return len;
    return pos + utf8_char_len(s, pos);
}

/* ------------------------------------------------------------------ */
/*  Width of the first `len` bytes of a UTF-8 string.                 */
/* ------------------------------------------------------------------ */

/* Caret x for the first `len` bytes of the NUL-terminated string `s`. Routes
 * through the shared text layout (BiDi-correct edges) so the caret/selection
 * match the painted glyphs exactly. All callers pass a full string. */
static float prefix_width(lens *ui, const char *s, size_t len, float size_px) {
    if (len == 0 || !s)
        return 0.0f;
    return lensi_text_caret_x(ui, s, len, size_px, 0.0f);
}

/* ------------------------------------------------------------------ */
/*  Persistent state                                                   */
/* ------------------------------------------------------------------ */

typedef struct lens_textfield_state {
    uint32_t cursor;
    uint32_t sel_anchor;
    float scroll_x;
} lens_textfield_state;

static inline bool sel_active(const lens_textfield_state *ts) {
    return ts->cursor != ts->sel_anchor;
}

static inline uint32_t sel_lo(const lens_textfield_state *ts) {
    return ts->cursor < ts->sel_anchor ? ts->cursor : ts->sel_anchor;
}

static inline uint32_t sel_hi(const lens_textfield_state *ts) {
    return ts->cursor < ts->sel_anchor ? ts->sel_anchor : ts->cursor;
}

static inline void sel_clear(lens_textfield_state *ts) {
    ts->sel_anchor = ts->cursor;
}

static void delete_range(char *buf, size_t *len, uint32_t lo, uint32_t hi) {
    if (lo >= hi || lo >= *len)
        return;
    if (hi > *len)
        hi = (uint32_t)*len;
    memmove(buf + lo, buf + hi, *len - hi + 1);
    *len -= (hi - lo);
}

/* Byte offset of the caret nearest the mouse x within the field. */
static size_t mouse_x_to_cursor(lens *ui, const char *buf, size_t len, lens_node *n, float padding,
                                float scroll_x, float size_px) {
    if (!buf || !len)
        return 0;
    float local_x = ui->input.cursor.x - n->prev_rect.x - padding + scroll_x;
    return lensi_text_caret_byte(ui, buf, local_x, size_px, 0.0f);
}

/* ------------------------------------------------------------------ */
/*  lens_textfield                                                       */
/* ------------------------------------------------------------------ */

bool lens_textfield(lens *ui, const char *label, char *buf, size_t buf_cap) {
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

    lens_textfield_state *ts = lens_node_state(n, sizeof *ts);
    if (!ts)
        return false;

    size_t len = (buf && buf_cap) ? strlen(buf) : 0;
    if (ts->cursor > len)
        ts->cursor = (uint32_t)len;
    if (ts->sel_anchor > len)
        ts->sel_anchor = (uint32_t)len;

    /* ---- Measure --------------------------------------------------- */
    lens_text_metrics tm = lensi_text_measure_label(ui, buf ? buf : "", t->font_size, 0.0f);
    lens_text_metrics fm = lensi_text_measure_label(ui, "Ag", t->font_size, 0.0f);
    float min_w = 160.0f;
    float w = (n->fixed_w > 0) ? n->fixed_w : fmaxf(tm.width + 2.0f * t->padding, min_w);
    float h = (n->fixed_h > 0) ? n->fixed_h : fm.height + 2.0f * t->padding;
    n->measured = (flux_point){w, h};

    float text_y = (h - fm.height) * 0.5f;
    if (text_y < 0.0f)
        text_y = 0.0f;

    /* ---- Interaction ----------------------------------------------- */
    lens_response r = lensi_interact(ui, n, true, disabled);
    if (r.hovered)
        ui->cursor_hint = LENS_CURSOR_TEXT;
    bool changed = false;

    if (!disabled && r.focused && buf && buf_cap > 1) {
        bool shift = (ui->input.mods & LENS_MOD_SHIFT) != 0;
        bool ctrl = (ui->input.mods & LENS_MOD_CTRL) != 0;

        /* Clear selection when preedit starts — IME manages its own cursor. */
        if (ui->input.preedit_utf8[0])
            sel_clear(ts);

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
                /* Move by visual position (BiDi-aware); for LTR this is the
                 * previous character. */
                size_t nv = lensi_text_caret_visual(ui, buf, ts->cursor, false, t->font_size, 0.0f);
                if (nv != ts->cursor) {
                    ts->cursor = (uint32_t)nv;
                    if (!shift)
                        sel_clear(ts);
                } else if (!shift) {
                    sel_clear(ts);
                }
                break;
            }
            case LENS_KEY_RIGHT: {
                size_t nv = lensi_text_caret_visual(ui, buf, ts->cursor, true, t->font_size, 0.0f);
                if (nv != ts->cursor) {
                    ts->cursor = (uint32_t)nv;
                    if (!shift)
                        sel_clear(ts);
                } else if (!shift) {
                    sel_clear(ts);
                }
                break;
            }
            case LENS_KEY_HOME:
                ts->cursor = 0;
                if (!shift)
                    sel_clear(ts);
                break;
            case LENS_KEY_END:
                ts->cursor = (uint32_t)len;
                if (!shift)
                    sel_clear(ts);
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
                case 'v':
                case 'V':
                    lens_request_paste(ui);
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
                }
            }
        }

        /* Mouse: a press places the caret (and clears any selection); a
         * drag with the button held extends the selection from that anchor. */
        if (ui->input.mouse_pressed[LENS_MOUSE_LEFT] && r.hovered) {
            ts->cursor = (uint32_t)mouse_x_to_cursor(ui, buf, len, n, t->padding, ts->scroll_x,
                                                     t->font_size);
            sel_clear(ts);
        }
        if (ui->active_id == id && ui->input.mouse_down[LENS_MOUSE_LEFT] &&
            !ui->input.mouse_pressed[LENS_MOUSE_LEFT]) {
            ts->cursor = (uint32_t)mouse_x_to_cursor(ui, buf, len, n, t->padding, ts->scroll_x,
                                                     t->font_size);
        }
        if (ui->active_id == id && !ui->input.mouse_down[LENS_MOUSE_LEFT]) {
            ui->active_id = 0;
            if (ts->cursor == ts->sel_anchor)
                sel_clear(ts);
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

    if (changed)
        len = (buf && buf_cap) ? strlen(buf) : 0;

    /* ---------------------------------------------------------------- */
    /*  Draw commands                                                   */
    /* ---------------------------------------------------------------- */

    /* Background */
    lensi_drawlist_push(ui, n,
                        (lens_draw_cmd){.kind = LENS_DRAW_RECT,
                                        .rel = {0, 0, 0, 0},
                                        .color = t->color_bg,
                                        .radius = t->corner_radius});

    bool has_preedit = r.focused && buf && buf_cap > 1 && ui->input.preedit_utf8[0];

    /* Selection highlight (behind text) */
    bool has_sel = r.focused && sel_active(ts) && !has_preedit && buf && len;
    if (has_sel) {
        /* One rect for LTR; several where the selection crosses a direction
         * boundary (BiDi). */
        lens_text_xrange rects[8];
        int nr =
            lensi_text_sel_rects(ui, buf, sel_lo(ts), sel_hi(ts), t->font_size, 0.0f, rects, 8);
        for (int i = 0; i < nr; i++) {
            lensi_drawlist_push(ui, n,
                                (lens_draw_cmd){.kind = LENS_DRAW_RECT,
                                                .rel = {t->padding + rects[i].x0, text_y,
                                                        rects[i].x1 - rects[i].x0, fm.height},
                                                .color = lensi_color_alpha(t->color_accent, 0x40),
                                                .radius = 1.0f});
        }
    }

    /* Border */
    flux_color border_color = t->color_border;
    if (disabled)
        border_color = t->color_disabled;
    else if (error)
        border_color = t->color_error;
    else if (r.focused)
        border_color = t->color_accent;
    lensi_drawlist_push(ui, n,
                        (lens_draw_cmd){.kind = LENS_DRAW_BORDER,
                                        .rel = {0, 0, 0, 0},
                                        .color = border_color,
                                        .width = t->border_width,
                                        .radius = t->corner_radius});

    /* Caret X inside the widget (relative to text origin) */
    float base_caret_x = prefix_width(ui, buf, ts->cursor, t->font_size);
    float caret_x = base_caret_x;

    if (has_preedit) {
        const char *pe = ui->input.preedit_utf8;
        size_t pe_len = strlen(pe);

        /* Compose display string: buf[0..cursor) + preedit + buf[cursor..end) */
        size_t display_len = ts->cursor + pe_len + (len - ts->cursor);
        char *display = flux_arena_alloc(&ui->arena, display_len + 1);
        if (display) {
            if (ts->cursor)
                memcpy(display, buf, ts->cursor);
            if (pe_len)
                memcpy(display + ts->cursor, pe, pe_len);
            if (len > ts->cursor)
                memcpy(display + ts->cursor + pe_len, buf + ts->cursor, len - ts->cursor);
            display[display_len] = '\0';

            lensi_drawlist_push(ui, n,
                                (lens_draw_cmd){.kind = LENS_DRAW_TEXT,
                                                .rel = {t->padding, text_y, 0, 0},
                                                .color = t->color_fg,
                                                .text = display,
                                                .text_size = t->font_size});
        }

        /* Underline beneath the preedit region */
        float pe_width = prefix_width(ui, pe, pe_len, t->font_size);
        lensi_drawlist_push(ui, n,
                            (lens_draw_cmd){.kind = LENS_DRAW_RECT,
                                            .rel = {t->padding + base_caret_x,
                                                    text_y + fm.height - 1.0f, pe_width, 1.0f},
                                            .color = t->color_accent});

        /* Cursor is inside the preedit string */
        caret_x = base_caret_x + prefix_width(ui, pe, ui->input.preedit_cursor, t->font_size);
    } else if (buf && len) {
        lensi_drawlist_push(ui, n,
                            (lens_draw_cmd){.kind = LENS_DRAW_TEXT,
                                            .rel = {t->padding, text_y, 0, 0},
                                            .color = t->color_fg,
                                            .text = buf,
                                            .text_size = t->font_size});
    } else if (!r.focused && placeholder) {
        lensi_drawlist_push(ui, n,
                            (lens_draw_cmd){.kind = LENS_DRAW_TEXT,
                                            .rel = {t->padding, text_y, 0, 0},
                                            .color = t->color_disabled,
                                            .text = placeholder,
                                            .text_size = t->font_size});
    }

    /* Cursor */
    if (!disabled && r.focused) {
        lensi_drawlist_push(
            ui, n,
            (lens_draw_cmd){.kind = LENS_DRAW_RECT,
                            .rel = {t->padding + caret_x - 1.0f, text_y, 2.0f, fm.height},
                            .color = t->color_accent,
                            .radius = 1.0f});
    }

    /* Caret rectangle for the platform IME */
    if (!disabled && r.focused && buf && buf_cap > 1) {
        lensi_set_caret_rect(ui, (flux_rect){
                                     n->prev_rect.x + t->padding + caret_x,
                                     n->prev_rect.y + text_y,
                                     2.0f,
                                     fm.height,
                                 });
    }

    /* Semantics */
    uint32_t sem_flags = (r.focused ? LENS_A11Y_FOCUSED : 0) | (disabled ? LENS_A11Y_DISABLED : 0);
    lensi_node_semantics(ui, n, LENS_ROLE_TEXTFIELD, label, buf, sem_flags);

    ui->last_response = r;
    return changed;
}

lens_response lens_textfield_ex(lens *ui, lens_textfield_opts o) {
    lensi_apply_box(ui, o.box);
    if (o.placeholder)
        lensi_set_placeholder(ui, o.placeholder);
    bool scoped = o.box.id && o.box.id[0];
    if (scoped)
        lens_push_id(ui, o.box.id);
    lens_textfield(ui, o.label ? o.label : "", o.buf, o.buf_cap);
    if (scoped)
        lens_pop_id(ui);
    if (o.box.tooltip)
        lensi_tooltip(ui, o.box.tooltip);
    return ui->last_response;
}
