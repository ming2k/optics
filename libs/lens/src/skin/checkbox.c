/* skin/checkbox.c — default checkbox/switch/radio skin (ADR-0059).
 * Renders based on content.appearance (BOX, SWITCH, RADIO). */

#include "../internal.h"
#include <math.h>

void lensi_skin_checkbox(lens *ui, lens_node *n, const lens_widget_record *rec) {
    const lens_style_resolved *rs = &rec->style;
    bool disabled = (rec->state & LENS_STATE_DISABLED) != 0;
    bool on = (rec->state & LENS_STATE_ACTIVE) != 0 || (rec->state & LENS_STATE_SELECTED) != 0;

    if (rec->content.appearance == LENS_CHECKBOX_SWITCH) {
        const float track_w = 38.0f;
        const float track_h = 22.0f;
        const float knob_d = 16.0f;
        const float travel = track_w - knob_d - 6.0f;

        float w = rec->bounds.w;
        float h = rec->bounds.h;
        float text_y = fmaxf((h - rs->font_size) * 0.5f - 1.0f, 0.0f);
        float track_y = fmaxf((h - track_h) * 0.5f, 0.0f);
        float track_x = w - track_w - rs->padding;

        /* Row hover wash */
        if (rec->hover_t > 0.001f && !disabled) {
            flux_color wash = lensi_lerp_color(0, rs->bg_hover, rec->hover_t * 0.35f);
            lensi_drawlist_push(ui, n,
                                (lens_draw_cmd){
                                    .kind = LENS_DRAW_RECT,
                                    .rel = {0, 0, 0, 0},
                                    .color = wash,
                                    .radius = rs->corner_radius > 0 ? rs->corner_radius : 4.0f,
                                });
        }

        /* Label */
        if (rec->content.label && rec->content.label[0]) {
            flux_color fg = disabled ? rs->disabled : rs->fg;
            lensi_drawlist_push(ui, n,
                                (lens_draw_cmd){
                                    .kind = LENS_DRAW_TEXT,
                                    .rel = {rs->padding, text_y, 0, 0},
                                    .color = fg,
                                    .text = rec->content.label,
                                    .text_size = rs->font_size,
                                });
        }

        /* Track */
        flux_color track_off = rs->bg;
        flux_color track_on = rs->accent;
        flux_color track_col =
            disabled ? rs->disabled
                     : (on ? track_on : lensi_lerp_color(track_off, rs->bg_hover, rec->hover_t));

        lensi_drawlist_push(ui, n,
                            (lens_draw_cmd){
                                .kind = LENS_DRAW_RECT,
                                .rel = {track_x, track_y, track_w, track_h},
                                .color = track_col,
                                .radius = track_h * 0.5f,
                            });

        /* Knob */
        float knob_x = track_x + 3.0f + (on ? travel : 0.0f);
        float knob_y = track_y + 3.0f;
        flux_color knob_col = disabled ? rs->disabled : rs->fg;

        lensi_drawlist_push(ui, n,
                            (lens_draw_cmd){
                                .kind = LENS_DRAW_RECT,
                                .rel = {knob_x, knob_y, knob_d, knob_d},
                                .color = knob_col,
                                .radius = knob_d * 0.5f,
                            });
        return;
    }

    if (rec->content.appearance == LENS_CHECKBOX_RADIO) {
        float circle = roundf(rs->font_size);
        float circle_y = roundf((rec->bounds.h - circle) * 0.5f);
        float label_x = rs->padding + circle + 8.0f;
        float text_y = fmaxf((rec->bounds.h - rs->font_size) * 0.5f - 1.0f, 0.0f);

        flux_color ring = disabled ? rs->border : (on ? rs->accent : rs->fg);
        flux_color radio_bg = (rs->border_width > 0.0f) ? rs->bg : rs->bg_hover;
        lensi_drawlist_push(ui, n,
                            (lens_draw_cmd){
                                .kind = LENS_DRAW_RECT,
                                .rel = {rs->padding, circle_y, circle, circle},
                                .color = radio_bg,
                                .radius = circle * 0.5f,
                            });
        if (rs->border_width > 0.0f && (ring & 0xFF000000u) != 0) {
            lensi_drawlist_push(ui, n,
                                (lens_draw_cmd){
                                    .kind = LENS_DRAW_BORDER,
                                    .rel = {rs->padding, circle_y, circle, circle},
                                    .color = ring,
                                    .width = rs->border_width,
                                    .radius = circle * 0.5f,
                                });
        }

        if (on) {
            float dot = roundf(circle * 0.5f);
            float dot_x = rs->padding + roundf((circle - dot) * 0.5f);
            float dot_y = circle_y + roundf((circle - dot) * 0.5f);
            flux_color dot_col = disabled ? rs->disabled : rs->accent;
            lensi_drawlist_push(ui, n,
                                (lens_draw_cmd){
                                    .kind = LENS_DRAW_RECT,
                                    .rel = {dot_x, dot_y, dot, dot},
                                    .color = dot_col,
                                    .radius = dot * 0.5f,
                                });
        }

        if (rec->content.label && rec->content.label[0]) {
            flux_color fg = disabled ? rs->disabled : rs->fg;
            lensi_drawlist_push(ui, n,
                                (lens_draw_cmd){
                                    .kind = LENS_DRAW_TEXT,
                                    .rel = {label_x, text_y, 0, 0},
                                    .color = fg,
                                    .text = rec->content.label,
                                    .text_size = rs->font_size,
                                });
        }
        return;
    }

    /* Default BOX appearance */
    float box = rs->font_size;
    float box_y = roundf((rec->bounds.h - box) * 0.5f);
    float label_x = rs->padding + box + 8.0f;
    float text_y = fmaxf((rec->bounds.h - rs->font_size) * 0.5f - 1.0f, 0.0f);
    float radius = rs->corner_radius > 0.0f ? fminf(rs->corner_radius, 4.0f) : 3.0f;

    flux_color off_bg = (rs->border_width > 0.0f) ? rs->bg : rs->bg_hover;
    flux_color box_bg = on ? (disabled ? rs->disabled : rs->accent) : off_bg;

    lensi_drawlist_push(ui, n,
                        (lens_draw_cmd){
                            .kind = LENS_DRAW_RECT,
                            .rel = {rs->padding, box_y, box, box},
                            .color = box_bg,
                            .radius = radius,
                        });

    flux_color border_col = disabled ? rs->border : rs->fg;
    if (rs->border_width > 0.0f && (border_col & 0xFF000000u) != 0 && !on) {
        lensi_drawlist_push(ui, n,
                            (lens_draw_cmd){
                                .kind = LENS_DRAW_BORDER,
                                .rel = {rs->padding, box_y, box, box},
                                .color = border_col,
                                .width = rs->border_width,
                                .radius = radius,
                            });
    }

    if (on) {
        float mark = roundf(box * 0.5f);
        float mark_x = rs->padding + roundf((box - mark) * 0.5f);
        float mark_y = box_y + roundf((box - mark) * 0.5f);
        flux_color mark_col = disabled ? rs->disabled : flux_color_rgba_premul(255, 255, 255, 255);
        lensi_drawlist_push(ui, n,
                            (lens_draw_cmd){
                                .kind = LENS_DRAW_RECT,
                                .rel = {mark_x, mark_y, mark, mark},
                                .color = mark_col,
                                .radius = 1.0f,
                            });
    }

    if (rec->content.label && rec->content.label[0]) {
        flux_color fg = disabled ? rs->disabled : rs->fg;
        lensi_drawlist_push(ui, n,
                            (lens_draw_cmd){
                                .kind = LENS_DRAW_TEXT,
                                .rel = {label_x, text_y, 0, 0},
                                .color = fg,
                                .text = rec->content.label,
                                .text_size = rs->font_size,
                            });
    }
}
