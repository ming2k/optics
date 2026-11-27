/* skin/separator.c — default separator skin (ADR-0059): one rect, moved
 * verbatim from the widget. Orientation arrives via content.vertical. */

#include "../internal.h"

void optics_ui__skin_separator(optics_ui *ui, optics_ui_node *n,
                               const optics_ui_widget_record *rec) {
    const optics_ui_style_resolved *rs = &rec->style;
    float w = rec->bounds.w;
    float h = rec->bounds.h;

    if (rec->content.vertical) {
        optics_ui__drawlist_push(
            ui, n,
            (optics_ui_draw_cmd){.kind = OPTICS_UI_DRAW_RECT,
                                 .rel = {0, rs->padding, 1.0f,
                                         h > 2.0f * rs->padding ? h - 2.0f * rs->padding : 1.0f},
                                 .color = rs->border,
                                 .corner_radii = optics_gfx_corner_radii_uniform(0.5f)});
    } else {
        optics_ui__drawlist_push(
            ui, n,
            (optics_ui_draw_cmd){.kind = OPTICS_UI_DRAW_RECT,
                                 .rel = {rs->padding, 0,
                                         w > 2.0f * rs->padding ? w - 2.0f * rs->padding : 1.0f,
                                         1.0f},
                                 .color = rs->border,
                                 .corner_radii = optics_gfx_corner_radii_uniform(0.5f)});
    }
}
