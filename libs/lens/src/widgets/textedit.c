/* textedit.c — unified single-line and multi-line text input widget (ADR-0082). */

#include "../internal.h"
#include <math.h>
#include <string.h>

#define LENS_TEXTEDIT_LINE_HEIGHT 1.4f
#define LENS_TEXTEDIT_SCROLL_SPEED 40.0f

/* ------------------------------------------------------------------ */
/*  UTF-8 and word boundary helpers                                   */
/* ------------------------------------------------------------------ */

static size_t utf8_char_len(const char *s, size_t pos) {
    unsigned char c = (unsigned char)s[pos];
    if ((c & 0x80) == 0)
        return 1;
    if ((c & 0xE0) == 0xC0)
        return 2;
    if ((c & 0xF0) == 0xE0)
        return 3;
    if ((c & 0xF8) == 0xF0)
        return 4;
    return 1;
}

static size_t prev_char_boundary(const char *s, size_t pos) {
    if (pos == 0)
        return 0;
    pos--;
    while (pos > 0 && ((unsigned char)s[pos] & 0xC0) == 0x80)
        pos--;
    return pos;
}

static size_t next_char_boundary(const char *s, size_t len, size_t pos) {
    if (pos >= len)
        return len;
    return pos + utf8_char_len(s, pos);
}

static bool is_word_char(char c) {
    unsigned char u = (unsigned char)c;
    return (u >= 'a' && u <= 'z') || (u >= 'A' && u <= 'Z') || (u >= '0' && u <= '9') || u == '_';
}

static size_t prev_word_boundary(const char *s, size_t pos) {
    while (pos > 0 && s[pos - 1] == ' ')
        pos--;
    while (pos > 0 && is_word_char(s[pos - 1]))
        pos--;
    return pos;
}

static size_t next_word_boundary(const char *s, size_t len, size_t pos) {
    while (pos < len && is_word_char(s[pos]))
        pos++;
    while (pos < len && s[pos] == ' ')
        pos++;
    return pos;
}

/* ------------------------------------------------------------------ */
/*  State & persistent tracking                                       */
/* ------------------------------------------------------------------ */

typedef struct lens_textedit_state {
    uint32_t cursor;
    uint32_t sel_anchor;
    float scroll_y;
    bool dragging;
    bool select_all_seeded;
} lens_textedit_state;

static inline bool sel_active(const lens_textedit_state *ts) {
    return ts->sel_anchor != UINT32_MAX && ts->sel_anchor != ts->cursor;
}

static inline uint32_t sel_lo(const lens_textedit_state *ts) {
    return ts->sel_anchor < ts->cursor ? ts->sel_anchor : ts->cursor;
}

static inline uint32_t sel_hi(const lens_textedit_state *ts) {
    return ts->sel_anchor > ts->cursor ? ts->sel_anchor : ts->cursor;
}

static inline void sel_clear(lens_textedit_state *ts) {
    ts->sel_anchor = UINT32_MAX;
}

/* ------------------------------------------------------------------ */
/*  Buffer mutation                                                   */
/* ------------------------------------------------------------------ */

static bool delete_range(char *buf, size_t *len, uint32_t lo, uint32_t hi) {
    if (lo >= hi || hi > *len)
        return false;
    memmove(buf + lo, buf + hi, *len - hi + 1);
    *len -= (hi - lo);
    return true;
}

static bool insert_text(char *buf, size_t *len, size_t cap, uint32_t pos, const char *ins,
                        size_t ins_len) {
    if (*len + ins_len >= cap)
        return false;
    memmove(buf + pos + ins_len, buf + pos, *len - pos + 1);
    memcpy(buf + pos, ins, ins_len);
    *len += ins_len;
    return true;
}

/* ------------------------------------------------------------------ */
/*  Multi-line line layout helpers                                    */
/* ------------------------------------------------------------------ */

static void find_line(const char *buf, size_t pos, size_t *line_start, int *line_index) {
    size_t start = 0;
    int idx = 0;
    for (size_t i = 0; i < pos; i++) {
        if (buf[i] == '\n') {
            start = i + 1;
            idx++;
        }
    }
    *line_start = start;
    *line_index = idx;
}

static size_t line_length(const char *buf, size_t len, size_t start) {
    size_t i = start;
    while (i < len && buf[i] != '\n')
        i++;
    return i - start;
}

static float prefix_width(lens *ui, const char *str, size_t len, float font_size) {
    if (!str || len == 0)
        return 0.0f;
    return flux_text_measure(ui->text, str, len,
                             &(flux_text_style){.size_px = font_size,
                                                .weight = 0.0f,
                                                .family = (flux_text_family)ui->text_family})
        .width;
}

/* ------------------------------------------------------------------ */
/*  Main lens_textedit widget                                         */
/* ------------------------------------------------------------------ */

lens_response lens_textedit(lens *ui, const lens_textedit_opts *opts) {
    lens_textedit_opts default_opts = {0};
    if (!opts)
        opts = &default_opts;

    lensi_apply_box(ui, opts->box);
    const lens_theme *t = &ui->theme;
    bool disabled = ui->next_disabled;
    bool error = ui->next_error || opts->box.error;
    const char *placeholder = opts->placeholder;
    ui->next_disabled = false;
    ui->next_error = false;
    ui->next_placeholder = NULL;
    lens_style eff = lensi_style_effective(ui);
    float font_size = lensi_style_font_size(ui, &eff, t);
    float padding = lensi_style_padding(&eff, t);

    const char *label = opts->box.id ? opts->box.id : "##textedit";
    lens_id id = lensi_gen_widget_id(ui, label);
    lens_node *n = lensi_store_touch(ui, id);
    if (!n)
        return (lens_response){0};
    lensi_link_child(ui, n);
    n->is_container = false;

    char *buf = opts->buf;
    size_t buf_cap = opts->cap;
    bool multiline = opts->multiline;

    lens_textedit_state *ts = lens_node_state(n, sizeof *ts);
    if (!ts)
        return (lens_response){0};

    if (buf && buf_cap > 0)
        buf[buf_cap - 1] = '\0';
    size_t len = buf ? strlen(buf) : 0;
    if (ts->cursor > len)
        ts->cursor = (uint32_t)len;
    if (ts->sel_anchor != UINT32_MAX && ts->sel_anchor > len)
        ts->sel_anchor = (uint32_t)len;

    float line_height = font_size * LENS_TEXTEDIT_LINE_HEIGHT;
    uint32_t rows = opts->rows > 0 ? opts->rows : (multiline ? 4 : 1);
    float min_h = multiline ? (float)rows * line_height : 0.0f;

    float h = (n->fixed_h > 0) ? n->fixed_h
                               : (multiline ? min_h + 2.0f * padding : font_size + 2.0f * padding);
    float w = (n->fixed_w > 0) ? n->fixed_w : 200.0f;
    n->measured = (flux_point){w, h};

    lens_response r = lensi_interact(ui, n, true, disabled);

    /* Selection on focus */
    if (r.focused && opts->select_all_on_focus && !ts->select_all_seeded && len > 0) {
        ts->sel_anchor = 0;
        ts->cursor = (uint32_t)len;
        ts->select_all_seeded = true;
    }
    if (!r.focused)
        ts->select_all_seeded = false;

    bool changed = false;

    /* Single-line vs Multi-line interaction handling */
    if (!disabled && r.focused && buf && buf_cap > 1 && !opts->readonly) {
        /* Direct character input */
        if (ui->input.text_utf8[0] != '\0') {
            size_t in_len = strlen(ui->input.text_utf8);
            if (sel_active(ts)) {
                uint32_t lo = sel_lo(ts), hi = sel_hi(ts);
                delete_range(buf, &len, lo, hi);
                ts->cursor = lo;
                sel_clear(ts);
            }
            if (insert_text(buf, &len, buf_cap, ts->cursor, ui->input.text_utf8, in_len)) {
                ts->cursor += (uint32_t)in_len;
                changed = true;
            }
        }

        /* Keyboard events */
        bool shift = (ui->input.mods & LENS_MOD_SHIFT) != 0;
        bool ctrl = (ui->input.mods & LENS_MOD_CTRL) != 0;
        for (uint32_t i = 0; i < ui->input.key_count; i++) {
            lens_key_event k = ui->input.keys[i];
            if (!k.pressed)
                continue;

            if (k.key == LENS_KEY_LEFT) {
                if (shift && ts->sel_anchor == UINT32_MAX)
                    ts->sel_anchor = ts->cursor;
                if (!shift && sel_active(ts)) {
                    ts->cursor = sel_lo(ts);
                    sel_clear(ts);
                } else if (ctrl) {
                    ts->cursor = (uint32_t)prev_word_boundary(buf, ts->cursor);
                } else {
                    ts->cursor = (uint32_t)prev_char_boundary(buf, ts->cursor);
                }
                if (!shift)
                    sel_clear(ts);
            } else if (k.key == LENS_KEY_RIGHT) {
                if (shift && ts->sel_anchor == UINT32_MAX)
                    ts->sel_anchor = ts->cursor;
                if (!shift && sel_active(ts)) {
                    ts->cursor = sel_hi(ts);
                    sel_clear(ts);
                } else if (ctrl) {
                    ts->cursor = (uint32_t)next_word_boundary(buf, len, ts->cursor);
                } else {
                    ts->cursor = (uint32_t)next_char_boundary(buf, len, ts->cursor);
                }
                if (!shift)
                    sel_clear(ts);
            } else if (k.key == LENS_KEY_HOME) {
                if (shift && ts->sel_anchor == UINT32_MAX)
                    ts->sel_anchor = ts->cursor;
                if (multiline && !ctrl) {
                    size_t ls;
                    int li;
                    find_line(buf, ts->cursor, &ls, &li);
                    ts->cursor = (uint32_t)ls;
                } else {
                    ts->cursor = 0;
                }
                if (!shift)
                    sel_clear(ts);
            } else if (k.key == LENS_KEY_END) {
                if (shift && ts->sel_anchor == UINT32_MAX)
                    ts->sel_anchor = ts->cursor;
                if (multiline && !ctrl) {
                    size_t ls;
                    int li;
                    find_line(buf, ts->cursor, &ls, &li);
                    ts->cursor = (uint32_t)(ls + line_length(buf, len, ls));
                } else {
                    ts->cursor = (uint32_t)len;
                }
                if (!shift)
                    sel_clear(ts);
            } else if (k.key == LENS_KEY_BACKSPACE) {
                if (sel_active(ts)) {
                    uint32_t lo = sel_lo(ts), hi = sel_hi(ts);
                    delete_range(buf, &len, lo, hi);
                    ts->cursor = lo;
                    sel_clear(ts);
                    changed = true;
                } else if (ts->cursor > 0) {
                    uint32_t prev = (uint32_t)(ctrl ? prev_word_boundary(buf, ts->cursor)
                                                    : prev_char_boundary(buf, ts->cursor));
                    delete_range(buf, &len, prev, ts->cursor);
                    ts->cursor = prev;
                    changed = true;
                }
            } else if (k.key == LENS_KEY_DELETE) {
                if (sel_active(ts)) {
                    uint32_t lo = sel_lo(ts), hi = sel_hi(ts);
                    delete_range(buf, &len, lo, hi);
                    ts->cursor = lo;
                    sel_clear(ts);
                    changed = true;
                } else if (ts->cursor < len) {
                    uint32_t nxt = (uint32_t)(ctrl ? next_word_boundary(buf, len, ts->cursor)
                                                   : next_char_boundary(buf, len, ts->cursor));
                    delete_range(buf, &len, ts->cursor, nxt);
                    changed = true;
                }
            } else if (k.key == LENS_KEY_RETURN) {
                if (multiline) {
                    if (sel_active(ts)) {
                        uint32_t lo = sel_lo(ts), hi = sel_hi(ts);
                        delete_range(buf, &len, lo, hi);
                        ts->cursor = lo;
                        sel_clear(ts);
                    }
                    if (insert_text(buf, &len, buf_cap, ts->cursor, "\n", 1)) {
                        ts->cursor++;
                        changed = true;
                    }
                }
            } else if (ctrl && (k.key == 'a' || k.key == 'A')) {
                ts->sel_anchor = 0;
                ts->cursor = (uint32_t)len;
            } else if (ctrl && (k.key == 'c' || k.key == 'C')) {
                if (sel_active(ts)) {
                    uint32_t lo = sel_lo(ts), hi = sel_hi(ts);
                    lens_copy(ui, buf + lo, hi - lo);
                }
            } else if (ctrl && (k.key == 'x' || k.key == 'X')) {
                if (sel_active(ts)) {
                    uint32_t lo = sel_lo(ts), hi = sel_hi(ts);
                    lens_copy(ui, buf + lo, hi - lo);
                    delete_range(buf, &len, lo, hi);
                    ts->cursor = lo;
                    sel_clear(ts);
                    changed = true;
                }
            } else if (ctrl && (k.key == 'v' || k.key == 'V')) {
                lens_request_paste(ui);
            }
        }

        /* Handle paste payload */
        uint32_t paste_len = 0;
        const char *paste = lensi_take_paste(ui, &paste_len);
        if (paste && paste_len > 0) {
            if (sel_active(ts)) {
                uint32_t lo = sel_lo(ts), hi = sel_hi(ts);
                delete_range(buf, &len, lo, hi);
                ts->cursor = lo;
                sel_clear(ts);
            }
            if (insert_text(buf, &len, buf_cap, ts->cursor, paste, paste_len)) {
                ts->cursor += (uint32_t)paste_len;
                changed = true;
            }
        }
    }

    /* Measure and layout display text */
    bool show_placeholder = (len == 0 && placeholder && placeholder[0]);
    const char *display_text = show_placeholder ? placeholder : (buf ? buf : "");
    float text_y = fmaxf((h - font_size) * 0.5f - 1.0f, 0.0f);
    lens_text_metrics fm = lensi_text_measure_label(ui, "Ag", font_size, 0.0f);

    float caret_x = 0.0f;
    if (r.focused && !show_placeholder && buf) {
        if (multiline) {
            size_t ls;
            int li;
            find_line(buf, ts->cursor, &ls, &li);
            caret_x = prefix_width(ui, buf + ls, ts->cursor - ls, font_size);
        } else {
            caret_x = prefix_width(ui, buf, ts->cursor, font_size);
        }
    }

    flux_rect caret_rect = {
        padding + caret_x,
        text_y,
        1.5f,
        fm.height > 0 ? fm.height : font_size,
    };

    /* Build text lines for multiline or single-line */
    lens_text_line *lines = NULL;
    int line_count = 0;
    if (multiline) {
        int line_cap = 8;
        lines = flux_arena_alloc(&ui->arena, (size_t)line_cap * sizeof *lines);
        size_t start = 0;
        int row_idx = 0;
        while (start <= len && lines) {
            size_t llen = line_length(display_text, len, start);
            char *line_str = flux_arena_alloc(&ui->arena, llen + 1);
            if (line_str) {
                memcpy(line_str, display_text + start, llen);
                line_str[llen] = '\0';
                lines[line_count++] = (lens_text_line){
                    .text = line_str,
                    .x = padding,
                    .y = padding + (float)row_idx * line_height,
                };
            }
            row_idx++;
            start += llen + 1;
            if (start > len && llen == 0 && row_idx > 1)
                break;
        }
    }

    /* Caret & selection geometry for skin */
    flux_rect sel_rect = {0};
    int sel_rect_count = 0;
    if (sel_active(ts) && !multiline) {
        uint32_t lo = sel_lo(ts), hi = sel_hi(ts);
        float x1 = prefix_width(ui, buf, lo, font_size);
        float x2 = prefix_width(ui, buf, hi, font_size);
        sel_rect =
            (flux_rect){padding + x1, text_y, x2 - x1, fm.height > 0 ? fm.height : font_size};
        sel_rect_count = 1;
    }

    /* Skin emission */
    lens_style_resolved rs = lensi_style_resolve(ui, &eff, t, r.state);
    lensi_skin_emit(ui, n,
                    &(lens_widget_record){
                        .kind = LENS_WIDGET_TEXTEDIT,
                        .state = r.state,
                        .bounds = {0, 0, w, h},
                        .last_bounds = n->prev_rect,
                        .style = rs,
                        .style_fields = eff.fields,
                        .content =
                            {
                                .multiline = multiline,
                                .edit_text = multiline ? NULL : display_text,
                                .edit_text_y = text_y,
                                .show_placeholder = show_placeholder,
                                .lines = lines,
                                .line_count = line_count,
                                .sel_rects = &sel_rect,
                                .sel_rect_count = sel_rect_count,
                                .caret = caret_rect,
                                .show_caret = r.focused && !disabled,
                                .error = error,
                            },
                    });

    /* Platform caret and context reporting */
    if (!disabled && r.focused && buf && buf_cap > 1) {
        lensi_set_caret_rect(ui, (flux_rect){
                                     n->prev_rect.x + caret_rect.x,
                                     n->prev_rect.y + caret_rect.y,
                                     2.0f,
                                     caret_rect.h,
                                 });
        lensi_set_text_context(ui, buf, (uint32_t)len, ts->cursor, multiline);
    }

    uint32_t sem_flags = (r.focused ? LENS_A11Y_FOCUSED : 0) | (disabled ? LENS_A11Y_DISABLED : 0) |
                         (opts->readonly ? LENS_A11Y_READONLY : 0);
    lensi_node_semantics(ui, n, multiline ? LENS_ROLE_TEXTAREA : LENS_ROLE_TEXTFIELD, label, buf,
                         sem_flags);

    r.changed = changed;
    ui->last_response = r;
    return r;
}

void lens_textedit_set_caret(lens *ui, const char *label, uint32_t caret) {
    if (!ui || !label)
        return;
    lens_node *n = lens_find(ui, lens_current_id(ui, label));
    if (!n)
        return;
    lens_textedit_state *ts = lens_node_state(n, sizeof *ts);
    if (ts) {
        ts->cursor = caret;
        sel_clear(ts);
    }
}

void lens_textedit_set_selection(lens *ui, const char *label, uint32_t sel_start,
                                 uint32_t sel_end) {
    if (!ui || !label)
        return;
    lens_node *n = lens_find(ui, lens_current_id(ui, label));
    if (!n)
        return;
    lens_textedit_state *ts = lens_node_state(n, sizeof *ts);
    if (ts) {
        ts->sel_anchor = sel_start;
        ts->cursor = sel_end;
    }
}
