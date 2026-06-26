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
    if (!disabled)
        n->hover_t = lensi_approach(ui, n->hover_t, r.hovered ? 1.f : 0.f, dt, 12.f);

    /* track geometry from last frame's width (one-frame latency, ADR-0006) */
    flux_rect rect = n->has_prev ? n->prev_rect : (flux_rect){0, 0, w, h};
    float track_x0 = t->padding;
    float track_w = rect.w - 2.0f * t->padding - LENS_SLIDER_KNOB;
    if (track_w < 1.0f)
        track_w = 1.0f;

    float span = (max > min) ? (max - min) : 1.0f;
    if (!disabled && r.pressed && value) {
        float local = ui->input.cursor.x - rect.x - track_x0 - LENS_SLIDER_KNOB * 0.5f;
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
    flux_color knob_color = disabled
                                ? t->color_disabled
                                : lensi_lerp_color(t->color_accent, t->color_active, n->hover_t);

    /* track */
    lensi_drawlist_push(ui, n,
                        (lens_draw_cmd){.kind = LENS_DRAW_RECT,
                                        .rel = {track_x0, h * 0.5f - 3.0f, 0, 6.0f},
                                        .color = track_color,
                                        .radius = 3.0f});

    /* filled portion — extend to the knob's right edge so the track
     * never shows through the gap between fill and knob. */
    lensi_drawlist_push(
        ui, n,
        (lens_draw_cmd){.kind = LENS_DRAW_RECT,
                        .rel = {track_x0, h * 0.5f - 3.0f, frac * track_w + LENS_SLIDER_KNOB, 6.0f},
                        .color = fill_color,
                        .radius = 3.0f});

    /* knob */
    lensi_drawlist_push(
        ui, n,
        (lens_draw_cmd){.kind = LENS_DRAW_RECT,
                        .rel = {track_x0 + frac * track_w, h * 0.5f - LENS_SLIDER_KNOB * 0.5f,
                                LENS_SLIDER_KNOB, LENS_SLIDER_KNOB},
                        .color = knob_color,
                        .radius = LENS_SLIDER_KNOB * 0.5f});

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
