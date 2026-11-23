/* skin/image.c — default image skin (ADR-0059). */

#include "../internal.h"

void optics_ui__skin_image(optics_ui *ui, optics_ui_node *n, const optics_ui_widget_record *rec) {
    const optics_ui_style_resolved *rs = &rec->style;
    float w = rec->bounds.w;
    float h = rec->bounds.h;
    optics_gfx_color tint = (rec->content.tint == 0)
                                ? optics_gfx_color_rgba_premul(255, 255, 255, 255)
                                : rec->content.tint;

    optics_ui__drawlist_push(ui, n,
                             (optics_ui_draw_cmd){
                                 .kind = OPTICS_UI_DRAW_IMAGE,
                                 .rel = {0, 0, w, h},
                                 .color = tint,
                                 .outline_color = rs->outline_color,
                                 .outline_width = rs->outline_width,
                                 .image = rec->content.image,
                             });
}
