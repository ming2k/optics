/* label.c — static text (ADR-0031). Non-interactive. */

#include "../internal.h"

static bool ascii_wrap_space(unsigned char c) {
    return c == ' ' || c == '\t' || c == '\r';
}

static bool middle_dot_at(const char *text, size_t len, size_t pos) {
    return pos + 1 < len && (unsigned char)text[pos] == 0xc2 &&
           (unsigned char)text[pos + 1] == 0xb7;
}

static size_t utf8_next_boundary(const char *text, size_t len, size_t pos) {
    if (pos >= len)
        return len;
    unsigned char lead = (unsigned char)text[pos];
    size_t width = lead < 0x80   ? 1
                   : lead < 0xe0 ? 2
                   : lead < 0xf0 ? 3
                                 : 4;
    if (width > len - pos)
        width = 1;
    return pos + width;
}

static size_t utf8_prev_boundary(const char *text, size_t pos) {
    if (pos == 0)
        return 0;
    pos--;
    while (pos > 0 && ((unsigned char)text[pos] & 0xc0) == 0x80)
        pos--;
    return pos;
}

static flux_text_metrics measure_slice(lens *ui, const char *text, size_t len, float size) {
    return flux_text_measure(ui->text, text, len,
                             &(flux_text_style){.size_px = size, .weight = 0.0f});
}

/* Pick the next visual line in [start, len). Whole words are preferred; a
 * token wider than max_width is split at the nearest UTF-8 cluster boundary
 * reported by flux-text so URLs and identifiers cannot escape the box. */
static size_t wrapped_line_end(lens *ui, const char *text, size_t len, size_t start,
                               float size, float max_width, size_t *next) {
    size_t segment_end = start;
    while (segment_end < len && text[segment_end] != '\n')
        segment_end++;

    if (segment_end == start) {
        *next = segment_end < len ? segment_end + 1 : segment_end;
        return start;
    }

    if (measure_slice(ui, text + start, segment_end - start, size).width <= max_width) {
        *next = segment_end < len ? segment_end + 1 : segment_end;
        return segment_end;
    }

    const char *segment = text + start;
    size_t segment_len = segment_end - start;
    const flux_text_style style = {.size_px = size, .weight = 0.0f};
    size_t cut = flux_text_byte_for_x(ui->text, segment, segment_len, max_width, &style);
    if (cut > segment_len)
        cut = segment_len;
    /* byte_for_x returns the *nearest* boundary, which can overshoot the
     * budget by up to half a glyph; back off one codepoint at a time until
     * the caret x of `cut` fits. Querying the caret within the full segment
     * keeps every step on the same layout-cache entry — measuring each
     * shrinking prefix instead would shape a fresh string per step. */
    while (cut > 0 && flux_text_x_for_byte(ui->text, segment, segment_len, cut, &style) > max_width)
        cut = utf8_prev_boundary(segment, cut);
    if (cut == 0)
        cut = utf8_next_boundary(segment, segment_len, 0);

    size_t word_break = cut;
    while (word_break > 0) {
        size_t previous = utf8_prev_boundary(segment, word_break);
        if (word_break - previous == 1 && ascii_wrap_space((unsigned char)segment[previous])) {
            size_t line_end = previous;
            while (line_end > 0 && ascii_wrap_space((unsigned char)segment[line_end - 1]))
                line_end--;
            if (line_end >= 2 && middle_dot_at(segment, segment_len, line_end - 2)) {
                line_end -= 2;
                while (line_end > 0 && ascii_wrap_space((unsigned char)segment[line_end - 1]))
                    line_end--;
            }
            size_t resume = word_break;
            while (resume < segment_len && ascii_wrap_space((unsigned char)segment[resume]))
                resume++;
            if (middle_dot_at(segment, segment_len, resume)) {
                resume += 2;
                while (resume < segment_len && ascii_wrap_space((unsigned char)segment[resume]))
                    resume++;
            }
            if (line_end > 0) {
                *next = start + resume;
                return start + line_end;
            }
            break;
        }
        word_break = previous;
    }

    *next = start + cut;
    return start + cut;
}

static void push_text_slice(lens *ui, lens_node *n, const char *text, size_t len, float x,
                            float y, float size) {
    if (len == 0)
        return;
    char *line = flux_arena_alloc(&ui->arena, len + 1);
    if (!line) {
        ui->overflow = true;
        return;
    }
    memcpy(line, text, len);
    line[len] = '\0';
    lensi_drawlist_push(ui, n,
                        (lens_draw_cmd){.kind = LENS_DRAW_TEXT,
                                        .rel = {x, y, 0, 0},
                                        .color = ui->theme.color_fg,
                                        .text = line,
                                        .text_size = size,
                                        .text_weight = 0.0f});
}

static void label_wrapped(lens *ui, const char *text, float size, float max_width) {
    const lens_theme *t = &ui->theme;
    lens_id id = lensi_gen_widget_id(ui, text);
    lens_node *n = lensi_store_touch(ui, id);
    if (!n)
        return;
    lensi_link_child(ui, n);
    n->is_container = false;

    lens_text_metrics intrinsic = lensi_text_measure_label(ui, text, size, 0.0f);
    float requested_width = max_width > 0.0f ? max_width : intrinsic.width + 2.0f * t->padding;
    float w = n->fixed_w > 0.0f ? n->fixed_w
                                : fminf(intrinsic.width + 2.0f * t->padding, requested_width);
    float content_width = fmaxf(w - 2.0f * t->padding, 1.0f);
    flux_text_metrics line_metrics = measure_slice(ui, "Ag", 2, size);
    float line_height = line_metrics.height > 0.0f ? line_metrics.height : size;
    float line_gap = fmaxf(size * 0.25f, 2.0f);

    size_t len = strlen(text);
    size_t start = 0;
    size_t line_count = 0;
    do {
        size_t next = start;
        size_t end = wrapped_line_end(ui, text, len, start, size, content_width, &next);
        push_text_slice(ui, n, text + start, end - start, t->padding,
                        t->padding + (float)line_count * (line_height + line_gap), size);
        line_count++;
        if (next <= start)
            break;
        start = next;
    } while (start < len);

    float content_height = (float)line_count * line_height;
    if (line_count > 1)
        content_height += (float)(line_count - 1) * line_gap;
    float h = n->fixed_h > 0.0f ? n->fixed_h : content_height + 2.0f * t->padding;
    n->measured = (flux_point){w, h};

    lensi_node_semantics(ui, n, LENS_ROLE_LABEL, text, NULL, 0);
    ui->last_response = (lens_response){.id = id, .rect = n->prev_rect};
}

void lens_label(lens *ui, const char *text) {
    const lens_theme *t = &ui->theme;
    lens_id id = lensi_gen_widget_id(ui, text);
    lens_node *n = lensi_store_touch(ui, id);
    if (!n)
        return;
    lensi_link_child(ui, n);
    n->is_container = false;

    lens_text_metrics tm = lensi_text_measure_label(ui, text, t->font_size, 0.0f);
    float w = (n->fixed_w > 0) ? n->fixed_w : tm.width + 2.0f * t->padding;
    float h = (n->fixed_h > 0) ? n->fixed_h : tm.height + 2.0f * t->padding;
    n->measured = (flux_point){w, h};

    lensi_node_semantics(ui, n, LENS_ROLE_LABEL, text, NULL, 0);

    lensi_drawlist_push(ui, n,
                        (lens_draw_cmd){.kind = LENS_DRAW_TEXT,
                                        .rel = {t->padding, t->padding, 0, 0},
                                        .color = t->color_fg,
                                        .text = text,
                                        .text_size = t->font_size,
                                        .text_weight = 0.0f});

    ui->last_response = (lens_response){.id = id, .rect = n->prev_rect};
}

void lens_label_ex(lens *ui, const char *text, float size) {
    const lens_theme *t = &ui->theme;
    lens_id id = lensi_gen_widget_id(ui, text);
    lens_node *n = lensi_store_touch(ui, id);
    if (!n)
        return;
    lensi_link_child(ui, n);
    n->is_container = false;

    lens_text_metrics tm = lensi_text_measure_label(ui, text, size, 0.0f);
    float w = (n->fixed_w > 0) ? n->fixed_w : tm.width + 2.0f * t->padding;
    float h = (n->fixed_h > 0) ? n->fixed_h : tm.height + 2.0f * t->padding;
    n->measured = (flux_point){w, h};

    lensi_node_semantics(ui, n, LENS_ROLE_LABEL, text, NULL, 0);

    lensi_drawlist_push(ui, n,
                        (lens_draw_cmd){.kind = LENS_DRAW_TEXT,
                                        .rel = {t->padding, t->padding, 0, 0},
                                        .color = t->color_fg,
                                        .text = text,
                                        .text_size = size,
                                        .text_weight = 0.0f});

    ui->last_response = (lens_response){.id = id, .rect = n->prev_rect};
}

void lens_label_wrapped(lens *ui, const char *text, float max_width) {
    label_wrapped(ui, text, ui->theme.font_size, max_width);
}

void lens_label_wrapped_ex(lens *ui, const char *text, float size, float max_width) {
    label_wrapped(ui, text, size, max_width);
}

void lens_label_compact_ex(lens *ui, const char *text, float size) {
    lens_id id = lensi_gen_widget_id(ui, text);
    lens_node *n = lensi_store_touch(ui, id);
    if (!n)
        return;
    lensi_link_child(ui, n);
    n->is_container = false;

    lens_text_metrics tm = lensi_text_measure_label(ui, text, size, 0.0f);
    float w = (n->fixed_w > 0) ? n->fixed_w : tm.width;
    float h = (n->fixed_h > 0) ? n->fixed_h : tm.height;
    n->measured = (flux_point){w, h};

    lensi_node_semantics(ui, n, LENS_ROLE_LABEL, text, NULL, 0);

    float y = (h - tm.height) * 0.5f;
    if (y < 0.0f)
        y = 0.0f;
    lensi_drawlist_push(ui, n,
                        (lens_draw_cmd){.kind = LENS_DRAW_TEXT,
                                        .rel = {0, y, 0, 0},
                                        .color = ui->theme.color_fg,
                                        .text = text,
                                        .text_size = size,
                                        .text_weight = 0.0f});

    ui->last_response = (lens_response){.id = id, .rect = n->prev_rect};
}
