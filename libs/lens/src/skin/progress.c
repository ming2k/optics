/* skin/progress.c — default progress-bar skin (ADR-0059): track + fill,
 * moved verbatim from the widget. The fill keeps at least one cap diameter
 * for tiny non-zero values (a 2px sliver reads as a square tick, not a
 * rounded cap) while preserving a true empty state at 0. */

#include "../internal.h"

void lensi_skin_progress(lens *ui, lens_node *n, const lens_widget_record *rec) {
    const lens_style_resolved *rs = &rec->style;
    float w = rec->bounds.w;
    float h = rec->bounds.h;

    float bar_h = rs->font_size * 0.6f;
    float bar_y = (h - bar_h) * 0.5f;
    float fill = rec->content.ratio;
    float track_w = w - 2.0f * rs->padding;
    float fill_w = track_w * fill;

    /* Track background */
    lensi_drawlist_push(ui, n,
                        (lens_draw_cmd){.kind = LENS_DRAW_RECT,
                                        .rel = {rs->padding, bar_y, track_w, bar_h},
                                        .color = lensi_color_alpha(rs->border, 0x40),
                                        .radius = bar_h * 0.5f});

    if (fill_w > 0.5f) {
        float visual_w = fill_w < bar_h ? bar_h : fill_w;
        if (visual_w > track_w)
            visual_w = track_w;
        lensi_drawlist_push(ui, n,
                            (lens_draw_cmd){.kind = LENS_DRAW_RECT,
                                            .rel = {rs->padding, bar_y, visual_w, bar_h},
                                            .color = rs->accent,
                                            .radius = bar_h * 0.5f});
    }
}
