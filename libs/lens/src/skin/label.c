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
                                                .text = line->text,
                                                .text_size = size,
                                                .text_weight = rec->content.text_weight});
        }
        return;
    }

    if (rec->content.compact) {
        /* Compact form: no padding, text centred in the intrinsic box, with
         * the opt-in outline atoms (ADR-0061). */
        float y = (rec->bounds.h - rec->content.text.height) * 0.5f;
        if (y < 0.0f)
            y = 0.0f;
        lensi_drawlist_push(ui, n,
                            (lens_draw_cmd){.kind = LENS_DRAW_TEXT,
                                            .rel = {0, y, 0, 0},
                                            .color = rs->fg,
                                            .outline_color = rs->outline_color,
                                            .outline_width = rs->outline_width,
                                            .text = rec->content.label,
                                            .text_size = size,
                                            .text_weight = 0.0f});
        return;
    }

    /* Padded forms (label/label_ex/title/heading): negative rel.h centres
     * vertically in the RESOLVED node box at replay, so text stays centred
     * even when a fixed-height parent constrains the node. */
    lensi_drawlist_push(ui, n,
                        (lens_draw_cmd){.kind = LENS_DRAW_TEXT,
                                        .rel = {rs->padding, 0, 0, -1.0f},
                                        .color = rs->fg,
                                        .text = rec->content.label,
                                        .text_size = size,
                                        .text_weight = rec->content.text_weight});
}
