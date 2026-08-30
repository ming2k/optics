/* icon.c — bare vector icon rendering */

#include "../internal.h"

lens_response lens_icon(lens *ui, const lens_icon_opts *opts) {
    lens_icon_opts default_opts = {0};
    if (!opts)
        opts = &default_opts;

    if (!lensi_icon_valid((int32_t)opts->id) || opts->id == LENS_ICON_INVALID)
        return (lens_response){0};

    lensi_apply_box(ui, opts->box);
    const lens_theme *t = &ui->theme;
    ui->next_disabled = false;
    ui->next_error = false;
    lens_style eff = lensi_style_effective(ui);

    lens_id nid =
        opts->box.id ? lensi_gen_widget_id(ui, opts->box.id) : lensi_gen_container_id(ui, "icon");
    lens_node *n = lensi_store_touch(ui, nid);
    if (!n)
        return (lens_response){0};

    lensi_link_child(ui, n);
    n->is_container = false;

    float glyph = opts->size > 0 ? opts->size : lensi_style_font_size(ui, &eff, t);
    float bw = n->fixed_w > 0 ? n->fixed_w : glyph;
    float bh = n->fixed_h > 0 ? n->fixed_h : glyph;
    n->measured = (flux_point){bw, bh};

    lens_style_resolved rs = lensi_style_resolve(ui, &eff, t, 0);
    if ((opts->color & 0xFF000000u) != 0)
        rs.fg = opts->color;

    lensi_skin_emit(ui, n,
                    &(lens_widget_record){
                        .kind = LENS_WIDGET_ICON,
                        .state = 0,
                        .bounds = {0, 0, bw, bh},
                        .last_bounds = n->prev_rect,
                        .style = rs,
                        .style_fields = eff.fields,
                        .content =
                            {
                                .icon = opts->id,
                                .glyph_size = glyph,
                            },
                    });

    lens_response r = (lens_response){.id = nid, .rect = n->prev_rect};
    ui->last_response = r;
    return r;
}
