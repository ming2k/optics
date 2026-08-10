#include "../internal.h"
#include <stdio.h>

static void icon_impl(lens *ui, lens_icon_id id, float size) {
    if (!lensi_icon_valid((int32_t)id))
        return;
    const lens_theme *t = &ui->theme;
    ui->next_disabled = false;
    ui->next_error = false;
    lens_style eff = lensi_style_effective(ui);
    /* Bare icons carry no label, so hash (scope, kind, sibling sequence)
     * like containers and spacers: two lens_icon calls in one scope must not
     * share a node — with a label-less widget id both glyphs' draw commands
     * would pile onto the first icon's slot while the second slot stays
     * empty. */
    lens_id nid = lensi_gen_container_id(ui, "icon");
    lens_node *n = lensi_store_touch(ui, nid);
    if (!n)
        return;
    lensi_link_child(ui, n);
    n->is_container = false;

    /* `size` is the glyph size. A pending size_next only reserves the layout
     * box — it must not silently rescale the glyph; the glyph is centered
     * inside the box instead. */
    float glyph = size > 0 ? size : lensi_style_font_size(&eff, t);
    float bw = n->fixed_w > 0 ? n->fixed_w : glyph;
    float bh = n->fixed_h > 0 ? n->fixed_h : glyph;
    n->measured = (flux_point){bw, bh};
    float s = fminf(glyph, fminf(bw, bh));

    /* Non-interactive: resolve with an empty state. The outline atoms make
     * the old *_outlined variant reachable through any box.style or scope
     * (ADR-0061 item 6). Emission is the skin's (ADR-0059). */
    lens_style_resolved rs = lensi_style_resolve(&eff, t, 0);
    (void)s; /* glyph clamping to the box happens in the skin */
    lensi_skin_emit(ui, n,
                    &(lens_widget_record){
                        .kind = LENS_WIDGET_ICON,
                        .state = 0,
                        .bounds = {0, 0, bw, bh},
                        .last_bounds = n->prev_rect,
                        .style = rs,
                        .style_fields = eff.fields,
                        .content = {.icon = id, .glyph_size = glyph},
                    });
}

LENS_API void lens_icon(lens *ui, lens_icon_id id, float size) {
    icon_impl(ui, id, size);
}

/* Flat ("ghost") icon button for navigation strips and toolbars. Transparent
 * at rest, with a subtle fill on hover. The active state is a steady neutral
 * tint (the cascade-resolved bg_pressed — theme: color_active) with a plain
 * foreground glyph: state as data, no flavor (ADR-0061 item 7). An accent
 * rail or accent glyph is a style-atom / custom-skin decision the caller
 * owns, not a separate API. */
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
    if (!lensi_icon_valid((int32_t)id))
        return false;
    const lens_theme *t = &ui->theme;
    bool disabled = ui->next_disabled;
    ui->next_disabled = false;
    ui->next_error = false;
    lens_style eff = lensi_style_effective(ui);

    lens_id nid = lensi_gen_widget_id(ui, spec.identity);
    lens_node *n = lensi_store_touch(ui, nid);
    if (!n)
        return false;
    lensi_link_child(ui, n);
    n->is_container = false;

    float font_size = lensi_style_font_size(&eff, t);
    float padding = lensi_style_padding(&eff, t);
    float icon_size = font_size;
    float w = icon_size + 2.0f * padding;
    float h = icon_size + 2.0f * padding;
    if (n->fixed_w > 0)
        w = n->fixed_w;
    if (n->fixed_h > 0)
        h = n->fixed_h;
    n->measured = (flux_point){w, h};

    lens_response r = lensi_interact(ui, n, true, disabled);
    if (spec.active_surface || spec.checked)
        r.state |= LENS_STATE_ACTIVE; /* steady on-state (ADR-0058) */
    uint32_t sem_flags = (r.focused ? LENS_A11Y_FOCUSED : 0) | (disabled ? LENS_A11Y_DISABLED : 0) |
                         (spec.checked ? LENS_A11Y_CHECKED : 0);
    lensi_node_semantics(ui, n, LENS_ROLE_BUTTON, spec.identity, NULL, sem_flags);

    if (!disabled)
        n->hover_t = r.hovered ? 1.f : 0.f;

    /* resolve + emit — through the replaceable skin (ADR-0059); the glyph
     * sizing and badge geometry live in the skin now. */
    lensi_skin_emit(ui, n,
                    &(lens_widget_record){
                        .kind = LENS_WIDGET_ICON_BUTTON,
                        .state = r.state,
                        .bounds = {0, 0, w, h},
                        .last_bounds = n->prev_rect,
                        .style = lensi_style_resolve(&eff, t, r.state),
                        .style_fields = eff.fields,
                        .hover_t = n->hover_t,
                        .active_t = n->active_t,
                        .content = {.icon = spec.icon,
                                    .badge = spec.badge,
                                    .glyph_size = spec.requested_size,
                                    .rounded = spec.rounded,
                                    .active_surface = spec.active_surface,
                                    .accent_checked = spec.accent_checked},
                    });

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
    if (!lensi_icon_valid((int32_t)unchecked_icon) || !lensi_icon_valid((int32_t)checked_icon))
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
