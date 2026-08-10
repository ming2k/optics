/* separator.c — horizontal or vertical dividing line. */

#include "../internal.h"

void lens_separator(lens *ui) {
    lens_style eff = lensi_style_effective(ui);
    lens_style_resolved rs = lensi_style_resolve(&eff, &ui->theme, 0);
    lens_id id = lensi_gen_widget_id(ui, "##sep");
    lens_node *n = lensi_store_touch(ui, id);
    if (!n)
        return;
    lensi_link_child(ui, n);
    n->is_container = false;

    lens_node *parent = lensi_open_container(ui);
    bool row = parent && parent->axis == LENS_ROW;

    float w = row ? 1.0f : 0.0f;
    float h = row ? 0.0f : 1.0f;
    if (n->fixed_w > 0)
        w = n->fixed_w;
    if (n->fixed_h > 0)
        h = n->fixed_h;
    n->measured = (flux_point){w, h};

    /* emit — through the replaceable skin (ADR-0059) */
    lensi_skin_emit(ui, n,
                    &(lens_widget_record){
                        .kind = LENS_WIDGET_SEPARATOR,
                        .state = 0,
                        .bounds = {0, 0, w, h},
                        .last_bounds = n->prev_rect,
                        .style = rs,
                        .style_fields = eff.fields,
                        .content = {.vertical = row},
                    });
}
