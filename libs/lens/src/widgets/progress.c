/* progress.c — horizontal progress bar (read-only, non-interactive). */

#include "../internal.h"

void lens_progress(lens *ui, const char *label, float value) {
    const lens_theme *t = &ui->theme;
    lens_id id = lensi_gen_widget_id(ui, label);
    lens_node *n = lensi_store_touch(ui, id);
    if (!n)
        return;
    lensi_link_child(ui, n);
    n->is_container = false;

    float bar_h = t->font_size * 0.6f;
    float w = (n->fixed_w > 0) ? n->fixed_w : 200.0f;
    float h = (n->fixed_h > 0) ? n->fixed_h : bar_h + 2.0f * t->padding;
    n->measured = (flux_point){w, h};

    lens_response r = lensi_interact(ui, n, false, false);

    float bar_y = (h - bar_h) * 0.5f;
    float fill = (value < 0.0f) ? 0.0f : (value > 1.0f ? 1.0f : value);
    float track_w = w - 2.0f * t->padding;
    float fill_w = track_w * fill;

    /* Track background */
    lensi_drawlist_push(ui, n,
                        (lens_draw_cmd){.kind = LENS_DRAW_RECT,
                                        .rel = {t->padding, bar_y, track_w, bar_h},
                                        .color = lensi_color_alpha(t->color_border, 0x40),
                                        .radius = bar_h * 0.5f});

    /* Fill. For tiny non-zero progress, a mathematically exact width can be
     * only a pixel or two, which reads as a square vertical tick rather than
     * the rounded cap of the track. Keep the visual fill at least one cap
     * diameter while preserving a true empty state at 0. */
    if (fill_w > 0.5f) {
        float visual_w = fill_w < bar_h ? bar_h : fill_w;
        if (visual_w > track_w)
            visual_w = track_w;
        lensi_drawlist_push(ui, n,
                            (lens_draw_cmd){.kind = LENS_DRAW_RECT,
                                            .rel = {t->padding, bar_y, visual_w, bar_h},
                                            .color = t->color_accent,
                                            .radius = bar_h * 0.5f});
    }

    uint32_t sem_flags = (r.focused ? LENS_A11Y_FOCUSED : 0);
    lensi_node_semantics(ui, n, LENS_ROLE_SLIDER, label, NULL, sem_flags);

    ui->last_response = r;
}
