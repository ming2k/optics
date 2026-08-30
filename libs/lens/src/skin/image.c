/* skin/image.c — default image skin (ADR-0059). */

#include "../internal.h"

void lensi_skin_image(lens *ui, lens_node *n, const lens_widget_record *rec) {
    const lens_style_resolved *rs = &rec->style;
    float w = rec->bounds.w;
    float h = rec->bounds.h;
    flux_color tint =
        (rec->content.tint == 0) ? flux_color_rgba_premul(255, 255, 255, 255) : rec->content.tint;

    lensi_drawlist_push(ui, n,
                        (lens_draw_cmd){
                            .kind = LENS_DRAW_IMAGE,
                            .rel = {0, 0, w, h},
                            .color = tint,
                            .outline_color = rs->outline_color,
                            .outline_width = rs->outline_width,
                            .image = rec->content.image,
                        });
}
