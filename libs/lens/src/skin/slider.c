/* skin/slider.c — default slider skin, horizontal and vertical
 * (ADR-0059). Neutral default per ADR-0061: quiet track, fill to the exact
 * value, knob always visible at full size — the knob is the slider's
 * interaction affordance and is never hidden in service of a "visually
 * quiet" look (the pre-0061 fade+scale reveal was flavor). Hover/drag
 * feedback is colour-only, matching every other widget. The slider tokens
 * (track/knob geometry and colours, error colour) have no lens_style slot
 * yet, so the skin reads them from the theme; the common tokens come from
 * the resolved style. Track geometry uses last frame's arranged rect (the
 * documented one-frame latency, ADR-0029). */

#include "../internal.h"

void lensi_skin_slider(lens *ui, lens_node *n, const lens_widget_record *rec) {
    const lens_style_resolved *rs = &rec->style;
    const lens_theme *t = &ui->theme; /* slider tokens (no style slots yet) */
    bool disabled = (rec->state & LENS_STATE_DISABLED) != 0;
    bool error = rec->content.error;
    float frac = rec->content.ratio;
    float w = rec->bounds.w;
    float h = rec->bounds.h;

    /* track geometry from last frame's extent (one-frame latency, ADR-0029) */
    flux_rect rect = n->has_prev ? n->prev_rect : (flux_rect){0, 0, w, h};
    float track_thickness = t->slider_track_thickness;
    float knob_extent = t->slider_knob_size;
    float track_radius = track_thickness * 0.5f;

    flux_color track_color =
        disabled ? rs->disabled : (error ? t->color_error : t->color_slider_track);
    flux_color fill_color =
        disabled ? rs->disabled : (error ? t->color_error : t->color_slider_fill);
    flux_color knob_target =
        disabled ? rs->disabled : (error ? t->color_error : t->color_slider_knob);

    /* Knob affordance is always present (ADR-0061); hover and drag only
     * shift its colour toward the accent — no scale, no alpha reveal.
     * lensi_approach on hover_t/active_t marks animation pending, so
     * input-driven hosts keep rendering until the colour settles. */
    float emphasis = fmaxf(rec->hover_t * 0.35f, rec->active_t * 0.6f);
    flux_color knob_color =
        disabled || error ? knob_target : lensi_lerp_color(knob_target, rs->accent, emphasis);

    if (!rec->content.vertical) {
        float track_x0 = rs->padding + knob_extent * 0.5f;
        float track_w = rect.w - 2.0f * rs->padding - knob_extent;
        if (track_w < 1.0f)
            track_w = 1.0f;

        /* track */
        lensi_drawlist_push(
            ui, n,
            (lens_draw_cmd){.kind = LENS_DRAW_RECT,
                            .rel = {track_x0, h * 0.5f - track_radius, track_w, track_thickness},
                            .color = track_color,
                            .radius = track_radius});

        /* Filled portion ends at the exact value. The knob is an interaction
         * affordance, not part of the value geometry, so it must not move
         * or lengthen the fill. */
        float fill_w = frac * track_w;
        if (fill_w > 0.001f)
            lensi_drawlist_push(
                ui, n,
                (lens_draw_cmd){.kind = LENS_DRAW_RECT,
                                .rel = {track_x0, h * 0.5f - track_radius, fill_w, track_thickness},
                                .color = fill_color,
                                .radius = track_radius});

        float knob_x = track_x0 + frac * track_w - knob_extent * 0.5f;
        float knob_y = h * 0.5f - knob_extent * 0.5f;
        lensi_drawlist_push(ui, n,
                            (lens_draw_cmd){.kind = LENS_DRAW_RECT,
                                            .rel = {knob_x, knob_y, knob_extent, knob_extent},
                                            .color = knob_color,
                                            .radius = knob_extent * 0.5f});
        return;
    }

    float track_y0 = rs->padding + knob_extent * 0.5f;
    float track_h = rect.h - 2.0f * rs->padding - knob_extent;
    if (track_h < 1.0f)
        track_h = 1.0f;
    float track_x = w * 0.5f - track_thickness * 0.5f;

    lensi_drawlist_push(ui, n,
                        (lens_draw_cmd){.kind = LENS_DRAW_RECT,
                                        .rel = {track_x, track_y0, track_thickness, track_h},
                                        .color = track_color,
                                        .radius = track_thickness * 0.5f});
    float fill_h = frac * track_h;
    if (fill_h > 0.001f)
        lensi_drawlist_push(
            ui, n,
            (lens_draw_cmd){.kind = LENS_DRAW_RECT,
                            .rel = {track_x, track_y0 + track_h - fill_h, track_thickness, fill_h},
                            .color = fill_color,
                            .radius = track_thickness * 0.5f});

    float knob_x = w * 0.5f - knob_extent * 0.5f;
    float knob_y = track_y0 + (1.0f - frac) * track_h - knob_extent * 0.5f;
    lensi_drawlist_push(ui, n,
                        (lens_draw_cmd){.kind = LENS_DRAW_RECT,
                                        .rel = {knob_x, knob_y, knob_extent, knob_extent},
                                        .color = knob_color,
                                        .radius = knob_extent * 0.5f});
}
