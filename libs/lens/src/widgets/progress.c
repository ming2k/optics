/* progress.c — horizontal progress bar (read-only, non-interactive). */

#include "../internal.h"

void lens_progress(lens *ui, const char *label, float value) {
    lens_style eff = lensi_style_effective(ui);
    float font_size = lensi_style_font_size(&eff, &ui->theme);
    float padding = lensi_style_padding(&eff, &ui->theme);
    lens_style_resolved rs = lensi_style_resolve(&eff, &ui->theme, 0);
    lens_id id = lensi_gen_widget_id(ui, label);
    lens_node *n = lensi_store_touch(ui, id);
    if (!n)
        return;
    lensi_link_child(ui, n);
    n->is_container = false;

    float bar_h = font_size * 0.6f;
    float w = (n->fixed_w > 0) ? n->fixed_w : 200.0f;
    float h = (n->fixed_h > 0) ? n->fixed_h : bar_h + 2.0f * padding;
    n->measured = (flux_point){w, h};

    lens_response r = lensi_interact(ui, n, false, false);

    float fill = (value < 0.0f) ? 0.0f : (value > 1.0f ? 1.0f : value);

    uint32_t sem_flags = (r.focused ? LENS_A11Y_FOCUSED : 0);
    lensi_node_semantics(ui, n, LENS_ROLE_PROGRESS, label, NULL, sem_flags);

    /* emit — through the replaceable skin (ADR-0059) */
    lensi_skin_emit(ui, n,
                    &(lens_widget_record){
                        .kind = LENS_WIDGET_PROGRESS,
                        .state = r.state,
                        .bounds = {0, 0, w, h},
                        .last_bounds = n->prev_rect,
                        .style = rs,
                        .style_fields = eff.fields,
                        .hover_t = n->hover_t,
                        .active_t = n->active_t,
                        .content = {.label = label, .ratio = fill},
                    });

    ui->last_response = r;
}
