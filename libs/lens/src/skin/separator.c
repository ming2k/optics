/* skin/separator.c — default separator skin (ADR-0059): one rect, moved
 * verbatim from the widget. Orientation arrives via content.vertical. */

#include "../internal.h"

void lensi_skin_separator(lens *ui, lens_node *n, const lens_widget_record *rec) {
    const lens_style_resolved *rs = &rec->style;
    float w = rec->bounds.w;
    float h = rec->bounds.h;

    if (rec->content.vertical) {
        lensi_drawlist_push(
            ui, n,
            (lens_draw_cmd){.kind = LENS_DRAW_RECT,
                            .rel = {0, rs->padding, 1.0f,
                                    h > 2.0f * rs->padding ? h - 2.0f * rs->padding : 1.0f},
                            .color = rs->border,
                            .radius = 0.5f});
    } else {
        lensi_drawlist_push(
            ui, n,
            (lens_draw_cmd){.kind = LENS_DRAW_RECT,
                            .rel = {rs->padding, 0,
                                    w > 2.0f * rs->padding ? w - 2.0f * rs->padding : 1.0f, 1.0f},
                            .color = rs->border,
                            .radius = 0.5f});
    }
}
