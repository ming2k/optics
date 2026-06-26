/* label.c — static text (ADR-0008). Non-interactive. */

#include "../internal.h"

void lens_label(lens *ui, const char *text) {
    const lens_theme *t = &ui->theme;
    lens_id id = lensi_gen_widget_id(ui, text);
    lens_node *n = lensi_store_touch(ui, id);
    if (!n)
        return;
    lensi_link_child(ui, n);
    n->is_container = false;

    lens_text_metrics tm = lensi_text_measure_label(ui, text, t->font_size, 0.0f);
    float w = (n->fixed_w > 0) ? n->fixed_w : tm.width + 2.0f * t->padding;
    float h = (n->fixed_h > 0) ? n->fixed_h : tm.height + 2.0f * t->padding;
    n->measured = (flux_point){w, h};

    lensi_node_semantics(ui, n, LENS_ROLE_LABEL, text, NULL, 0);

    lensi_drawlist_push(ui, n,
                        (lens_draw_cmd){.kind = LENS_DRAW_TEXT,
                                        .rel = {t->padding, t->padding, 0, 0},
                                        .color = t->color_fg,
                                        .text = text,
                                        .text_size = t->font_size,
                                        .text_weight = 0.0f});

    ui->last_response = (lens_response){.id = id, .rect = n->prev_rect};
}

void lens_label_ex(lens *ui, const char *text, float size) {
    const lens_theme *t = &ui->theme;
    lens_id id = lensi_gen_widget_id(ui, text);
    lens_node *n = lensi_store_touch(ui, id);
    if (!n)
        return;
    lensi_link_child(ui, n);
    n->is_container = false;

    lens_text_metrics tm = lensi_text_measure_label(ui, text, size, 0.0f);
    float w = (n->fixed_w > 0) ? n->fixed_w : tm.width + 2.0f * t->padding;
    float h = (n->fixed_h > 0) ? n->fixed_h : tm.height + 2.0f * t->padding;
    n->measured = (flux_point){w, h};

    lensi_node_semantics(ui, n, LENS_ROLE_LABEL, text, NULL, 0);

    lensi_drawlist_push(ui, n,
                        (lens_draw_cmd){.kind = LENS_DRAW_TEXT,
                                        .rel = {t->padding, t->padding, 0, 0},
                                        .color = t->color_fg,
                                        .text = text,
                                        .text_size = size,
                                        .text_weight = 0.0f});

    ui->last_response = (lens_response){.id = id, .rect = n->prev_rect};
}
