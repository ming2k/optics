/* image.c — raster texture rendering */

#include "../internal.h"

lens_response lens_image(lens *ui, const lens_image_opts *opts) {
    lens_image_opts default_opts = {0};
    if (!opts)
        opts = &default_opts;

    lensi_apply_box(ui, opts->box);
    ui->next_disabled = false;
    ui->next_error = false;
    lens_style eff = lensi_style_effective(ui);

    lens_id nid =
        opts->box.id ? lensi_gen_widget_id(ui, opts->box.id) : lensi_gen_container_id(ui, "image");
    lens_node *n = lensi_store_touch(ui, nid);
    if (!n)
        return (lens_response){0};

    lensi_link_child(ui, n);
    n->is_container = false;

    float w = opts->width;
    float h = opts->height;
    if (w <= 0.0f && h > 0.0f)
        w = h;
    if (h <= 0.0f && w > 0.0f)
        h = w;
    if (w <= 0.0f) {
        w = lensi_style_font_size(ui, &eff, &ui->theme);
        h = w;
    }
    if (n->fixed_w > 0.0f)
        w = n->fixed_w;
    if (n->fixed_h > 0.0f)
        h = n->fixed_h;
    n->measured = (flux_point){w, h};

    lens_style_resolved rs = lensi_style_resolve(ui, &eff, &ui->theme, 0);
    lensi_skin_emit(ui, n,
                    &(lens_widget_record){
                        .kind = LENS_WIDGET_IMAGE,
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

    lens_response r = (lens_response){.id = nid, .rect = n->prev_rect};
    ui->last_response = r;
    return r;
}
