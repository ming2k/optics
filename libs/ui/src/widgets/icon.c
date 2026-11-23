/* icon.c — bare vector icon rendering */

#include "../internal.h"

optics_ui_response optics_ui_icon(optics_ui *ui, const optics_ui_icon_opts *opts) {
    optics_ui_icon_opts default_opts = {};
    if (!opts)
        opts = &default_opts;

    if (!optics_ui__icon_valid((int32_t)opts->id) || opts->id == OPTICS_UI_ICON_INVALID)
        return (optics_ui_response){};

    optics_ui__apply_box(ui, opts->box);
    const optics_ui_theme *t = &ui->theme;
    ui->next_disabled = false;
    ui->next_error = false;
    optics_ui_style eff = optics_ui__style_effective(ui);

    optics_ui_id nid = opts->box.id ? optics_ui__gen_widget_id(ui, opts->box.id)
                                    : optics_ui__gen_container_id(ui, "icon");
    optics_ui_node *n = optics_ui__store_touch(ui, nid);
    if (!n)
        return (optics_ui_response){};

    optics_ui__link_child(ui, n);
    optics_ui__node_box(ui, n, &opts->box);
    n->is_container = false;

    float glyph = opts->size > 0 ? opts->size : optics_ui__style_font_size(ui, &eff, t);
    float bw = n->fixed_w > 0 ? n->fixed_w : glyph;
    float bh = n->fixed_h > 0 ? n->fixed_h : glyph;
    n->measured = (optics_gfx_point){bw, bh};

    optics_ui_style_resolved rs = optics_ui__style_resolve(ui, &eff, t, 0);
    if ((opts->color & 0xFF000000u) != 0)
        rs.fg = opts->color;

    optics_ui__skin_emit(ui, n,
                         &(optics_ui_widget_record){
                             .kind = OPTICS_UI_WIDGET_ICON,
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

    optics_ui_response r = (optics_ui_response){.id = nid, .rect = n->prev_rect};
    ui->last_response = r;
    return r;
}
