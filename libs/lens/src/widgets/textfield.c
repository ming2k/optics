/* textfield.c — single-line text input widget with selection (ADR-0031). */

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

/* Back `pos` off to the start of its code point — a no-op when it already
 * sits on a boundary (or at 0, which covers the empty buffer). */
static size_t utf8_snap_boundary(const char *s, size_t pos) {
    while (pos > 0 && ((unsigned char)s[pos] & 0xc0) == 0x80)
        pos--;
    return pos;
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
    lens_style eff = lensi_style_effective(ui);
    float font_size = lensi_style_font_size(&eff, t);
    float padding = lensi_style_padding(&eff, t);
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
    /* Host-set offsets (lens_textfield_set_caret / _set_selection) can land
     * mid-character; snap back to a code-point boundary. Widget-written
     * offsets are always boundaries, so this is a no-op for them. */
    if (buf) {
        ts->cursor = (uint32_t)utf8_snap_boundary(buf, ts->cursor);
        ts->sel_anchor = (uint32_t)utf8_snap_boundary(buf, ts->sel_anchor);
    }

    /* ---- Measure --------------------------------------------------- */
    lens_text_metrics tm = lensi_text_measure_label(ui, buf ? buf : "", font_size, 0.0f);
    lens_text_metrics fm = lensi_text_measure_label(ui, "Ag", font_size, 0.0f);
    float min_w = 160.0f;
    float w = (n->fixed_w > 0) ? n->fixed_w : fmaxf(tm.width + 2.0f * padding, min_w);
    float h = (n->fixed_h > 0) ? n->fixed_h : fm.height + 2.0f * padding;
    n->measured = (flux_point){w, h};

    float text_y = (h - fm.height) * 0.5f;
    if (text_y < 0.0f)
        text_y = 0.0f;

    /* ---- Interaction ----------------------------------------------- */
    lens_response r = lensi_interact(ui, n, true, disabled);
    lens_style_resolved rs = lensi_style_resolve(&eff, t, r.state);
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
                size_t nv = lensi_text_caret_visual(ui, buf, ts->cursor, false, font_size, 0.0f);
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
                size_t nv = lensi_text_caret_visual(ui, buf, ts->cursor, true, font_size, 0.0f);
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
            ts->cursor =
                (uint32_t)mouse_x_to_cursor(ui, buf, len, n, padding, ts->scroll_x, font_size);
            sel_clear(ts);
        }
        if (ui->active_id == id && ui->input.mouse_down[LENS_MOUSE_LEFT] &&
            !ui->input.mouse_pressed[LENS_MOUSE_LEFT]) {
            ts->cursor =
                (uint32_t)mouse_x_to_cursor(ui, buf, len, n, padding, ts->scroll_x, font_size);
        }
        if (ui->active_id == id && !ui->input.mouse_down[LENS_MOUSE_LEFT]) {
            ui->active_id = 0;
            if (ts->cursor == ts->sel_anchor)
                sel_clear(ts);
        }

        /* IME delete_surrounding_text: applied BEFORE the commit string per
         * the text-input-v3 protocol, so the freshly committed text lands in
         * the right place relative to the IME's expected context window. */
        if (ui->input.ime_delete_before || ui->input.ime_delete_after) {
            uint32_t lo = ts->cursor >= ui->input.ime_delete_before
                              ? ts->cursor - ui->input.ime_delete_before
                              : 0;
            uint32_t hi = ts->cursor + ui->input.ime_delete_after;
            if (hi > len)
                hi = (uint32_t)len;
            if (hi > lo) {
                delete_range(buf, &len, lo, hi);
                ts->cursor = lo;
                if (ts->sel_anchor > hi)
                    ts->sel_anchor -= (hi - lo);
                else if (ts->sel_anchor > lo)
                    ts->sel_anchor = lo;
                changed = true;
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

    if (changed)
        len = (buf && buf_cap) ? strlen(buf) : 0;

    /* ---------------------------------------------------------------- */
    /*  Skin record precompute (all node-local; the skin never shapes)  */
    /* ---------------------------------------------------------------- */

    bool has_preedit = r.focused && buf && buf_cap > 1 && ui->input.preedit_utf8[0];

    /* Selection highlight quads (one for LTR; several where the selection
     * crosses a BiDi direction boundary). */
    flux_rect sel_rects[8];
    int sel_count = 0;
    bool has_sel = r.focused && sel_active(ts) && !has_preedit && buf && len;
    if (has_sel) {
        lens_text_xrange xr[8];
        int nr = lensi_text_sel_rects(ui, buf, sel_lo(ts), sel_hi(ts), font_size, 0.0f, xr, 8);
        for (int i = 0; i < nr; i++)
            sel_rects[sel_count++] =
                (flux_rect){padding + xr[i].x0, text_y, xr[i].x1 - xr[i].x0, fm.height};
    }

    /* Caret X inside the widget (relative to text origin) */
    float base_caret_x = prefix_width(ui, buf, ts->cursor, font_size);
    float caret_x = base_caret_x;

    const char *display_text = NULL;
    bool show_placeholder = false;
    flux_rect preedit_underline = {0, 0, 0, 0};
    flux_rect preedit_clause = {0, 0, 0, 0};

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
            display_text = display;
        }

        /* Underline beneath the preedit region */
        float pe_width = prefix_width(ui, pe, pe_len, font_size);
        preedit_underline =
            (flux_rect){padding + base_caret_x, text_y + fm.height - 1.0f, pe_width, 1.0f};

        /* Active clause of the composition (the IME's preedit_sel range) */
        uint32_t clause_lo = ui->input.preedit_sel_lo;
        uint32_t clause_hi = ui->input.preedit_sel_hi;
        if (clause_hi > clause_lo && clause_hi <= pe_len) {
            float clause_x = prefix_width(ui, pe, clause_lo, font_size);
            preedit_clause =
                (flux_rect){padding + base_caret_x + clause_x, text_y + fm.height - 1.0f,
                            prefix_width(ui, pe, clause_hi, font_size) - clause_x, 2.0f};
        }

        /* Cursor is inside the preedit string */
        caret_x = base_caret_x + prefix_width(ui, pe, ui->input.preedit_cursor, font_size);
    } else if (buf && len) {
        display_text = buf;
    } else if (!r.focused && placeholder) {
        display_text = placeholder;
        show_placeholder = true;
    }

    bool show_caret = !disabled && r.focused;
    flux_rect caret = {padding + caret_x - 1.0f, text_y, 2.0f, fm.height};

    /* emit — through the replaceable skin (ADR-0059). The arena array is
     * copied into the per-frame arena by the record walk below, so the
     * stack staging buffer stays local. */
    flux_rect *sel_out = NULL;
    if (sel_count > 0) {
        sel_out = flux_arena_alloc(&ui->arena, (size_t)sel_count * sizeof *sel_out);
        if (sel_out)
            memcpy(sel_out, sel_rects, (size_t)sel_count * sizeof *sel_out);
        else
            lensi_set_overflow(ui);
    }
    lensi_skin_emit(ui, n,
                    &(lens_widget_record){
                        .kind = LENS_WIDGET_TEXTFIELD,
                        .state = r.state,
                        .bounds = {0, 0, w, h},
                        .last_bounds = n->prev_rect,
                        .style = rs,
                        .style_fields = eff.fields,
                        .hover_t = n->hover_t,
                        .active_t = n->active_t,
                        .content = {.label = label,
                                    .text = fm,
                                    .error = error,
                                    .edit_text = display_text,
                                    .edit_text_y = text_y,
                                    .show_placeholder = show_placeholder,
                                    .sel_rects = sel_out,
                                    .sel_rect_count = sel_out ? sel_count : 0,
                                    .caret = caret,
                                    .show_caret = show_caret,
                                    .preedit_underline = preedit_underline,
                                    .has_preedit = has_preedit,
                                    .preedit_clause = preedit_clause},
                    });

    /* Caret rect and surrounding-text context for the platform IME
     * (behaviour, not chrome — stays) */
    if (!disabled && r.focused && buf && buf_cap > 1) {
        lensi_set_caret_rect(ui, (flux_rect){
                                     n->prev_rect.x + padding + caret_x,
                                     n->prev_rect.y + text_y,
                                     2.0f,
                                     fm.height,
                                 });
        lensi_set_text_context(ui, buf, (uint32_t)len, ts->cursor, false);
    }

    /* Semantics */
    uint32_t sem_flags = (r.focused ? LENS_A11Y_FOCUSED : 0) | (disabled ? LENS_A11Y_DISABLED : 0);
    lensi_node_semantics(ui, n, LENS_ROLE_TEXTFIELD, label, buf, sem_flags);

    /* The descriptor form reports the edit through lens_response.changed. */
    r.changed = changed;
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

/* ------------------------------------------------------------------ */
/*  Host caret / selection control (ADR-0064)                          */
/* ------------------------------------------------------------------ */

void lens_textfield_set_selection(lens *ui, const char *label, uint32_t anchor, uint32_t caret) {
    if (!ui || !label)
        return;
    lens_node *n = lensi_store_touch(ui, lensi_gen_widget_id(ui, label));
    if (!n)
        return;
    lens_textfield_state *ts = lens_node_state(n, sizeof *ts);
    if (!ts)
        return;
    /* Unconditional: the host's write wins over the field's remembered
     * position. Range/character repair is deferred to the next
     * lens_textfield build, which clamps to the buffer length and snaps
     * mid-character offsets back to a UTF-8 boundary. */
    ts->sel_anchor = anchor;
    ts->cursor = caret;
}

void lens_textfield_set_caret(lens *ui, const char *label, uint32_t caret) {
    lens_textfield_set_selection(ui, label, caret, caret);
}
