/* slider.c — horizontal and vertical float sliders (ADR-0008). */

#include "../internal.h"
#include <stdio.h>

#define LENS_SLIDER_DEFAULT_W 160.0f
#define LENS_SLIDER_DEFAULT_H 160.0f
#define LENS_SLIDER_KNOB 14.0f
#define LENS_SLIDER_TRACK 6.0f

static float slider_span(float min, float max) {
    return max > min ? max - min : 1.0f;
}

static float slider_step(float min, float max, float step) {
    return step > 0.0f ? step : slider_span(min, max) / 20.0f;
}

static float slider_clamp(float value, float min, float max) {
    return value < min ? min : (value > max ? max : value);
}

static bool set_slider_value(float *value, float next, float min, float max) {
    if (!value)
        return false;
    next = slider_clamp(next, min, max);
    if (fabsf(next - *value) <= 0.000001f)
        return false;
    *value = next;
    return true;
}

static bool adjust_from_keys(lens *ui, const lens_response *r, float *value, float min, float max,
                             float step) {
    if (!r->focused || !value)
        return false;
    float next = *value;
    bool handled = false;
    for (uint32_t i = 0; i < ui->input.key_count; i++) {
        lens_key_event key = ui->input.keys[i];
        if (!key.pressed)
            continue;
        if (key.key == LENS_KEY_UP || key.key == LENS_KEY_RIGHT) {
            next += step;
            handled = true;
        } else if (key.key == LENS_KEY_DOWN || key.key == LENS_KEY_LEFT) {
            next -= step;
            handled = true;
        } else if (key.key == LENS_KEY_HOME) {
            next = min;
            handled = true;
        } else if (key.key == LENS_KEY_END) {
            next = max;
            handled = true;
        }
    }
    return handled && set_slider_value(value, next, min, max);
}

static bool adjust_from_scroll(lens *ui, bool hovered, float *value, float min, float max,
                               float step) {
    if (!hovered || !value || fabsf(ui->input.scroll_y) <= 0.0001f)
        return false;
    float delta = ui->input.scroll_y;
    ui->input.scroll_y = 0.0f;
    return set_slider_value(value, *value + delta * slider_step(min, max, step), min, max);
}

bool lens_adjust_float_on_scroll(lens *ui, float *value, float min, float max, float step) {
    if (!ui)
        return false;
    bool changed = adjust_from_scroll(ui, ui->last_response.hovered, value, min, max, step);
    ui->last_response.changed = ui->last_response.changed || changed;
    return changed;
}

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
    n->hover_t =
        lensi_approach(ui, n->hover_t, reveal_knob ? 1.f : 0.f, dt, reveal_knob ? 18.f : 14.f);

    /* track geometry from last frame's width (one-frame latency, ADR-0006) */
    flux_rect rect = n->has_prev ? n->prev_rect : (flux_rect){0, 0, w, h};
    float track_x0 = t->padding + LENS_SLIDER_KNOB * 0.5f;
    float track_w = rect.w - 2.0f * t->padding - LENS_SLIDER_KNOB;
    if (track_w < 1.0f)
        track_w = 1.0f;

    float span = slider_span(min, max);
    if (!disabled && r.pressed && value) {
        float local = ui->input.cursor.x - rect.x - track_x0;
        float tt = local / track_w;
        tt = tt < 0 ? 0 : (tt > 1 ? 1 : tt);
        if (set_slider_value(value, min + tt * span, min, max)) {
            r.changed = true;
        }
    }
    if (!disabled && adjust_from_keys(ui, &r, value, min, max, span / 20.0f))
        r.changed = true;

    char vbuf[32];
    snprintf(vbuf, sizeof vbuf, "%.4g", (double)(value ? *value : 0.0f));
    uint32_t sem_flags = (r.focused ? LENS_A11Y_FOCUSED : 0) | (disabled ? LENS_A11Y_DISABLED : 0) |
                         (error ? LENS_A11Y_DISABLED : 0);
    lensi_node_semantics(ui, n, LENS_ROLE_SLIDER, label, vbuf, sem_flags);

    float frac = value ? (*value - min) / span : 0.0f;
    frac = frac < 0 ? 0 : (frac > 1 ? 1 : frac);

    flux_color track_color =
        disabled ? t->color_disabled : (error ? t->color_error : t->color_slider_track);
    flux_color fill_color =
        disabled ? t->color_disabled : (error ? t->color_error : t->color_slider_fill);
    /* track */
    lensi_drawlist_push(ui, n,
                        (lens_draw_cmd){.kind = LENS_DRAW_RECT,
                                        .rel = {track_x0, h * 0.5f - LENS_SLIDER_TRACK * 0.5f,
                                                track_w, LENS_SLIDER_TRACK},
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
                                                 disabled ? t->color_disabled
                                                 : error  ? t->color_error
                                                          : t->color_slider_knob,
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

bool lens_slider_vertical(lens *ui, const char *label, float *value, float min, float max,
                          float step) {
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

    float w = t->font_size + 2.0f * t->padding;
    float h = LENS_SLIDER_DEFAULT_H;
    if (n->fixed_w > 0.0f)
        w = n->fixed_w;
    if (n->fixed_h > 0.0f)
        h = n->fixed_h;
    n->measured = (flux_point){w, h};

    lens_response r = lensi_interact(ui, n, true, disabled);
    float dt = ui->input.dt_seconds;
    /* A vertical slider most often lives in a transient value popup. Keep its
     * knob visible so the current value remains legible before the pointer
     * crosses from the owner into the track. */
    n->hover_t =
        lensi_approach(ui, n->hover_t, disabled ? 0.0f : 1.0f, dt, disabled ? 14.0f : 18.0f);

    flux_rect rect = n->has_prev ? n->prev_rect : (flux_rect){0, 0, w, h};
    float track_y0 = t->padding + LENS_SLIDER_KNOB * 0.5f;
    float track_h = rect.h - 2.0f * t->padding - LENS_SLIDER_KNOB;
    if (track_h < 1.0f)
        track_h = 1.0f;
    float span = slider_span(min, max);

    if (!disabled && r.pressed && value) {
        float local = ui->input.cursor.y - rect.y - track_y0;
        float frac = 1.0f - local / track_h;
        frac = frac < 0.0f ? 0.0f : (frac > 1.0f ? 1.0f : frac);
        if (set_slider_value(value, min + frac * span, min, max))
            r.changed = true;
    }
    if (!disabled && adjust_from_scroll(ui, r.hovered, value, min, max, step))
        r.changed = true;
    if (!disabled && adjust_from_keys(ui, &r, value, min, max, slider_step(min, max, step)))
        r.changed = true;

    char vbuf[32];
    snprintf(vbuf, sizeof vbuf, "%.4g", (double)(value ? *value : 0.0f));
    uint32_t sem_flags = (r.focused ? LENS_A11Y_FOCUSED : 0) | (disabled ? LENS_A11Y_DISABLED : 0) |
                         (error ? LENS_A11Y_DISABLED : 0);
    lensi_node_semantics(ui, n, LENS_ROLE_SLIDER, label, vbuf, sem_flags);

    float frac = value ? (*value - min) / span : 0.0f;
    frac = frac < 0.0f ? 0.0f : (frac > 1.0f ? 1.0f : frac);
    float track_x = w * 0.5f - LENS_SLIDER_TRACK * 0.5f;
    flux_color track_color =
        disabled ? t->color_disabled : (error ? t->color_error : t->color_slider_track);
    flux_color fill_color =
        disabled ? t->color_disabled : (error ? t->color_error : t->color_slider_fill);

    lensi_drawlist_push(ui, n,
                        (lens_draw_cmd){.kind = LENS_DRAW_RECT,
                                        .rel = {track_x, track_y0, LENS_SLIDER_TRACK, track_h},
                                        .color = track_color,
                                        .radius = LENS_SLIDER_TRACK * 0.5f});
    float fill_h = frac * track_h;
    if (fill_h > 0.001f)
        lensi_drawlist_push(ui, n,
                            (lens_draw_cmd){.kind = LENS_DRAW_RECT,
                                            .rel = {track_x, track_y0 + track_h - fill_h,
                                                    LENS_SLIDER_TRACK, fill_h},
                                            .color = fill_color,
                                            .radius = LENS_SLIDER_TRACK * 0.5f});

    float knob_t = n->hover_t * n->hover_t * (3.0f - 2.0f * n->hover_t);
    if (knob_t > 0.001f) {
        float knob_size = LENS_SLIDER_KNOB * (0.72f + 0.28f * knob_t);
        float knob_x = w * 0.5f - knob_size * 0.5f;
        float knob_y = track_y0 + (1.0f - frac) * track_h - knob_size * 0.5f;
        flux_color knob_color = lensi_lerp_color(
            flux_color_rgba(0, 0, 0, 0),
            disabled ? t->color_disabled : (error ? t->color_error : t->color_slider_knob), knob_t);
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
