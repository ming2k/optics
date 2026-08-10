/* skin/switch.c — default switch-row skin (ADR-0059). The pre-skin emit
 * section of lens_switch, moved verbatim: hover wash, label (+ optional
 * description) on the left, trailing track with the knob riding the eased
 * active float. */

#include "../internal.h"

void lensi_skin_switch(lens *ui, lens_node *n, const lens_widget_record *rec) {
    const lens_style_resolved *rs = &rec->style;
    bool disabled = (rec->state & LENS_STATE_DISABLED) != 0;

    const float track_w = 38.0f;
    const float track_h = 22.0f;
    const float knob_pad = 2.0f;
    const float knob = track_h - 2.0f * knob_pad;
    const float desc_size = rs->font_size * 0.84f;
    const float text_gap = 3.0f;
    bool has_description = rec->content.description && rec->content.description[0];
    float text_h = rec->content.text.height +
                   (has_description ? text_gap + rec->content.desc_text.height : 0.0f);

    if (rec->hover_t > 0.001f) {
        lensi_drawlist_push(
            ui, n,
            (lens_draw_cmd){.kind = LENS_DRAW_RECT,
                            .rel = {0, 0, 0, 0},
                            .color = lensi_lerp_color(rs->bg, rs->bg_hover, rec->hover_t),
                            .radius = rs->corner_radius});
    }

    float text_y = (rec->bounds.h - text_h) * 0.5f;
    flux_color label_color = disabled ? rs->disabled : rs->fg;
    lensi_drawlist_push(ui, n,
                        (lens_draw_cmd){.kind = LENS_DRAW_TEXT,
                                        .rel = {rs->padding, text_y, 0, 0},
                                        .color = label_color,
                                        .text = rec->content.label,
                                        .text_size = rs->font_size});
    if (has_description) {
        lensi_drawlist_push(
            ui, n,
            (lens_draw_cmd){.kind = LENS_DRAW_TEXT,
                            .rel = {rs->padding, text_y + rec->content.text.height + text_gap, 0, 0},
                            .color = rs->disabled,
                            .text = rec->content.description,
                            .text_size = desc_size});
    }

    float track_y = (rec->bounds.h - track_h) * 0.5f;
    flux_color track_off = lensi_lerp_color(rs->border, rs->fg, 0.18f);
    flux_color track_color =
        disabled ? rs->disabled : lensi_lerp_color(track_off, rs->accent, rec->active_t);
    lensi_drawlist_push(ui, n,
                        (lens_draw_cmd){.kind = LENS_DRAW_RECT,
                                        .rel = {-rs->padding, track_y, track_w, track_h},
                                        .color = track_color,
                                        .radius = track_h * 0.5f});

    float travel = track_w - 2.0f * knob_pad - knob;
    float knob_right = rs->padding + knob_pad + (1.0f - rec->active_t) * travel;
    flux_color knob_color = disabled ? rs->bg : rs->fg;
    lensi_drawlist_push(ui, n,
                        (lens_draw_cmd){.kind = LENS_DRAW_RECT,
                                        .rel = {-knob_right, track_y + knob_pad, knob, knob},
                                        .color = knob_color,
                                        .radius = knob * 0.5f});
}
