/* image.c — raster texture rendering */

#include "../internal.h"

optics_ui_response optics_ui_image(optics_ui *ui, const optics_ui_image_opts *opts) {
    optics_ui_image_opts default_opts = {};
    if (!opts)
        opts = &default_opts;

    optics_ui__apply_box(ui, opts->box);
    ui->next_disabled = false;
    ui->next_error = false;
    optics_ui_style eff = optics_ui__style_effective(ui);

    optics_ui_id nid = opts->box.id ? optics_ui__gen_widget_id(ui, opts->box.id)
                                    : optics_ui__gen_container_id(ui, "image");
    optics_ui_node *n = optics_ui__store_touch(ui, nid);
    if (!n)
        return (optics_ui_response){};

    optics_ui__link_child(ui, n);
    optics_ui__node_box(ui, n, &opts->box);
    n->is_container = false;

    float w = opts->width;
    float h = opts->height;
    if (w <= 0.0f && h > 0.0f)
        w = h;
    if (h <= 0.0f && w > 0.0f)
        h = w;
    if (w <= 0.0f) {
        w = optics_ui__style_font_size(ui, &eff, &ui->theme);
        h = w;
    }
    if (n->fixed_w > 0.0f)
        w = n->fixed_w;
    if (n->fixed_h > 0.0f)
        h = n->fixed_h;
    n->measured = (optics_gfx_point){w, h};

    optics_ui_style_resolved rs = optics_ui__style_resolve(ui, &eff, &ui->theme, 0);
    optics_ui__skin_emit(ui, n,
                         &(optics_ui_widget_record){
                             .kind = OPTICS_UI_WIDGET_IMAGE,
                             .state = 0,
                             .bounds = {0, 0, w, h},
                             .last_bounds = n->prev_rect,
                             .style = rs,
                             .style_fields = eff.fields,
                             .content =
                                 {
                                     .image = opts->image,
                                     .tint = opts->tint,
                                 },
                         });

    optics_ui_response r = (optics_ui_response){.id = nid, .rect = n->prev_rect};
    ui->last_response = r;
    return r;
}
