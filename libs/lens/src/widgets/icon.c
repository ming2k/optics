#include "../internal.h"
#include <stdio.h>

LENS_API void lens_icon(lens *ui, lens_icon_id id, float size) {
    if ((unsigned)id >= LENS_ICON_COUNT)
        return;
    const lens_theme *t = &ui->theme;
    ui->next_disabled = false;
    ui->next_error = false;
    lens_id nid = lensi_gen_widget_id(ui, "");
    lens_node *n = lensi_store_touch(ui, nid);
    if (!n)
        return;
    lensi_link_child(ui, n);
    n->is_container = false;

    float s = size > 0 ? size : t->font_size;
    if (n->fixed_w > 0)
        s = n->fixed_w;
    if (n->fixed_h > 0 && n->fixed_h < s)
        s = n->fixed_h;
    n->measured = (flux_point){s, s};

    lensi_drawlist_push(ui, n,
                        (lens_draw_cmd){
                            .kind = LENS_DRAW_ICON,
                            .rel = {0, 0, s, s},
                            .color = t->color_fg,
                            .width = 2.0f * (s / 24.0f),
                            .icon_id = id,
                        });
}

/* Flat ("ghost") icon button — the activity-bar / toolbar idiom. Transparent
 * at rest, a subtle fill on hover. The active state is layered:
 *   - a steady background tint (always, the universal "this is selected"
 *     signal), PLUS
 *   - an optional accent-coloured indicator treatment controlled by
 *     `theme.active_indicator_width`: when > 0 a left accent bar of that
 *     width is drawn and the glyph takes the accent colour; when 0 the
 *     indicator is suppressed and the glyph stays foreground, leaving a
 *     calm tint-only active state.
 * No filled accent pill or rounded shape, so a column of these reads as a
 * flat icon strip (VS Code activity bar) rather than a stack of buttons. */
static bool icon_button_impl(lens *ui, lens_icon_id id, bool active) {
    if ((unsigned)id >= LENS_ICON_COUNT)
        return false;
    const lens_theme *t = &ui->theme;
    bool disabled = ui->next_disabled;
    ui->next_disabled = false;
    ui->next_error = false;

    char label[32];
    snprintf(label, sizeof(label), "##icon%d", (int)id);
    lens_id nid = lensi_gen_widget_id(ui, label);
    lens_node *n = lensi_store_touch(ui, nid);
    if (!n)
        return false;
    lensi_link_child(ui, n);
    n->is_container = false;

    float icon_size = t->font_size;
    float pad = t->padding;
    float w = icon_size + 2.0f * pad;
    float h = icon_size + 2.0f * pad;
    if (n->fixed_w > 0)
        w = n->fixed_w;
    if (n->fixed_h > 0)
        h = n->fixed_h;
    n->measured = (flux_point){w, h};

    float max_s = w < h ? w : h;
    max_s -= 2.0f * pad;
    if (max_s < 1.0f)
        max_s = 1.0f;
    /* Keep glyph size independent from a larger square hit target. */
    float s = t->font_size * 1.55f;
    if (s > max_s)
        s = max_s;
    if (s < 1.0f)
        s = 1.0f;

    lens_response r = lensi_interact(ui, n, true, disabled);
    uint32_t sem_flags = (r.focused ? LENS_A11Y_FOCUSED : 0) | (disabled ? LENS_A11Y_DISABLED : 0) |
                         (active ? LENS_A11Y_CHECKED : 0);
    lensi_node_semantics(ui, n, LENS_ROLE_BUTTON, label, NULL, sem_flags);

    if (!disabled)
        n->hover_t = r.hovered ? 1.f : 0.f;

    /* Background tint: transparent at rest, hover tint when hovered, and a
     * steady slightly-stronger tint when active. This is always drawn the
     * same way — it's the baseline active signal independent of the
     * indicator treatment below. */
    float fill = active ? 1.0f : n->hover_t * 0.6f;
    if (fill > 0.001f) {
        flux_color bg = lensi_lerp_color(t->color_bg, t->color_hover, fill);
        lensi_drawlist_push(
            ui, n,
            (lens_draw_cmd){
                .kind = LENS_DRAW_RECT, .rel = {0, 0, 0, 0}, .color = bg, .radius = 0.0f});
    }

    /* Optional accent indicator (left bar + accent glyph) for the active
     * item, theme-tunable via `active_indicator_width`. Width 0 suppresses
     * both, leaving only the background tint. */
    float indicator_w = active ? t->active_indicator_width : 0.0f;
    if (indicator_w > 0.0f)
        lensi_drawlist_push(ui, n,
                            (lens_draw_cmd){.kind = LENS_DRAW_RECT,
                                            .rel = {0, 0, indicator_w, 0},
                                            .color = t->color_accent,
                                            .radius = 0.0f});

    float icon_x = (w - s) * 0.5f;
    float icon_y = (h - s) * 0.5f;

    /* Glyph: dimmed when disabled, accent only when the indicator treatment
     * is active, plain foreground otherwise. */
    flux_color glyph = disabled             ? t->color_disabled
                       : indicator_w > 0.0f ? t->color_accent
                                            : t->color_fg;
    lensi_drawlist_push(ui, n,
                        (lens_draw_cmd){
                            .kind = LENS_DRAW_ICON,
                            .rel = {icon_x, icon_y, s, s},
                            .color = glyph,
                            .width = 1.75f * (s / 24.0f),
                            .icon_id = id,
                        });

    ui->last_response = r;
    return r.clicked;
}

LENS_API bool lens_icon_button(lens *ui, lens_icon_id id) {
    return icon_button_impl(ui, id, false);
}

LENS_API bool lens_icon_button_active(lens *ui, lens_icon_id id, bool active) {
    return icon_button_impl(ui, id, active);
}
