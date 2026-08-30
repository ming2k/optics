/* skin/label.c — default label skin (ADR-0059): the text widgets' emit
 * sections, moved verbatim. One skin serves lens_label / lens_label_ex /
 * lens_label_wrapped* / lens_label_compact_ex / lens_title / lens_heading:
 * plain and compact forms draw `label` (compact adds the style-cascade
 * outline atoms), the wrapped form draws the pre-wrapped `lines` slices —
 * wrapping itself is measure behaviour and stays in the widget. */

#include "../internal.h"

void lensi_skin_label(lens *ui, lens_node *n, const lens_widget_record *rec) {
    const lens_style_resolved *rs = &rec->style;
    float size = rec->content.text_size > 0.0f ? rec->content.text_size : rs->font_size;

    if (rec->content.lines) {
        /* Wrapped label: one text command per pre-wrapped slice. */
        for (int i = 0; i < rec->content.line_count; i++) {
            const lens_text_line *line = &rec->content.lines[i];
            lensi_drawlist_push(ui, n,
                                (lens_draw_cmd){.kind = LENS_DRAW_TEXT,
                                                .rel = {line->x, line->y, 0, 0},
                                                .color = rs->fg,
                                                .outline_color = rs->outline_color,
                                                .outline_width = rs->outline_width,
                                                .text = line->text,
                                                .text_size = size,
                                                .text_weight = rec->content.text_weight});
        }
        return;
    }

    float x = rs->padding;
    float rel_w = 0.0f;
    if (rec->content.align == LENS_CENTER) {
        rel_w = -1.0f;
        x = 0.0f;
    } else if (rec->content.align == LENS_END) {
        x = rec->bounds.w - rec->content.text.width - rs->padding;
        if (x < rs->padding)
            x = rs->padding;
    }

    /* Single-line label: negative rel.h centres vertically in the RESOLVED
     * node box at replay; negative rel.w centres horizontally when align == LENS_CENTER. */
    lensi_drawlist_push(ui, n,
                        (lens_draw_cmd){.kind = LENS_DRAW_TEXT,
                                        .rel = {x, 0, rel_w, -1.0f},
                                        .color = rs->fg,
                                        .outline_color = rs->outline_color,
                                        .outline_width = rs->outline_width,
                                        .text = rec->content.label,
                                        .text_size = size,
                                        .text_weight = rec->content.text_weight});
}
