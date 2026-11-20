/* textedit.c — unified single-line and multi-line text input widget (ADR-0082). */

#include "../internal.h"
#include <math.h>
#include <string.h>

#define OPTICS_UI_TEXTEDIT_LINE_HEIGHT 1.4f
#define OPTICS_UI_TEXTEDIT_SCROLL_SPEED 40.0f

/* ------------------------------------------------------------------ */
/*  UTF-8 and word boundary helpers                                   */
/* ------------------------------------------------------------------ */

static inline bool is_utf8_continuation(unsigned char b) {
    return (b & 0xC0) == 0x80;
}

static size_t prev_char_boundary(const char *s, size_t pos) {
    if (pos == 0)
        return 0;
    pos--;
    while (pos > 0 && is_utf8_continuation((unsigned char)s[pos]))
        pos--;
    return pos;
}

static size_t next_char_boundary(const char *s, size_t len, size_t pos) {
    if (pos >= len)
        return len;
    pos++;
    while (pos < len && is_utf8_continuation((unsigned char)s[pos]))
        pos++;
    return pos;
}

static inline bool is_word_char(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' ||
           ((unsigned char)c >= 0x80);
}

static size_t prev_word_boundary(const char *s, size_t pos) {
    if (pos == 0)
        return 0;
    while (pos > 0 && (s[pos - 1] == ' ' || s[pos - 1] == '\t' || s[pos - 1] == '\n'))
        pos--;
    if (pos == 0)
        return 0;
    if (is_word_char(s[pos - 1])) {
        while (pos > 0 && is_word_char(s[pos - 1])) {
            pos--;
            while (pos > 0 && is_utf8_continuation((unsigned char)s[pos]))
                pos--;
        }
    } else {
        while (pos > 0 && s[pos - 1] != ' ' && s[pos - 1] != '\t' && s[pos - 1] != '\n' &&
               !is_word_char(s[pos - 1])) {
            pos--;
            while (pos > 0 && is_utf8_continuation((unsigned char)s[pos]))
                pos--;
        }
        if (pos > 0 && is_word_char(s[pos - 1])) {
            while (pos > 0 && is_word_char(s[pos - 1])) {
                pos--;
                while (pos > 0 && is_utf8_continuation((unsigned char)s[pos]))
                    pos--;
            }
        }
    }
    return pos;
}

static size_t next_word_boundary(const char *s, size_t len, size_t pos) {
    if (pos >= len)
        return len;
    while (pos < len && (s[pos] == ' ' || s[pos] == '\t' || s[pos] == '\n'))
        pos++;
    if (pos >= len)
        return len;
    if (is_word_char(s[pos])) {
        while (pos < len && is_word_char(s[pos])) {
            pos++;
            while (pos < len && is_utf8_continuation((unsigned char)s[pos]))
                pos++;
        }
    } else {
        while (pos < len && s[pos] != ' ' && s[pos] != '\t' && s[pos] != '\n' &&
               !is_word_char(s[pos])) {
            pos++;
            while (pos < len && is_utf8_continuation((unsigned char)s[pos]))
                pos++;
        }
        if (pos < len && is_word_char(s[pos])) {
            while (pos < len && is_word_char(s[pos])) {
                pos++;
                while (pos < len && is_utf8_continuation((unsigned char)s[pos]))
                    pos++;
            }
        }
    }
    while (pos < len && s[pos] == ' ')
        pos++;
    return pos;
}

static void word_boundaries_at(const char *s, size_t len, size_t pos, size_t *out_start,
                               size_t *out_end) {
    if (len == 0) {
        *out_start = 0;
        *out_end = 0;
        return;
    }
    if (pos > len)
        pos = len;

    if (pos > 0 && (pos == len || s[pos] == ' ' || s[pos] == '\t' || s[pos] == '\n')) {
        if (is_word_char(s[pos - 1])) {
            pos--;
            while (pos > 0 && is_utf8_continuation((unsigned char)s[pos]))
                pos--;
        }
    }

    if (pos < len && is_word_char(s[pos])) {
        size_t start = pos;
        while (start > 0 && is_word_char(s[start - 1])) {
            start--;
            while (start > 0 && is_utf8_continuation((unsigned char)s[start]))
                start--;
        }
        size_t end = pos;
        while (end < len && is_word_char(s[end])) {
            end++;
            while (end < len && is_utf8_continuation((unsigned char)s[end]))
                end++;
        }
        *out_start = start;
        *out_end = end;
    } else if (pos < len && (s[pos] == ' ' || s[pos] == '\t' || s[pos] == '\n')) {
        size_t start = pos;
        while (start > 0 && (s[start - 1] == ' ' || s[start - 1] == '\t'))
            start--;
        size_t end = pos;
        while (end < len && (s[end] == ' ' || s[end] == '\t'))
            end++;
        *out_start = start;
        *out_end = end;
    } else if (pos < len) {
        size_t start = pos;
        char punc = s[pos];
        while (start > 0 && s[start - 1] == punc)
            start--;
        size_t end = pos;
        while (end < len && s[end] == punc)
            end++;
        *out_start = start;
        *out_end = end;
    } else {
        *out_start = pos;
        *out_end = pos;
    }
}

#if defined(_WIN32)
#include <windows.h>
static inline uint64_t textedit_now_ms(void) {
    return (uint64_t)GetTickCount64();
}
#else
#include <time.h>
static inline uint64_t textedit_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1000000ULL;
}
#endif

/* ------------------------------------------------------------------ */
/*  State & persistent tracking                                       */
/* ------------------------------------------------------------------ */

#define OPTICS_UI_TEXTEDIT_UNDO_MAX 32
#define OPTICS_UI_TEXTEDIT_UNDO_POOL_SIZE 4096

typedef struct optics_ui_undo_record {
    uint32_t pos;
    uint32_t del_len;
    uint32_t ins_len;
    uint32_t del_offset;
    uint32_t ins_offset;
    uint32_t cursor_before;
    uint32_t sel_before;
    uint32_t cursor_after;
    uint32_t sel_after;
    uint64_t timestamp_ms;
    bool can_coalesce;
} optics_ui_undo_record;

typedef struct optics_ui_undo_stack {
    optics_ui_undo_record records[OPTICS_UI_TEXTEDIT_UNDO_MAX];
    char pool[OPTICS_UI_TEXTEDIT_UNDO_POOL_SIZE];
    uint32_t pool_head;
    uint32_t count;
    uint32_t index;
} optics_ui_undo_stack;

typedef struct optics_ui_textedit_state {
    uint32_t cursor;
    uint32_t sel_anchor;
    float scroll_y;
    bool dragging;
    bool select_all_seeded;

    uint64_t last_click_ms;
    float last_click_x;
    float last_click_y;
    int click_count;
    bool word_select_mode;
    bool line_select_mode;
    uint32_t sel_pivot_lo;
    uint32_t sel_pivot_hi;

    optics_ui_undo_stack undo;
} optics_ui_textedit_state;

static inline bool sel_active(const optics_ui_textedit_state *ts) {
    return ts->sel_anchor != UINT32_MAX && ts->sel_anchor != ts->cursor;
}

static inline uint32_t sel_lo(const optics_ui_textedit_state *ts) {
    return ts->sel_anchor < ts->cursor ? ts->sel_anchor : ts->cursor;
}

static inline uint32_t sel_hi(const optics_ui_textedit_state *ts) {
    return ts->sel_anchor > ts->cursor ? ts->sel_anchor : ts->cursor;
}

static inline void sel_clear(optics_ui_textedit_state *ts) {
    ts->sel_anchor = UINT32_MAX;
}

/* ------------------------------------------------------------------ */
/*  Buffer mutation helpers                                           */
/* ------------------------------------------------------------------ */

static void delete_range(char *buf, size_t *len, size_t lo, size_t hi) {
    if (lo >= hi || hi > *len)
        return;
    memmove(buf + lo, buf + hi, *len - hi + 1);
    *len -= (hi - lo);
}

static bool insert_text(char *buf, size_t *len, size_t cap, size_t pos, const char *ins,
                        size_t ins_len) {
    if (*len + ins_len >= cap)
        return false;
    memmove(buf + pos + ins_len, buf + pos, *len - pos + 1);
    memcpy(buf + pos, ins, ins_len);
    *len += ins_len;
    return true;
}

static void undo_record_edit(optics_ui_textedit_state *ts, const char *buf, size_t pos,
                             size_t del_len, const char *ins_text, size_t ins_len,
                             uint32_t cursor_before, uint32_t sel_before, uint32_t cursor_after,
                             uint32_t sel_after, uint64_t now_ms, bool typing) {
    if (ts->undo.index < ts->undo.count) {
        ts->undo.count = ts->undo.index;
    }

    if (typing && ts->undo.index > 0) {
        optics_ui_undo_record *top = &ts->undo.records[ts->undo.index - 1];
        if (top->can_coalesce && (now_ms - top->timestamp_ms < 1000) && del_len == 0 &&
            ins_len == 1 && ins_text && ins_text[0] != ' ' && ins_text[0] != '\n' &&
            ins_text[0] != '\t' && pos == top->pos + top->ins_len &&
            top->ins_offset + top->ins_len == ts->undo.pool_head &&
            ts->undo.pool_head < OPTICS_UI_TEXTEDIT_UNDO_POOL_SIZE) {
            ts->undo.pool[ts->undo.pool_head++] = ins_text[0];
            top->ins_len++;
            top->cursor_after = cursor_after;
            top->sel_after = sel_after;
            top->timestamp_ms = now_ms;
            return;
        }
    }

    if (ts->undo.pool_head + del_len + ins_len > OPTICS_UI_TEXTEDIT_UNDO_POOL_SIZE) {
        ts->undo.pool_head = 0;
        ts->undo.count = 0;
        ts->undo.index = 0;
    }

    if (ts->undo.index >= OPTICS_UI_TEXTEDIT_UNDO_MAX) {
        memmove(&ts->undo.records[0], &ts->undo.records[1],
                (OPTICS_UI_TEXTEDIT_UNDO_MAX - 1) * sizeof(optics_ui_undo_record));
        ts->undo.index = OPTICS_UI_TEXTEDIT_UNDO_MAX - 1;
        ts->undo.count = OPTICS_UI_TEXTEDIT_UNDO_MAX - 1;
    }

    optics_ui_undo_record *r = &ts->undo.records[ts->undo.index];
    r->pos = (uint32_t)pos;
    r->del_len = (uint32_t)del_len;
    r->ins_len = (uint32_t)ins_len;
    r->cursor_before = cursor_before;
    r->sel_before = sel_before;
    r->cursor_after = cursor_after;
    r->sel_after = sel_after;
    r->timestamp_ms = now_ms;
    r->can_coalesce = typing && (del_len == 0) && (ins_len == 1) && ins_text &&
                      (ins_text[0] != ' ') && (ins_text[0] != '\n') && (ins_text[0] != '\t');

    r->del_offset = ts->undo.pool_head;
    if (del_len > 0) {
        memcpy(&ts->undo.pool[ts->undo.pool_head], buf + pos, del_len);
        ts->undo.pool_head += (uint32_t)del_len;
    }

    r->ins_offset = ts->undo.pool_head;
    if (ins_len > 0 && ins_text) {
        memcpy(&ts->undo.pool[ts->undo.pool_head], ins_text, ins_len);
        ts->undo.pool_head += (uint32_t)ins_len;
    }

    ts->undo.index++;
    ts->undo.count = ts->undo.index;
}

static bool undo_apply(optics_ui_textedit_state *ts, char *buf, size_t *len, size_t buf_cap) {
    if (ts->undo.index == 0)
        return false;

    ts->undo.index--;
    const optics_ui_undo_record *r = &ts->undo.records[ts->undo.index];
    if (r->pos + r->ins_len > *len)
        return false;

    if (*len - r->ins_len + r->del_len >= buf_cap)
        return false;

    delete_range(buf, len, r->pos, r->pos + r->ins_len);
    if (r->del_len > 0) {
        insert_text(buf, len, buf_cap, r->pos, &ts->undo.pool[r->del_offset], r->del_len);
    }
    ts->cursor = r->cursor_before;
    ts->sel_anchor = r->sel_before;
    return true;
}

static bool redo_apply(optics_ui_textedit_state *ts, char *buf, size_t *len, size_t buf_cap) {
    if (ts->undo.index >= ts->undo.count)
        return false;

    const optics_ui_undo_record *r = &ts->undo.records[ts->undo.index];
    ts->undo.index++;
    if (r->pos + r->del_len > *len)
        return false;

    if (*len - r->del_len + r->ins_len >= buf_cap)
        return false;

    delete_range(buf, len, r->pos, r->pos + r->del_len);
    if (r->ins_len > 0) {
        insert_text(buf, len, buf_cap, r->pos, &ts->undo.pool[r->ins_offset], r->ins_len);
    }
    ts->cursor = r->cursor_after;
    ts->sel_anchor = r->sel_after;
    return true;
}

static bool textedit_replace_range(optics_ui_textedit_state *ts, char *buf, size_t *len,
                                   size_t buf_cap, size_t lo, size_t hi, const char *ins,
                                   size_t ins_len, uint32_t cursor_after, uint32_t sel_after,
                                   uint64_t now_ms, bool typing) {
    uint32_t cursor_before = ts->cursor;
    uint32_t sel_before = ts->sel_anchor;
    size_t del_len = (hi > lo) ? (hi - lo) : 0;

    if (*len - del_len + ins_len >= buf_cap)
        return false;

    undo_record_edit(ts, buf, lo, del_len, ins, ins_len, cursor_before, sel_before, cursor_after,
                     sel_after, now_ms, typing);

    if (del_len > 0) {
        delete_range(buf, len, lo, hi);
    }
    if (ins_len > 0 && ins) {
        insert_text(buf, len, buf_cap, lo, ins, ins_len);
    }

    ts->cursor = cursor_after;
    ts->sel_anchor = sel_after;
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

static float prefix_width(optics_ui *ui, const char *str, size_t len, float font_size) {
    if (!str || len == 0)
        return 0.0f;
    return optics_text_measure(
               ui->text,
               &(optics_text_measure_opts){
                   .utf8 = str,
                   .len = len,
                   .style = &(optics_text_style){.size_px = font_size,
                                                 .weight = 0.0f,
                                                 .family = (optics_text_family)ui->text_family},
               })
        .width;
}

static size_t hit_test_line(optics_ui *ui, const char *line, size_t line_len, float font_size,
                            float click_x) {
    if (!line || line_len == 0 || click_x <= 0.0f)
        return 0;
    size_t pos = 0;
    float prev_x = 0.0f;
    while (pos < line_len) {
        size_t next = next_char_boundary(line, line_len, pos);
        float next_x = prefix_width(ui, line, next, font_size);
        if (click_x < (prev_x + next_x) * 0.5f)
            return pos;
        prev_x = next_x;
        pos = next;
    }
    return line_len;
}

/* ------------------------------------------------------------------ */
/*  Main lens_textedit widget                                         */
/* ------------------------------------------------------------------ */

optics_ui_response optics_ui_textedit(optics_ui *ui, const optics_ui_textedit_opts *opts) {
    optics_ui_textedit_opts default_opts = {};
    if (!opts)
        opts = &default_opts;

    lensi_apply_box(ui, opts->box);
    const optics_ui_theme *t = &ui->theme;
    bool disabled = ui->next_disabled;
    for (optics_ui_node *parent = lensi_open_container(ui); parent; parent = parent->parent)
        disabled = disabled || parent->box_disabled;
    bool error = ui->next_error || opts->box.error;
    const char *placeholder = opts->placeholder;
    ui->next_disabled = false;
    ui->next_error = false;
    ui->next_placeholder = nullptr;
    optics_ui_style eff = lensi_style_effective(ui);
    float font_size = lensi_style_font_size(ui, &eff, t);
    float padding = lensi_style_padding(&eff, t);

    const char *label = opts->box.id ? opts->box.id : "##textedit";
    optics_ui_id id = lensi_gen_widget_id(ui, label);
    optics_ui_node *n = lensi_store_touch(ui, id);
    if (!n)
        return (optics_ui_response){};
    lensi_link_child(ui, n);
    lensi_node_box(ui, n, &opts->box);
    n->is_container = false;

    char *buf = opts->buf;
    size_t buf_cap = opts->cap;
    bool multiline = opts->multiline;

    optics_ui_textedit_state *ts = optics_ui_node_state(n, sizeof *ts);
    if (!ts)
        return (optics_ui_response){};

    if (buf && buf_cap > 0)
        buf[buf_cap - 1] = '\0';
    size_t len = buf ? strlen(buf) : 0;
    if (ts->cursor > len)
        ts->cursor = (uint32_t)len;
    if (ts->sel_anchor != UINT32_MAX && ts->sel_anchor > len)
        ts->sel_anchor = (uint32_t)len;

    optics_ui_text_metrics fm = lensi_text_measure_label(ui, "Ag", font_size, 0.0f);
    float line_h = fm.height > 0.0f ? fm.height : font_size;
    float line_height = line_h * OPTICS_UI_TEXTEDIT_LINE_HEIGHT;
    uint32_t rows = opts->rows > 0 ? opts->rows : (multiline ? 4 : 1);
    float min_h = multiline ? (float)rows * line_height : 0.0f;

    float h = (n->fixed_h > 0) ? n->fixed_h
                               : (multiline ? min_h + 2.0f * padding : line_h + 2.0f * padding);
    float w = (n->fixed_w > 0) ? n->fixed_w : 200.0f;
    n->measured = (optics_gfx_point){w, h};

    optics_ui_response r = lensi_interact(ui, n, true, disabled);

    /* Text cursor hint on hover or drag */
    if (!disabled && (r.hovered || ts->dragging))
        ui->cursor_hint = OPTICS_UI_CURSOR_TEXT;

    optics_ui_style_resolved rs = lensi_style_resolve(ui, &eff, t, r.state);

    /* Concentric padding: when corner radius is large (e.g. pill/capsule),
     * inset the text horizontally so it never collides with the circular corner arc. */
    float pad_x = padding;
    if (rs.corner_radius > padding) {
        /* Inset concentric to the rounded corner arc */
        float arc_inset = rs.corner_radius * 0.8f;
        if (arc_inset > pad_x)
            pad_x = arc_inset;
    }

    /* Mouse click & drag interaction */
    if (!disabled && buf && buf_cap > 1 && !opts->readonly) {
        bool mouse_down = ui->input.mouse_down[OPTICS_UI_MOUSE_LEFT];
        bool mouse_pressed = ui->input.mouse_pressed[OPTICS_UI_MOUSE_LEFT];

        if (r.pressed && mouse_pressed) {
            uint64_t now_ms = textedit_now_ms();
            float click_x = ui->input.cursor.x;
            float click_y = ui->input.cursor.y;
            bool is_multiclick =
                (ts->last_click_ms > 0 && now_ms >= ts->last_click_ms &&
                 now_ms - ts->last_click_ms <= 400 && fabsf(click_x - ts->last_click_x) <= 5.0f &&
                 fabsf(click_y - ts->last_click_y) <= 5.0f);
            if (is_multiclick) {
                ts->click_count = (ts->click_count % 3) + 1;
            } else {
                ts->click_count = 1;
            }
            ts->last_click_ms = now_ms;
            ts->last_click_x = click_x;
            ts->last_click_y = click_y;

            if (ts->click_count == 1 && opts->select_all_on_focus && !ts->select_all_seeded &&
                len > 0) {
                ts->sel_anchor = 0;
                ts->cursor = (uint32_t)len;
                ts->select_all_seeded = true;
                ts->dragging = false;
                ts->word_select_mode = false;
                ts->line_select_mode = false;
            } else {
                float local_x = ui->input.cursor.x - (n->prev_rect.x + pad_x);
                uint32_t target_cursor = 0;
                if (multiline) {
                    float local_y = ui->input.cursor.y - (n->prev_rect.y + padding - ts->scroll_y);
                    int target_line = (int)floorf(local_y / line_height);
                    if (target_line < 0)
                        target_line = 0;
                    size_t ls = 0;
                    int li = 0;
                    while (li < target_line && ls < len) {
                        size_t llen = line_length(buf, len, ls);
                        ls += llen;
                        if (ls < len && buf[ls] == '\n') {
                            ls++;
                            li++;
                        }
                    }
                    size_t llen = line_length(buf, len, ls);
                    target_cursor =
                        (uint32_t)(ls + hit_test_line(ui, buf + ls, llen, font_size, local_x));
                } else {
                    target_cursor = (uint32_t)hit_test_line(ui, buf, len, font_size, local_x);
                }

                bool shift = (ui->input.mods & OPTICS_UI_MOD_SHIFT) != 0;

                if (ts->click_count == 2) {
                    /* Double-click: select word under cursor */
                    size_t ws = 0, we = 0;
                    word_boundaries_at(buf, len, target_cursor, &ws, &we);
                    ts->sel_anchor = (uint32_t)ws;
                    ts->cursor = (uint32_t)we;
                    ts->sel_pivot_lo = (uint32_t)ws;
                    ts->sel_pivot_hi = (uint32_t)we;
                    ts->dragging = true;
                    ts->word_select_mode = true;
                    ts->line_select_mode = false;
                } else if (ts->click_count == 3) {
                    /* Triple-click: select line or whole buffer */
                    size_t ls = 0, le = len;
                    if (multiline) {
                        int li = 0;
                        find_line(buf, target_cursor, &ls, &li);
                        le = ls + line_length(buf, len, ls);
                    }
                    ts->sel_anchor = (uint32_t)ls;
                    ts->cursor = (uint32_t)le;
                    ts->sel_pivot_lo = (uint32_t)ls;
                    ts->sel_pivot_hi = (uint32_t)le;
                    ts->dragging = true;
                    ts->word_select_mode = false;
                    ts->line_select_mode = true;
                } else {
                    /* Single click */
                    ts->word_select_mode = false;
                    ts->line_select_mode = false;
                    if (shift) {
                        if (ts->sel_anchor == UINT32_MAX)
                            ts->sel_anchor = ts->cursor;
                        ts->cursor = target_cursor;
                    } else {
                        ts->cursor = target_cursor;
                        ts->sel_anchor = target_cursor;
                        ts->dragging = true;
                    }
                }
            }
        } else if (ts->dragging) {
            if (mouse_down) {
                float local_x = ui->input.cursor.x - (n->prev_rect.x + pad_x);
                uint32_t target_cursor = 0;
                if (multiline) {
                    float local_y = ui->input.cursor.y - (n->prev_rect.y + padding - ts->scroll_y);
                    int target_line = (int)floorf(local_y / line_height);
                    if (target_line < 0)
                        target_line = 0;
                    size_t ls = 0;
                    int li = 0;
                    while (li < target_line && ls < len) {
                        size_t llen = line_length(buf, len, ls);
                        ls += llen;
                        if (ls < len && buf[ls] == '\n') {
                            ls++;
                            li++;
                        }
                    }
                    size_t llen = line_length(buf, len, ls);
                    target_cursor =
                        (uint32_t)(ls + hit_test_line(ui, buf + ls, llen, font_size, local_x));
                } else {
                    target_cursor = (uint32_t)hit_test_line(ui, buf, len, font_size, local_x);
                }

                if (ts->word_select_mode) {
                    size_t ws = 0, we = 0;
                    word_boundaries_at(buf, len, target_cursor, &ws, &we);
                    if (target_cursor < ts->sel_pivot_lo) {
                        ts->sel_anchor = ts->sel_pivot_hi;
                        ts->cursor = (uint32_t)ws;
                    } else if (target_cursor > ts->sel_pivot_hi) {
                        ts->sel_anchor = ts->sel_pivot_lo;
                        ts->cursor = (uint32_t)we;
                    } else {
                        ts->sel_anchor = ts->sel_pivot_lo;
                        ts->cursor = ts->sel_pivot_hi;
                    }
                } else if (ts->line_select_mode) {
                    size_t ls = 0, le = len;
                    if (multiline) {
                        int li = 0;
                        find_line(buf, target_cursor, &ls, &li);
                        le = ls + line_length(buf, len, ls);
                    }
                    if (target_cursor < ts->sel_pivot_lo) {
                        ts->sel_anchor = ts->sel_pivot_hi;
                        ts->cursor = (uint32_t)ls;
                    } else if (target_cursor > ts->sel_pivot_hi) {
                        ts->sel_anchor = ts->sel_pivot_lo;
                        ts->cursor = (uint32_t)le;
                    } else {
                        ts->sel_anchor = ts->sel_pivot_lo;
                        ts->cursor = ts->sel_pivot_hi;
                    }
                } else {
                    ts->cursor = target_cursor;
                }
            } else {
                ts->dragging = false;
                ts->word_select_mode = false;
                ts->line_select_mode = false;
                if (ts->sel_anchor == ts->cursor)
                    sel_clear(ts);
            }
        }
    }

    /* Selection on programmatic/keyboard focus */
    if (r.focused && opts->select_all_on_focus && !ts->select_all_seeded && len > 0 &&
        !ui->input.mouse_pressed[OPTICS_UI_MOUSE_LEFT]) {
        ts->sel_anchor = 0;
        ts->cursor = (uint32_t)len;
        ts->select_all_seeded = true;
    }
    if (!r.focused) {
        ts->select_all_seeded = false;
        ts->dragging = false;
        ts->click_count = 0;
        ts->word_select_mode = false;
        ts->line_select_mode = false;
    }

    bool changed = false;
    bool submitted = false;
    bool tabbed = false;

    /* Single-line vs Multi-line interaction handling */
    if (!disabled && r.focused && buf && buf_cap > 1 && !opts->readonly) {
        uint64_t now_ms = textedit_now_ms();

        /* IME delete_surrounding_text: applied BEFORE the commit string per
         * the text-input-v3 protocol, so freshly committed text lands in
         * the right place relative to the IME's expected context window. */
        if (ui->input.ime_delete_before || ui->input.ime_delete_after) {
            uint32_t lo = ts->cursor >= ui->input.ime_delete_before
                              ? ts->cursor - ui->input.ime_delete_before
                              : 0;
            uint32_t hi = ts->cursor + ui->input.ime_delete_after;
            if (hi > len)
                hi = (uint32_t)len;
            if (hi > lo) {
                if (textedit_replace_range(ts, buf, &len, buf_cap, lo, hi, nullptr, 0, lo,
                                           UINT32_MAX, now_ms, false)) {
                    changed = true;
                }
            }
        }

        /* Direct character input / IME committed text */
        if (ui->input.text_utf8[0] != '\0') {
            size_t in_len = strlen(ui->input.text_utf8);
            uint32_t lo = sel_active(ts) ? sel_lo(ts) : ts->cursor;
            uint32_t hi = sel_active(ts) ? sel_hi(ts) : ts->cursor;
            uint32_t new_cursor = lo + (uint32_t)in_len;
            if (textedit_replace_range(ts, buf, &len, buf_cap, lo, hi, ui->input.text_utf8, in_len,
                                       new_cursor, UINT32_MAX, now_ms, true)) {
                changed = true;
            }
        }

        /* Keyboard events */
        bool shift = (ui->input.mods & OPTICS_UI_MOD_SHIFT) != 0;
        bool ctrl = (ui->input.mods & OPTICS_UI_MOD_CTRL) != 0;
        for (uint32_t i = 0; i < ui->input.key_count; i++) {
            optics_ui_key_event k = ui->input.keys[i];
            if (!k.pressed)
                continue;

            if (k.key == OPTICS_UI_KEY_TAB && opts->accept_tab) {
                tabbed = true;
                optics_ui_consume_key(ui, OPTICS_UI_KEY_TAB);
            } else if (k.key == OPTICS_UI_KEY_LEFT) {
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
            } else if (k.key == OPTICS_UI_KEY_RIGHT) {
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
            } else if (k.key == OPTICS_UI_KEY_UP && multiline) {
                if (shift && ts->sel_anchor == UINT32_MAX)
                    ts->sel_anchor = ts->cursor;
                size_t ls;
                int li;
                find_line(buf, ts->cursor, &ls, &li);
                if (li > 0) {
                    size_t col = ts->cursor - ls;
                    size_t prev_ls = 0;
                    int prev_li = 0;
                    find_line(buf, ls - 1, &prev_ls, &prev_li);
                    size_t prev_len = line_length(buf, len, prev_ls);
                    ts->cursor = (uint32_t)(prev_ls + (col < prev_len ? col : prev_len));
                } else {
                    ts->cursor = 0;
                }
                if (!shift)
                    sel_clear(ts);
            } else if (k.key == OPTICS_UI_KEY_DOWN && multiline) {
                if (shift && ts->sel_anchor == UINT32_MAX)
                    ts->sel_anchor = ts->cursor;
                size_t ls;
                int li;
                find_line(buf, ts->cursor, &ls, &li);
                size_t cur_len = line_length(buf, len, ls);
                if (ls + cur_len < len) {
                    size_t col = ts->cursor - ls;
                    size_t next_ls = ls + cur_len + 1;
                    size_t next_len = line_length(buf, len, next_ls);
                    ts->cursor = (uint32_t)(next_ls + (col < next_len ? col : next_len));
                } else {
                    ts->cursor = (uint32_t)len;
                }
                if (!shift)
                    sel_clear(ts);
            } else if (k.key == OPTICS_UI_KEY_HOME) {
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
            } else if (k.key == OPTICS_UI_KEY_END) {
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
            } else if (k.key == OPTICS_UI_KEY_BACKSPACE) {
                if (sel_active(ts)) {
                    uint32_t lo = sel_lo(ts), hi = sel_hi(ts);
                    if (textedit_replace_range(ts, buf, &len, buf_cap, lo, hi, nullptr, 0, lo,
                                               UINT32_MAX, now_ms, false)) {
                        changed = true;
                    }
                } else if (ts->cursor > 0) {
                    size_t prev = (ctrl ? prev_word_boundary(buf, ts->cursor)
                                        : prev_char_boundary(buf, ts->cursor));
                    if (textedit_replace_range(ts, buf, &len, buf_cap, prev, ts->cursor, nullptr, 0,
                                               (uint32_t)prev, UINT32_MAX, now_ms, true)) {
                        changed = true;
                    }
                }
            } else if (k.key == OPTICS_UI_KEY_DELETE) {
                if (sel_active(ts)) {
                    uint32_t lo = sel_lo(ts), hi = sel_hi(ts);
                    if (textedit_replace_range(ts, buf, &len, buf_cap, lo, hi, nullptr, 0, lo,
                                               UINT32_MAX, now_ms, false)) {
                        changed = true;
                    }
                } else if (ts->cursor < len) {
                    size_t next = (ctrl ? next_word_boundary(buf, len, ts->cursor)
                                        : next_char_boundary(buf, len, ts->cursor));
                    if (textedit_replace_range(ts, buf, &len, buf_cap, ts->cursor, next, nullptr, 0,
                                               ts->cursor, UINT32_MAX, now_ms, false)) {
                        changed = true;
                    }
                }
            } else if (k.key == OPTICS_UI_KEY_RETURN) {
                if (multiline) {
                    uint32_t lo = sel_active(ts) ? sel_lo(ts) : ts->cursor;
                    uint32_t hi = sel_active(ts) ? sel_hi(ts) : ts->cursor;
                    if (textedit_replace_range(ts, buf, &len, buf_cap, lo, hi, "\n", 1, lo + 1,
                                               UINT32_MAX, now_ms, false)) {
                        changed = true;
                    }
                } else {
                    submitted = true;
                    optics_ui_consume_key(ui, OPTICS_UI_KEY_RETURN);
                }
            } else if (ctrl && (k.key == 'a' || k.key == 'A')) {
                ts->sel_anchor = 0;
                ts->cursor = (uint32_t)len;
            } else if (ctrl && (k.key == 'c' || k.key == 'C')) {
                if (sel_active(ts)) {
                    uint32_t lo = sel_lo(ts), hi = sel_hi(ts);
                    optics_ui_copy(ui, &(optics_ui_copy_opts){.utf8 = buf + lo, .len = hi - lo});
                }
            } else if (ctrl && (k.key == 'x' || k.key == 'X')) {
                if (sel_active(ts)) {
                    uint32_t lo = sel_lo(ts), hi = sel_hi(ts);
                    optics_ui_copy(ui, &(optics_ui_copy_opts){.utf8 = buf + lo, .len = hi - lo});
                    if (textedit_replace_range(ts, buf, &len, buf_cap, lo, hi, nullptr, 0, lo,
                                               UINT32_MAX, now_ms, false)) {
                        changed = true;
                    }
                }
            } else if (ctrl && (k.key == 'v' || k.key == 'V')) {
                optics_ui_request_paste(ui);
            } else if (ctrl && (k.key == 'z' || k.key == 'Z')) {
                if (shift) {
                    if (redo_apply(ts, buf, &len, buf_cap))
                        changed = true;
                } else {
                    if (undo_apply(ts, buf, &len, buf_cap))
                        changed = true;
                }
                optics_ui_consume_key(ui, k.key);
            } else if (ctrl && (k.key == 'y' || k.key == 'Y')) {
                if (redo_apply(ts, buf, &len, buf_cap))
                    changed = true;
                optics_ui_consume_key(ui, k.key);
            }
        }

        /* Handle paste payload */
        uint32_t paste_len = 0;
        const char *paste = lensi_take_paste(ui, &paste_len);
        if (paste && paste_len > 0) {
            uint32_t lo = sel_active(ts) ? sel_lo(ts) : ts->cursor;
            uint32_t hi = sel_active(ts) ? sel_hi(ts) : ts->cursor;
            uint32_t new_cursor = lo + paste_len;
            if (textedit_replace_range(ts, buf, &len, buf_cap, lo, hi, paste, paste_len, new_cursor,
                                       UINT32_MAX, now_ms, false)) {
                changed = true;
            }
        }
    }

    /* Measure and layout display text */
    bool has_preedit = r.focused && buf && buf_cap > 1 && ui->input.preedit_utf8[0] != '\0';
    bool show_placeholder = (!has_preedit && len == 0 && placeholder && placeholder[0]);
    const char *display_text = show_placeholder ? placeholder : (buf ? buf : "");
    float cap_height = fm.baseline > 0.0f ? (fm.baseline * 0.72f) : (font_size * 0.72f);
    float optical_baseline = roundf(h * 0.5f + cap_height * 0.5f);
    float text_y = multiline ? padding - ts->scroll_y : (optical_baseline - fm.baseline);
    float font_asc = fm.baseline > 0.0f ? fm.baseline : (font_size * 0.95f);
    float font_desc = (fm.height > fm.baseline && fm.baseline > 0.0f) ? (fm.height - fm.baseline)
                                                                      : (font_size * 0.25f);
    float caret_h = font_asc + font_desc;
    float base_caret_x = prefix_width(ui, buf, ts->cursor, font_size);
    float caret_x = base_caret_x;
    float caret_y = optical_baseline - font_asc;
    optics_gfx_rect preedit_underline = {};
    optics_gfx_rect preedit_clause = {};
    if (has_preedit) {
        const char *pe = ui->input.preedit_utf8;
        size_t pe_len = strlen(pe);
        size_t display_len = ts->cursor + pe_len + (len - ts->cursor);
        char *display = optics_gfx_arena_alloc(&ui->arena, display_len + 1);
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
        float pe_width = prefix_width(ui, pe, pe_len, font_size);
        preedit_underline =
            (optics_gfx_rect){pad_x + base_caret_x, optical_baseline + 1.0f, pe_width, 1.5f};
        uint32_t clause_lo = ui->input.preedit_sel_lo;
        uint32_t clause_hi = ui->input.preedit_sel_hi;
        if (clause_hi > clause_lo && clause_hi <= pe_len) {
            float clause_x = prefix_width(ui, pe, clause_lo, font_size);
            float clause_w = prefix_width(ui, pe, clause_hi, font_size) - clause_x;
            preedit_clause = (optics_gfx_rect){pad_x + base_caret_x + clause_x,
                                               optical_baseline + 1.0f, clause_w, 2.5f};
        }
        caret_x = base_caret_x + prefix_width(ui, pe, ui->input.preedit_cursor, font_size);
    } else if (r.focused && !show_placeholder && buf) {
        if (multiline) {
            size_t ls;
            int li;
            find_line(buf, ts->cursor, &ls, &li);
            caret_x = prefix_width(ui, buf + ls, ts->cursor - ls, font_size);
            caret_y = padding - ts->scroll_y + (float)li * line_height;
            caret_h = line_h;
        } else {
            caret_x = base_caret_x;
        }
    }
    optics_gfx_rect caret_rect = {pad_x + caret_x, caret_y, 1.5f, caret_h};
    /* Build text lines for multiline or single-line */
    optics_ui_text_line *lines = nullptr;
    int line_count = 0;
    if (multiline) {
        int line_cap = 8;
        lines = optics_gfx_arena_alloc(&ui->arena, (size_t)line_cap * sizeof *lines);
        size_t start = 0;
        int row_idx = 0;
        while (start <= len && lines) {
            size_t llen = line_length(display_text, len, start);
            char *line_str = optics_gfx_arena_alloc(&ui->arena, llen + 1);
            if (line_str) {
                memcpy(line_str, display_text + start, llen);
                line_str[llen] = '\0';
                lines[line_count++] = (optics_ui_text_line){
                    .text = line_str,
                    .x = pad_x,
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
    optics_gfx_rect sel_rect = {};
    int sel_rect_count = 0;
    if (sel_active(ts) && !multiline) {
        uint32_t lo = sel_lo(ts), hi = sel_hi(ts);
        float x1 = prefix_width(ui, buf, lo, font_size);
        float x2 = prefix_width(ui, buf, hi, font_size);
        sel_rect = (optics_gfx_rect){pad_x + x1, text_y, x2 - x1, line_h};
        sel_rect_count = 1;
    }

    /* Skin emission */
    lensi_skin_emit(ui, n,
                    &(optics_ui_widget_record){
                        .kind = OPTICS_UI_WIDGET_TEXTEDIT,
                        .state = r.state,
                        .bounds = {0, 0, w, h},
                        .last_bounds = n->prev_rect,
                        .style = rs,
                        .style_fields = eff.fields,
                        .content =
                            {
                                .multiline = multiline,
                                .edit_text = multiline ? nullptr : display_text,
                                .edit_text_x = pad_x,
                                .edit_text_y = text_y,
                                .show_placeholder = show_placeholder,
                                .lines = lines,
                                .line_count = line_count,
                                .sel_rects = &sel_rect,
                                .sel_rect_count = sel_rect_count,
                                .caret = caret_rect,
                                .show_caret = r.focused && !disabled,
                                .preedit_underline = preedit_underline,
                                .has_preedit = has_preedit,
                                .preedit_clause = preedit_clause,
                                .error = error,
                            },
                    });

    /* Platform caret and context reporting */
    if (!disabled && r.focused && buf && buf_cap > 1) {
        lensi_set_caret_rect(ui, (optics_gfx_rect){
                                     n->prev_rect.x + caret_rect.x,
                                     n->prev_rect.y + caret_rect.y,
                                     2.0f,
                                     caret_rect.h,
                                 });
        lensi_set_text_context(ui, buf, (uint32_t)len, ts->cursor, multiline);
    }

    uint32_t sem_flags = (r.focused ? OPTICS_UI_A11Y_FOCUSED : 0) |
                         (disabled ? OPTICS_UI_A11Y_DISABLED : 0) |
                         (opts->readonly ? OPTICS_UI_A11Y_READONLY : 0);
    lensi_node_semantics(ui, n, multiline ? OPTICS_UI_ROLE_TEXTAREA : OPTICS_UI_ROLE_TEXTFIELD,
                         label, buf, sem_flags);

    r.changed = changed;
    r.submitted = submitted;
    r.tabbed = tabbed;
    ui->last_response = r;
    return r;
}

void optics_ui_textedit_set_caret(optics_ui *ui, const optics_ui_textedit_set_caret_opts *opts) {
    if (!ui || !opts || !opts->label)
        return;
    optics_ui_node *n = optics_ui_find(ui, optics_ui_current_id(ui, opts->label));
    if (!n)
        return;
    optics_ui_textedit_state *ts = optics_ui_node_state(n, sizeof *ts);
    if (ts) {
        ts->cursor = opts->caret;
        sel_clear(ts);
    }
}

bool optics_ui_textedit_get_caret(const optics_ui *ui,
                                  const optics_ui_textedit_get_caret_opts *opts) {
    if (!ui || !opts || !opts->label)
        return false;
    optics_ui_node *n = optics_ui_find((optics_ui *)ui, optics_ui_current_id(ui, opts->label));
    if (!n)
        return false;
    optics_ui_textedit_state *ts = optics_ui_node_state(n, sizeof *ts);
    if (!ts)
        return false;
    if (opts->out_caret)
        *opts->out_caret = ts->cursor;
    return true;
}

void optics_ui_textedit_set_selection(optics_ui *ui,
                                      const optics_ui_textedit_set_selection_opts *opts) {
    if (!ui || !opts || !opts->label)
        return;
    optics_ui_node *n = optics_ui_find(ui, optics_ui_current_id(ui, opts->label));
    if (!n)
        return;
    optics_ui_textedit_state *ts = optics_ui_node_state(n, sizeof *ts);
    if (ts) {
        ts->sel_anchor = opts->sel_start;
        ts->cursor = opts->sel_end;
    }
}

bool optics_ui_textedit_get_selection(const optics_ui *ui,
                                      const optics_ui_textedit_get_selection_opts *opts) {
    if (!ui || !opts || !opts->label)
        return false;
    optics_ui_node *n = optics_ui_find((optics_ui *)ui, optics_ui_current_id(ui, opts->label));
    if (!n)
        return false;
    optics_ui_textedit_state *ts = optics_ui_node_state(n, sizeof *ts);
    if (!ts || !sel_active(ts))
        return false;
    if (opts->out_sel_start)
        *opts->out_sel_start = sel_lo(ts);
    if (opts->out_sel_end)
        *opts->out_sel_end = sel_hi(ts);
    return true;
}
