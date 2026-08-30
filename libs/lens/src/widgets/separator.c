/* separator.c — horizontal or vertical dividing line */

#include "../internal.h"

lens_response lens_separator(lens *ui, const lens_separator_opts *opts) {
    lens_separator_opts default_opts = {0};
    if (!opts)
        opts = &default_opts;

    lensi_apply_box(ui, opts->box);
    lens_style eff = lensi_style_effective(ui);
    lens_style_resolved rs = lensi_style_resolve(ui, &eff, &ui->theme, 0);
    lens_id id =
        opts->box.id ? lensi_gen_widget_id(ui, opts->box.id) : lensi_gen_widget_id(ui, "##sep");
    lens_node *n = lensi_store_touch(ui, id);
    if (!n)
        return (lens_response){0};

    lensi_link_child(ui, n);
    n->is_container = false;

    lens_node *parent = lensi_open_container(ui);
    bool is_vert =
        (opts->axis == LENS_COLUMN) || (opts->axis == 0 && parent && parent->axis == LENS_ROW);

    float th = opts->thickness > 0.0f ? opts->thickness : 1.0f;
    float w = is_vert ? th : 0.0f;
    float h = is_vert ? 0.0f : th;
    if (n->fixed_w > 0)
        w = n->fixed_w;
    if (n->fixed_h > 0)
        h = n->fixed_h;
    n->measured = (flux_point){w, h};

    lensi_skin_emit(ui, n,
                    &(lens_widget_record){
                        .kind = LENS_WIDGET_SEPARATOR,
                        .state = 0,
                        .bounds = {0, 0, w, h},
                        .last_bounds = n->prev_rect,
                        .style = rs,
                        .style_fields = eff.fields,
                        .content = {.vertical = is_vert},
                    });

    lens_response r = (lens_response){.id = id, .rect = n->prev_rect};
    ui->last_response = r;
    return r;
}
