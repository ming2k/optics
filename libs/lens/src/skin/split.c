/* skin/split.c — default split-divider skin (ADR-0059): the handle strip,
 * moved verbatim from lens_split_end. The ratio state machine and the pane
 * sizing (fixed main-axis extents) are layout behaviour and stay in the
 * widget; only the strip's chrome lives here. */

#include "../internal.h"

void lensi_skin_split(lens *ui, lens_node *n, const lens_widget_record *rec) {
    const lens_style_resolved *rs = &rec->style;
    bool hov = (rec->state & (LENS_STATE_HOVERED | LENS_STATE_DRAGGED)) != 0;
    float thick = rec->content.split_thickness;
    float first_len = rec->content.split_pos;

    /* The strip draws in last-frame space; nothing to draw on the first
     * frame (the one-frame latency, ADR-0029). */
    if (rec->last_bounds.w <= 0.0f && rec->last_bounds.h <= 0.0f)
        return;

    flux_color hc = hov ? rs->bg_pressed : rs->border;
    if (rec->content.vertical) {
        lensi_drawlist_push(
            ui, n,
            (lens_draw_cmd){.kind = LENS_DRAW_RECT,
                            .rel = {first_len - thick * 0.5f, 0, thick, rec->last_bounds.h},
                            .color = hc,
                            .radius = thick * 0.5f});
    } else {
        lensi_drawlist_push(
            ui, n,
            (lens_draw_cmd){.kind = LENS_DRAW_RECT,
                            .rel = {0, first_len - thick * 0.5f, rec->last_bounds.w, thick},
                            .color = hc,
                            .radius = thick * 0.5f});
    }
}
