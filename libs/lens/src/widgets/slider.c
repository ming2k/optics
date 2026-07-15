/* slider.c — horizontal float slider (ADR-0008). */

#include "../internal.h"
#include <stdio.h>

#define LENS_SLIDER_DEFAULT_W 160.0f
#define LENS_SLIDER_KNOB 14.0f

bool lens_slider(lens *ui, const char *label, float *value, float min, float max) {
    const lens_theme *t = &ui->theme;
    bool disabled = ui->next_disabled;
    bool error = ui->next_error;
    ui->next_disabled = false;
    ui->next_error = false;
    lens_id id = lensi_gen_widget_id(ui, label);
    lens_node *n = lensi_store_touch(ui, id);
    if (!n)
        return false;
    lensi_link_child(ui, n);
    n->is_container = false;

    float h = t->font_size + 2.0f * t->padding;
    float w = (n->fixed_w > 0) ? n->fixed_w : LENS_SLIDER_DEFAULT_W;
    if (n->fixed_h > 0)
        h = n->fixed_h;
    n->measured = (flux_point){w, h};

    lens_response r = lensi_interact(ui, n, true, disabled);

    float dt = ui->input.dt_seconds;
    bool reveal_knob = !disabled && (r.hovered || r.focused || r.pressed);
    n->hover_t = lensi_approach(ui, n->hover_t, reveal_knob ? 1.f : 0.f, dt,
                                reveal_knob ? 18.f : 14.f);

    /* track geometry from last frame's width (one-frame latency, ADR-0006) */
    flux_rect rect = n->has_prev ? n->prev_rect : (flux_rect){0, 0, w, h};
    float track_x0 = t->padding + LENS_SLIDER_KNOB * 0.5f;
    float track_w = rect.w - 2.0f * t->padding - LENS_SLIDER_KNOB;
    if (track_w < 1.0f)
        track_w = 1.0f;

    float span = (max > min) ? (max - min) : 1.0f;
    if (!disabled && r.pressed && value) {
        float local = ui->input.cursor.x - rect.x - track_x0;
        float tt = local / track_w;
        tt = tt < 0 ? 0 : (tt > 1 ? 1 : tt);
        float nv = min + tt * span;
        if (nv != *value) {
            *value = nv;
            r.changed = true;
        }
    }

    char vbuf[32];
    snprintf(vbuf, sizeof vbuf, "%.4g", (double)(value ? *value : 0.0f));
    uint32_t sem_flags = (r.focused ? LENS_A11Y_FOCUSED : 0) | (disabled ? LENS_A11Y_DISABLED : 0) |
                         (error ? LENS_A11Y_DISABLED : 0);
    lensi_node_semantics(ui, n, LENS_ROLE_SLIDER, label, vbuf, sem_flags);

    float frac = value ? (*value - min) / span : 0.0f;
    frac = frac < 0 ? 0 : (frac > 1 ? 1 : frac);

    flux_color track_color =
        disabled ? t->color_disabled : (error ? t->color_error : t->color_border);
    flux_color fill_color =
        disabled ? t->color_disabled : (error ? t->color_error : t->color_accent);
    /* track */
    lensi_drawlist_push(ui, n,
                        (lens_draw_cmd){.kind = LENS_DRAW_RECT,
                                        .rel = {track_x0, h * 0.5f - 3.0f, track_w, 6.0f},
                                        .color = track_color,
                                        .radius = 3.0f});

    /* Filled portion ends at the exact value. The knob is an interaction
     * affordance, not part of the value geometry, so hiding it must not move
     * or lengthen the fill. */
    float fill_w = frac * track_w;
    if (fill_w > 0.001f)
        lensi_drawlist_push(ui, n,
                            (lens_draw_cmd){.kind = LENS_DRAW_RECT,
                                            .rel = {track_x0, h * 0.5f - 3.0f, fill_w, 6.0f},
                                            .color = fill_color,
                                            .radius = 3.0f});

    /* Resting sliders are visually quiet. Hover, keyboard focus, and drag
     * reveal a knob with a short fade + scale transition. lensi_approach
     * marks animation pending, so input-driven hosts continue rendering until
     * the transition settles. */
    float knob_t = n->hover_t * n->hover_t * (3.0f - 2.0f * n->hover_t);
    if (knob_t > 0.001f) {
        float knob_size = LENS_SLIDER_KNOB * (0.72f + 0.28f * knob_t);
        float knob_x = track_x0 + frac * track_w - knob_size * 0.5f;
        float knob_y = h * 0.5f - knob_size * 0.5f;
        flux_color knob_color = lensi_lerp_color(flux_color_rgba(0, 0, 0, 0),
                                                 disabled ? t->color_disabled : t->color_accent,
                                                 knob_t);
        lensi_drawlist_push(ui, n,
                            (lens_draw_cmd){.kind = LENS_DRAW_RECT,
                                            .rel = {knob_x, knob_y, knob_size, knob_size},
                                            .color = knob_color,
                                            .radius = knob_size * 0.5f});
    }

    ui->last_response = r;
    return r.changed;
}

lens_response lens_slider_ex(lens *ui, lens_slider_opts o) {
    lensi_apply_box(ui, o.box);
    bool scoped = o.box.id && o.box.id[0];
    if (scoped)
        lens_push_id(ui, o.box.id);
    lens_slider(ui, o.label ? o.label : "", o.value, o.min, o.max);
    if (scoped)
        lens_pop_id(ui);
    if (o.box.tooltip)
        lensi_tooltip(ui, o.box.tooltip);
    return ui->last_response;
}
