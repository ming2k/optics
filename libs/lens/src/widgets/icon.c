#include "../internal.h"
#include <stdio.h>

static void icon_impl(lens *ui, lens_icon_id id, float size, lens_foreground_outline outline) {
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
                            .outline_color = outline.color,
                            .outline_width = outline.width > 0.0f ? outline.width : 0.0f,
                            .width = 2.0f * (s / 24.0f),
                            .icon_id = id,
                        });
}

LENS_API void lens_icon(lens *ui, lens_icon_id id, float size) {
    icon_impl(ui, id, size, (lens_foreground_outline){0});
}

LENS_API void lens_icon_outlined(lens *ui, lens_icon_id id, float size,
                                 lens_foreground_outline outline) {
    icon_impl(ui, id, size, outline);
}

/* Flat ("ghost") icon button for navigation strips and toolbars. Transparent
 * at rest, with a subtle fill on hover. The active state is layered:
 *   - a steady background tint (always, the universal "this is selected"
 *     signal), PLUS
 *   - an optional accent-coloured indicator treatment controlled by
 *     `theme.active_indicator_width`: when > 0 a left accent rail of that
 *     width is drawn and the glyph takes the accent colour. It is disabled by
 *     default; with width 0, the glyph stays foreground and the active state
 *     is tint-only.
 * There is no filled accent pill or rounded shape, so a column reads as a
 * single flat icon strip rather than a stack of buttons. */
typedef struct icon_button_spec {
    lens_icon_id icon;
    const char *identity;
    const char *badge;
    float requested_size;
    bool rounded;
    bool active_surface;
    bool checked;
    bool accent_checked;
} icon_button_spec;

static bool icon_button_impl(lens *ui, icon_button_spec spec) {
    lens_icon_id id = spec.icon;
    if ((unsigned)id >= LENS_ICON_COUNT)
        return false;
    const lens_theme *t = &ui->theme;
    bool disabled = ui->next_disabled;
    ui->next_disabled = false;
    ui->next_error = false;

    lens_id nid = lensi_gen_widget_id(ui, spec.identity);
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
    float s = spec.requested_size > 0.0f ? spec.requested_size : t->font_size * 1.55f;
    if (s > max_s)
        s = max_s;
    if (s < 1.0f)
        s = 1.0f;

    lens_response r = lensi_interact(ui, n, true, disabled);
    uint32_t sem_flags = (r.focused ? LENS_A11Y_FOCUSED : 0) | (disabled ? LENS_A11Y_DISABLED : 0) |
                         (spec.checked ? LENS_A11Y_CHECKED : 0);
    lensi_node_semantics(ui, n, LENS_ROLE_BUTTON, spec.identity, NULL, sem_flags);

    if (!disabled)
        n->hover_t = r.hovered ? 1.f : 0.f;

    /* Background tint: transparent at rest, hover tint when hovered, and a
     * steady slightly-stronger tint when active. This is always drawn the
     * same way — it's the baseline active signal independent of the
     * indicator treatment below. */
    float fill = spec.active_surface ? 1.0f : n->hover_t * 0.6f;
    if (fill > 0.001f) {
        flux_color bg = spec.active_surface && spec.rounded
                            ? t->color_active
                            : lensi_lerp_color(t->color_bg, t->color_hover, fill);
        lensi_drawlist_push(ui, n,
                            (lens_draw_cmd){.kind = LENS_DRAW_RECT,
                                            .rel = {0, 0, 0, 0},
                                            .color = bg,
                                            .radius = spec.rounded ? t->corner_radius : 0.0f});
    }

    /* Optional accent indicator (left bar + accent glyph) for the active
     * item, theme-tunable via `active_indicator_width`. Width 0 suppresses
     * both, leaving only the background tint. */
    float indicator_w = spec.active_surface && !spec.rounded ? t->active_indicator_width : 0.0f;
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
    flux_color glyph = disabled ? t->color_disabled
                       : spec.rounded && (spec.active_surface || spec.accent_checked)
                           ? t->color_accent
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

    if (spec.badge && spec.badge[0]) {
        float badge_size = s * 0.42f;
        if (badge_size < 8.0f)
            badge_size = 8.0f;
        lens_text_metrics bm = lensi_text_measure_label(ui, spec.badge, badge_size, 650.0f);
        float badge_x = icon_x + s - bm.width * 0.62f;
        float badge_y = icon_y - bm.height * 0.30f;
        if (badge_x + bm.width > w)
            badge_x = w - bm.width;
        if (badge_y < 0.0f)
            badge_y = 0.0f;
        lensi_drawlist_push(ui, n,
                            (lens_draw_cmd){.kind = LENS_DRAW_TEXT,
                                            .rel = {badge_x, badge_y, 0, 0},
                                            .color = glyph,
                                            .text = spec.badge,
                                            .text_size = badge_size,
                                            .text_weight = 650.0f});
    }

    ui->last_response = r;
    return r.clicked;
}

LENS_API bool lens_icon_button(lens *ui, lens_icon_id id) {
    char identity[32];
    snprintf(identity, sizeof(identity), "##icon%d", (int)id);
    return icon_button_impl(ui,
                            (icon_button_spec){.icon = id, .identity = identity, .checked = false});
}

LENS_API bool lens_icon_button_active(lens *ui, lens_icon_id id, bool active) {
    char identity[32];
    snprintf(identity, sizeof(identity), "##icon%d", (int)id);
    return icon_button_impl(
        ui, (icon_button_spec){
                .icon = id, .identity = identity, .active_surface = active, .checked = active});
}

LENS_API bool lens_icon_button_badged(lens *ui, lens_icon_id id, const char *badge,
                                      float glyph_size, bool active) {
    char identity[64];
    snprintf(identity, sizeof(identity), "##icon%d%s%s", (int)id, badge && badge[0] ? ":" : "",
             badge && badge[0] ? badge : "");
    return icon_button_impl(ui, (icon_button_spec){.icon = id,
                                                   .identity = identity,
                                                   .badge = badge,
                                                   .requested_size = glyph_size,
                                                   .rounded = true,
                                                   .active_surface = active,
                                                   .checked = active});
}

LENS_API bool lens_icon_toggle_button(lens *ui, lens_icon_id unchecked_icon,
                                      lens_icon_id checked_icon, float glyph_size, bool checked) {
    if ((unsigned)unchecked_icon >= LENS_ICON_COUNT || (unsigned)checked_icon >= LENS_ICON_COUNT)
        return false;
    char identity[48];
    snprintf(identity, sizeof(identity), "##toggle%d:%d", (int)unchecked_icon, (int)checked_icon);
    return icon_button_impl(ui, (icon_button_spec){.icon = checked ? checked_icon : unchecked_icon,
                                                   .identity = identity,
                                                   .requested_size = glyph_size,
                                                   .rounded = true,
                                                   .checked = checked,
                                                   .accent_checked = checked});
}
