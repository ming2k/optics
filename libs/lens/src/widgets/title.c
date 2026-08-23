/* title.c — semantic heading widgets (ADR-0031 extension).
 *
 * lens_title   : single top-level title, theme font_size_title + bold.
 * lens_heading : section heading, level 1-3 maps to h1/h2/h3 sizes.
 */

#include "../internal.h"

static float heading_size(const lens *ui, const lens_theme *t, int level) {
    float token;
    switch (level) {
    case 1:
        token = t->font_size_h1;
        break;
    case 2:
        token = t->font_size_h2;
        break;
    case 3:
    default:
        token = t->font_size_h3;
        break;
    }
    return lensi_font_px(ui, token);
}

void lens_title(lens *ui, const char *text) {
    const lens_theme *t = &ui->theme;
    lens_style eff = lensi_style_effective(ui);
    lens_style_resolved rs = lensi_style_resolve(ui, &eff, t, 0);
    lens_id id = lensi_gen_widget_id(ui, text);
    lens_node *n = lensi_store_touch(ui, id);
    if (!n)
        return;
    lensi_link_child(ui, n);
    n->is_container = false;

    float size = lensi_font_px(ui, t->font_size_title);
    float weight = t->font_weight_bold;
    lens_text_metrics tm = lensi_text_measure_label(ui, text, size, weight);
    float w = (n->fixed_w > 0) ? n->fixed_w : tm.width + 2.0f * rs.padding;
    float h = (n->fixed_h > 0) ? n->fixed_h : tm.height + 2.0f * rs.padding;
    n->measured = (flux_point){w, h};

    lensi_node_semantics(ui, n, LENS_ROLE_LABEL, text, NULL, 0);

    /* emit — through the replaceable skin (ADR-0059). The heading size and
     * bold weight are theme-only tokens (no style atom), so they ride the
     * record as content. */
    lensi_skin_emit(
        ui, n,
        &(lens_widget_record){
            .kind = LENS_WIDGET_LABEL,
            .state = 0,
            .bounds = {0, 0, w, h},
            .last_bounds = n->prev_rect,
            .style = rs,
            .style_fields = eff.fields,
            .content = {.label = text, .text = tm, .text_size = size, .text_weight = weight},
        });

    ui->last_response = (lens_response){.id = id, .rect = n->prev_rect};
}

void lens_heading(lens *ui, const char *text, int level) {
    const lens_theme *t = &ui->theme;
    lens_style eff = lensi_style_effective(ui);
    lens_style_resolved rs = lensi_style_resolve(ui, &eff, t, 0);
    lens_id id = lensi_gen_widget_id(ui, text);
    lens_node *n = lensi_store_touch(ui, id);
    if (!n)
        return;
    lensi_link_child(ui, n);
    n->is_container = false;

    float size = heading_size(ui, t, level);
    float weight = t->font_weight_bold;
    lens_text_metrics tm = lensi_text_measure_label(ui, text, size, weight);
    float w = (n->fixed_w > 0) ? n->fixed_w : tm.width + 2.0f * rs.padding;
    float h = (n->fixed_h > 0) ? n->fixed_h : tm.height + 2.0f * rs.padding;
    n->measured = (flux_point){w, h};

    lensi_node_semantics(ui, n, LENS_ROLE_LABEL, text, NULL, 0);

    lensi_skin_emit(
        ui, n,
        &(lens_widget_record){
            .kind = LENS_WIDGET_LABEL,
            .state = 0,
            .bounds = {0, 0, w, h},
            .last_bounds = n->prev_rect,
            .style = rs,
            .style_fields = eff.fields,
            .content = {.label = text, .text = tm, .text_size = size, .text_weight = weight},
        });

    ui->last_response = (lens_response){.id = id, .rect = n->prev_rect};
}
