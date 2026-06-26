/* separator.c — horizontal or vertical dividing line. */

#include "../internal.h"

void lens_separator(lens *ui) {
    const lens_theme *t = &ui->theme;
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

    if (row) {
        lensi_drawlist_push(ui, n,
                            (lens_draw_cmd){.kind = LENS_DRAW_RECT,
                                            .rel = {0, t->padding, 1.0f,
                                                    n->measured.y > 2.0f * t->padding
                                                        ? n->measured.y - 2.0f * t->padding
                                                        : 1.0f},
                                            .color = t->color_border,
                                            .radius = 0.5f});
    } else {
        lensi_drawlist_push(ui, n,
                            (lens_draw_cmd){.kind = LENS_DRAW_RECT,
                                            .rel = {t->padding, 0,
                                                    n->measured.x > 2.0f * t->padding
                                                        ? n->measured.x - 2.0f * t->padding
                                                        : 1.0f,
                                                    1.0f},
                                            .color = t->color_border,
                                            .radius = 0.5f});
    }
}
