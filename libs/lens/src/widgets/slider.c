/* slider.c — horizontal and vertical float sliders (ADR-0031). */

#include "../internal.h"
#include <stdio.h>

#define LENS_SLIDER_DEFAULT_W 160.0f
#define LENS_SLIDER_DEFAULT_H 160.0f

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
    float delta = ui->input.scroll_y + ui->input.scroll_pixels_y / 40.0f;
    if (!hovered || !value || fabsf(delta) <= 0.0001f)
        return false;
    ui->input.scroll_y = 0.0f;
    ui->input.scroll_pixels_y = 0.0f;
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
    lens_style eff = lensi_style_effective(ui);
    float padding = lensi_style_padding(&eff, t);
    lens_id id = lensi_gen_widget_id(ui, label);
    lens_node *n = lensi_store_touch(ui, id);
    if (!n)
        return false;
    lensi_link_child(ui, n);
    n->is_container = false;

    float h = lensi_style_font_size(&eff, t) + 2.0f * padding;
    float w = (n->fixed_w > 0) ? n->fixed_w : LENS_SLIDER_DEFAULT_W;
    if (n->fixed_h > 0)
        h = n->fixed_h;
    n->measured = (flux_point){w, h};

    lens_response r = lensi_interact(ui, n, true, disabled);

    float dt = ui->input.dt_seconds;
    bool reveal_knob = !disabled && (r.hovered || r.focused || r.pressed);
    n->hover_t =
        lensi_approach(ui, n->hover_t, reveal_knob ? 1.f : 0.f, dt, reveal_knob ? 18.f : 14.f);

    /* track geometry from last frame's width (one-frame latency, ADR-0029);
     * the same math runs in the skin for drawing */
    flux_rect rect = n->has_prev ? n->prev_rect : (flux_rect){0, 0, w, h};
    float knob_extent = t->slider_knob_size;
    float track_x0 = padding + knob_extent * 0.5f;
    float track_w = rect.w - 2.0f * padding - knob_extent;
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
    /* A captured slider is being dragged: the knob tracks the pointer. */
    if (r.pressed)
        r.state |= LENS_STATE_DRAGGED;
    if (!disabled && adjust_from_keys(ui, &r, value, min, max, span / 20.0f))
        r.changed = true;

    char vbuf[32];
    snprintf(vbuf, sizeof vbuf, "%.4g", (double)(value ? *value : 0.0f));
    uint32_t sem_flags = (r.focused ? LENS_A11Y_FOCUSED : 0) | (disabled ? LENS_A11Y_DISABLED : 0) |
                         (error ? LENS_A11Y_DISABLED : 0);
    lensi_node_semantics(ui, n, LENS_ROLE_SLIDER, label, vbuf, sem_flags);

    float frac = value ? (*value - min) / span : 0.0f;
    frac = frac < 0 ? 0 : (frac > 1 ? 1 : frac);

    /* resolve + emit — through the replaceable skin (ADR-0059) */
    lensi_skin_emit(ui, n,
                    &(lens_widget_record){
                        .kind = LENS_WIDGET_SLIDER,
                        .state = r.state,
                        .bounds = {0, 0, w, h},
                        .last_bounds = n->prev_rect,
                        .style = lensi_style_resolve(&eff, t, r.state),
                        .style_fields = eff.fields,
                        .hover_t = n->hover_t,
                        .active_t = n->active_t,
                        .content = {.label = label, .ratio = frac, .error = error},
                    });

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
    lens_style eff = lensi_style_effective(ui);
    float padding = lensi_style_padding(&eff, t);
    lens_id id = lensi_gen_widget_id(ui, label);
    lens_node *n = lensi_store_touch(ui, id);
    if (!n)
        return false;
    lensi_link_child(ui, n);
    n->is_container = false;

    float w = lensi_style_font_size(&eff, t) + 2.0f * padding;
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
    float knob_extent = t->slider_knob_size;
    float track_y0 = padding + knob_extent * 0.5f;
    float track_h = rect.h - 2.0f * padding - knob_extent;
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
    /* A captured slider is being dragged: the knob tracks the pointer. */
    if (r.pressed)
        r.state |= LENS_STATE_DRAGGED;
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

    /* resolve + emit — through the replaceable skin (ADR-0059) */
    lensi_skin_emit(ui, n,
                    &(lens_widget_record){
                        .kind = LENS_WIDGET_SLIDER,
                        .state = r.state,
                        .bounds = {0, 0, w, h},
                        .last_bounds = n->prev_rect,
                        .style = lensi_style_resolve(&eff, t, r.state),
                        .style_fields = eff.fields,
                        .hover_t = n->hover_t,
                        .active_t = n->active_t,
                        .content = {.label = label, .ratio = frac, .vertical = true, .error = error},
                    });

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
