/* skin/icon.c — default bare-icon skin (ADR-0059): the lens_icon emit
 * section, verbatim. The glyph centres in the layout box (a size hint must
 * not rescale the glyph); the outline atoms (ADR-0061) come through the
 * resolved style. */

#include "../internal.h"

void lensi_skin_icon(lens *ui, lens_node *n, const lens_widget_record *rec) {
    const lens_style_resolved *rs = &rec->style;
    float bw = rec->bounds.w;
    float bh = rec->bounds.h;
    float s = fminf(rec->content.glyph_size, fminf(bw, bh));

    lensi_drawlist_push(ui, n,
                        (lens_draw_cmd){
                            .kind = LENS_DRAW_ICON,
                            .rel = {(bw - s) * 0.5f, (bh - s) * 0.5f, s, s},
                            .color = rs->fg,
                            .outline_color = rs->outline_color,
                            .outline_width = rs->outline_width,
                            .width = 2.0f * (s / 24.0f),
                            .icon_id = rec->content.icon,
                        });
}
