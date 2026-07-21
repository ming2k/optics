/* radio.c — mutually-exclusive toggle (ADR-0031). */

#include "../internal.h"
#include <math.h>

bool lens_radio(lens *ui, const char *label, int *value, int option_value) {
    const lens_theme *t = &ui->theme;
    bool disabled = ui->next_disabled;
    ui->next_disabled = false;
    ui->next_error = false; /* drain so it never leaks to a later widget */
    lens_id id = lensi_gen_widget_id(ui, label);
    lens_node *n = lensi_store_touch(ui, id);
    if (!n)
        return false;
    lensi_link_child(ui, n);
    n->is_container = false;

    lens_text_metrics tm = lensi_text_measure_label(ui, label, t->font_size, 0.0f);
    float circle = roundf(t->font_size);
    float line_h = tm.height > circle ? tm.height : circle;
    float h = line_h + 2.0f * t->padding;
    float w = 2.0f * t->padding + circle + (tm.width > 0 ? 6.0f + tm.width : 0.0f);
    if (n->fixed_w > 0)
        w = n->fixed_w;
    if (n->fixed_h > 0)
        h = n->fixed_h;
    n->measured = (flux_point){w, h};

    lens_response r = lensi_interact(ui, n, true, disabled);
    if (r.clicked && value) {
        *value = option_value;
        r.changed = true;
    }
    if (r.focused && !disabled) {
        for (uint32_t i = 0; i < ui->input.key_count; i++) {
            int k = ui->input.keys[i].key;
            if (ui->input.keys[i].pressed && (k == LENS_KEY_RETURN || k == ' ')) {
                if (value) {
                    *value = option_value;
                    r.changed = true;
                }
            }
        }
    }
    bool on = value && *value == option_value;

    uint32_t sem_flags = (on ? LENS_A11Y_CHECKED : 0) | (r.focused ? LENS_A11Y_FOCUSED : 0) |
                         (disabled ? LENS_A11Y_DISABLED : 0);
    lensi_node_semantics(ui, n, LENS_ROLE_RADIO, label, NULL, sem_flags);

    float dt = ui->input.dt_seconds;
    if (!disabled)
        n->hover_t = lensi_approach(ui, n->hover_t, r.hovered ? 1.f : 0.f, dt, 12.f);

    float circle_y = roundf((h - circle) * 0.5f);

    /* circle background */
    lensi_drawlist_push(ui, n,
                        (lens_draw_cmd){.kind = LENS_DRAW_RECT,
                                        .rel = {t->padding, circle_y, circle, circle},
                                        .color = t->color_bg,
                                        .radius = circle * 0.5f});

    /* inner dot when selected */
    if (on) {
        float dot_pad = circle * 0.25f;
        lensi_drawlist_push(
            ui, n,
            (lens_draw_cmd){.kind = LENS_DRAW_RECT,
                            .rel = {t->padding + dot_pad, circle_y + dot_pad,
                                    circle - 2.0f * dot_pad, circle - 2.0f * dot_pad},
                            .color = disabled ? t->color_disabled : t->color_accent,
                            .radius = (circle - 2.0f * dot_pad) * 0.5f});
    }

    /* circle border — same treatment as the checkbox: color_border alone is
     * too subtle at this size on dark cards; hover emphasizes with accent
     * instead of going darker toward color_hover. */
    flux_color idle_border = lensi_lerp_color(t->color_border, t->color_fg, 0.35f);
    flux_color circle_border =
        disabled ? t->color_disabled
                 : (on ? t->color_accent
                       : lensi_lerp_color(idle_border, t->color_accent, n->hover_t));
    lensi_drawlist_push(ui, n,
                        (lens_draw_cmd){.kind = LENS_DRAW_BORDER,
                                        .rel = {t->padding, circle_y, circle, circle},
                                        .color = circle_border,
                                        .radius = circle * 0.5f,
                                        .width = t->border_width});

    /* label */
    lensi_drawlist_push(ui, n,
                        (lens_draw_cmd){.kind = LENS_DRAW_TEXT,
                                        .rel = {t->padding + circle + 6.0f, t->padding, 0, 0},
                                        .color = t->color_fg,
                                        .text = label,
                                        .text_size = t->font_size});

    ui->last_response = r;
    return r.changed;
}

lens_response lens_radio_ex(lens *ui, lens_radio_opts o) {
    lensi_apply_box(ui, o.box);
    bool scoped = o.box.id && o.box.id[0];
    if (scoped)
        lens_push_id(ui, o.box.id);
    lens_radio(ui, o.label ? o.label : "", o.value, o.option_value);
    if (scoped)
        lens_pop_id(ui);
    if (o.box.tooltip)
        lensi_tooltip(ui, o.box.tooltip);
    return ui->last_response;
}
