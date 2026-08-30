/* slider.c — scalar value dragging widget (horizontal or vertical) */

#include "../internal.h"
#include <math.h>
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

lens_response lens_slider(lens *ui, const lens_slider_opts *opts) {
    lens_slider_opts default_opts = {0};
    if (!opts)
        opts = &default_opts;

    lensi_apply_box(ui, opts->box);
    const lens_theme *t = &ui->theme;
    bool disabled = ui->next_disabled;
    bool error = ui->next_error;
    ui->next_disabled = false;
    ui->next_error = false;

    lens_style eff = lensi_style_effective(ui);
    float padding = lensi_style_padding(&eff, t);
    const char *label = opts->label ? opts->label : "";
    lens_id id =
        opts->box.id ? lensi_gen_widget_id(ui, opts->box.id) : lensi_gen_widget_id(ui, label);
    lens_node *n = lensi_store_touch(ui, id);
    if (!n)
        return (lens_response){0};

    lensi_link_child(ui, n);
    n->is_container = false;

    bool is_vert = (opts->axis == LENS_COLUMN);
    float font_size = lensi_style_font_size(ui, &eff, t);
    float w, h;
    if (is_vert) {
        w = (n->fixed_w > 0) ? n->fixed_w : (font_size + 2.0f * padding);
        h = (n->fixed_h > 0) ? n->fixed_h : LENS_SLIDER_DEFAULT_H;
    } else {
        h = (n->fixed_h > 0) ? n->fixed_h : (font_size + 2.0f * padding);
        w = (n->fixed_w > 0) ? n->fixed_w : LENS_SLIDER_DEFAULT_W;
    }
    n->measured = (flux_point){w, h};

    lens_response r = lensi_interact(ui, n, true, disabled);

    float dt = ui->input.dt_seconds;
    bool reveal_knob = !disabled && (r.hovered || r.focused || r.pressed);
    n->hover_t = lensi_approach(ui, n->hover_t, reveal_knob ? 1.0f : 0.0f, dt, 12.0f);

    float min = opts->min;
    float max = opts->max > min ? opts->max : min + 1.0f;
    float step = slider_step(min, max, opts->step);
    float *value = opts->value;
    float cur = value ? slider_clamp(*value, min, max) : min;
    if (value)
        *value = cur;

    if (!disabled && value) {
        if ((r.pressed || ui->active_id == id) && ui->input.mouse_down[LENS_MOUSE_LEFT]) {
            float ratio;
            if (is_vert) {
                float track_h = fmaxf(n->prev_rect.h - 2.0f * padding, 1.0f);
                float py = ui->input.cursor.y - (n->prev_rect.y + padding);
                ratio = 1.0f - (py / track_h);
            } else {
                float track_w = fmaxf(n->prev_rect.w - 2.0f * padding, 1.0f);
                float px = ui->input.cursor.x - (n->prev_rect.x + padding);
                ratio = px / track_w;
            }
            ratio = ratio < 0.0f ? 0.0f : (ratio > 1.0f ? 1.0f : ratio);
            float target = min + ratio * slider_span(min, max);
            if (opts->step > 0.0f)
                target = min + roundf((target - min) / opts->step) * opts->step;
            r.changed = set_slider_value(value, target, min, max) || r.changed;
        }

        r.changed = adjust_from_keys(ui, &r, value, min, max, step) || r.changed;
        r.changed = adjust_from_scroll(ui, r.hovered, value, min, max, step) || r.changed;
    }

    cur = value ? *value : min;
    float span = slider_span(min, max);
    float ratio = span > 0.0f ? (cur - min) / span : 0.0f;

    char val_str[32];
    snprintf(val_str, sizeof(val_str), opts->format ? opts->format : "%.2f", (double)cur);
    uint32_t sem_flags = (r.focused ? LENS_A11Y_FOCUSED : 0) | (disabled ? LENS_A11Y_DISABLED : 0);
    lensi_node_semantics(ui, n, LENS_ROLE_SLIDER, label, val_str, sem_flags);

    lens_style_resolved rs = lensi_style_resolve(ui, &eff, t, r.state);
    lensi_skin_emit(ui, n,
                    &(lens_widget_record){
                        .kind = LENS_WIDGET_SLIDER,
                        .state = r.state,
                        .bounds = {0, 0, w, h},
                        .last_bounds = n->prev_rect,
                        .style = rs,
                        .style_fields = eff.fields,
                        .hover_t = n->hover_t,
                        .active_t = n->active_t,
                        .content =
                            {
                                .label = label,
                                .ratio = ratio,
                                .vertical = is_vert,
                                .error = error,
                            },
                    });

    ui->last_response = r;
    return r;
}
