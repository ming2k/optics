/* separator.c — horizontal or vertical dividing line */

#include "../internal.h"

optics_ui_response optics_ui_separator(optics_ui *ui, const optics_ui_separator_opts *opts) {
    optics_ui_separator_opts default_opts = {};
    if (!opts)
        opts = &default_opts;

    optics_ui__apply_box(ui, opts->box);
    optics_ui_style eff = optics_ui__style_effective(ui);
    optics_ui_style_resolved rs = optics_ui__style_resolve(ui, &eff, &ui->theme, 0);
    optics_ui_id id = opts->box.id ? optics_ui__gen_widget_id(ui, opts->box.id)
                                   : optics_ui__gen_widget_id(ui, "##sep");
    optics_ui_node *n = optics_ui__store_touch(ui, id);
    if (!n)
        return (optics_ui_response){};

    optics_ui__link_child(ui, n);
    optics_ui__node_box(ui, n, &opts->box);
    n->is_container = false;

    optics_ui_node *parent = optics_ui__open_container(ui);
    bool is_vert = (opts->axis == OPTICS_UI_COLUMN) ||
                   (opts->axis == 0 && parent && parent->axis == OPTICS_UI_ROW);

    float th = opts->thickness > 0.0f ? opts->thickness : 1.0f;
    float w = is_vert ? th : 0.0f;
    float h = is_vert ? 0.0f : th;
    if (n->fixed_w > 0)
        w = n->fixed_w;
    if (n->fixed_h > 0)
        h = n->fixed_h;
    n->measured = (optics_gfx_point){w, h};

    optics_ui__skin_emit(ui, n,
                         &(optics_ui_widget_record){
                             .kind = OPTICS_UI_WIDGET_SEPARATOR,
                             .state = 0,
                             .bounds = {0, 0, w, h},
                             .last_bounds = n->prev_rect,
                             .style = rs,
                             .style_fields = eff.fields,
                             .content = {.vertical = is_vert},
                         });

    optics_ui_response r = (optics_ui_response){.id = id, .rect = n->prev_rect};
    ui->last_response = r;
    return r;
}
