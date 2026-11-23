/* skin/icon.c — default bare-icon skin (ADR-0059): the optics_ui_icon emit
 * section, verbatim. The glyph centres in the layout box (a size hint must
 * not rescale the glyph); the outline atoms (ADR-0061) come through the
 * resolved style. */

#include "../internal.h"

void optics_ui__skin_icon(optics_ui *ui, optics_ui_node *n, const optics_ui_widget_record *rec) {
    const optics_ui_style_resolved *rs = &rec->style;
    float bw = rec->bounds.w;
    float bh = rec->bounds.h;
    float s = fminf(rec->content.glyph_size, fminf(bw, bh));

    optics_ui__drawlist_push(ui, n,
                             (optics_ui_draw_cmd){
                                 .kind = OPTICS_UI_DRAW_ICON,
                                 .rel = {(bw - s) * 0.5f, (bh - s) * 0.5f, s, s},
                                 .color = rs->fg,
                                 .outline_color = rs->outline_color,
                                 .outline_width = rs->outline_width,
                                 .width = 2.0f * (s / 24.0f),
                                 .icon_id = rec->content.icon,
                             });
}
