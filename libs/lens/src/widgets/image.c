#include "../internal.h"

/* Raster image widget — the texture-backed sibling of lens_icon. The host
 * owns the flux_image (typically a cached, decoded application icon) and
 * borrows it for the frame; it must remain valid until lens_render. `w`/`h`
 * are the desired logical size; the image is scaled to fill the measured
 * box. A zero dimension adopts the other to keep the box square; both zero
 * falls back to the resolved font size. Identity is stack-derived like
 * lens_icon, so repeated calls in a loop (dock tiles, launcher rows) each
 * resolve to a distinct node. */
static void image_impl(lens *ui, flux_image *image, float w, float h, flux_color tint) {
    ui->next_disabled = false;
    ui->next_error = false;
    lens_style eff = lensi_style_effective(ui);
    lens_id nid = lensi_gen_widget_id(ui, "");
    lens_node *n = lensi_store_touch(ui, nid);
    if (!n)
        return;
    lensi_link_child(ui, n);
    n->is_container = false;

    if (w <= 0.0f && h > 0.0f)
        w = h;
    if (h <= 0.0f && w > 0.0f)
        h = w;
    if (w <= 0.0f) {
        w = lensi_style_font_size(&eff, &ui->theme);
        h = w;
    }
    if (n->fixed_w > 0.0f)
        w = n->fixed_w;
    if (n->fixed_h > 0.0f)
        h = n->fixed_h;
    n->measured = (flux_point){w, h};

    /* Non-interactive: resolve with an empty state. The outline atoms are
     * the old *_outlined variant's effect, reachable through any box.style
     * or scope (ADR-0061 item 6). Emission is the skin's (ADR-0059). */
    lens_style_resolved rs = lensi_style_resolve(&eff, &ui->theme, 0);
    lensi_skin_emit(ui, n,
                    &(lens_widget_record){
                        .kind = LENS_WIDGET_IMAGE,
                        .state = 0,
                        .bounds = {0, 0, w, h},
                        .last_bounds = n->prev_rect,
                        .style = rs,
                        .style_fields = eff.fields,
                        .content = {.image = image, .tint = tint},
                    });
}

LENS_API void lens_image(lens *ui, flux_image *image, float w, float h) {
    image_impl(ui, image, w, h, flux_color_rgba_premul(255, 255, 255, 255));
}

LENS_API void lens_image_tinted(lens *ui, flux_image *image, float w, float h, flux_color tint) {
    image_impl(ui, image, w, h, tint);
}

/* Texture-backed variant of icon_button_impl: identical hover/active/click
 * behaviour and background treatment, but draws the host-owned raster image
 * where the glyph would be. A NULL image draws the background only (blank
 * tile) so the caller can use the same widget whether or not an icon
 * texture is available. Identity is keyed off the image pointer so distinct
 * tiles in a dock each resolve to a distinct node. */
#include <stdio.h>
static bool image_button_impl(lens *ui, flux_image *image, bool active) {
    const lens_theme *t = &ui->theme;
    bool disabled = ui->next_disabled;
    ui->next_disabled = false;
    ui->next_error = false;
    lens_style eff = lensi_style_effective(ui);

    char label[40];
    snprintf(label, sizeof(label), "##img%lx", (unsigned long)(uintptr_t)image);
    lens_id nid = lensi_gen_widget_id(ui, label);
    lens_node *n = lensi_store_touch(ui, nid);
    if (!n)
        return false;
    lensi_link_child(ui, n);
    n->is_container = false;

    float padding = lensi_style_padding(&eff, t);
    float icon_size = lensi_style_font_size(&eff, t);
    float w = icon_size + 2.0f * padding;
    float h = icon_size + 2.0f * padding;
    if (n->fixed_w > 0.0f)
        w = n->fixed_w;
    if (n->fixed_h > 0.0f)
        h = n->fixed_h;
    n->measured = (flux_point){w, h};

    float s = (w < h ? w : h) - 2.0f * padding;
    if (s < 1.0f)
        s = 1.0f;

    lens_response r = lensi_interact(ui, n, true, disabled);
    if (active)
        r.state |= LENS_STATE_ACTIVE; /* steady on-state (ADR-0058) */
    uint32_t sem_flags = (r.focused ? LENS_A11Y_FOCUSED : 0) | (disabled ? LENS_A11Y_DISABLED : 0) |
                         (active ? LENS_A11Y_CHECKED : 0);
    lensi_node_semantics(ui, n, LENS_ROLE_BUTTON, label, NULL, sem_flags);

    float dt = ui->input.dt_seconds;
    if (!disabled)
        n->hover_t = lensi_approach(ui, n->hover_t, r.hovered ? 1.f : 0.f, dt, 12.f);

    lens_style_resolved rs = lensi_style_resolve(&eff, t, r.state);

    /* emit — through the replaceable skin (ADR-0059) */
    lensi_skin_emit(ui, n,
                    &(lens_widget_record){
                        .kind = LENS_WIDGET_IMAGE,
                        .state = r.state,
                        .bounds = {0, 0, w, h},
                        .last_bounds = n->prev_rect,
                        .style = rs,
                        .style_fields = eff.fields,
                        .hover_t = n->hover_t,
                        .active_t = n->active_t,
                        .content = {.image = image,
                                    .tint = flux_color_rgba_premul(255, 255, 255, 255),
                                    .image_button = true},
                    });

    ui->last_response = r;
    return r.clicked;
}

LENS_API bool lens_image_button(lens *ui, flux_image *image) {
    return image_button_impl(ui, image, false);
}
LENS_API bool lens_image_button_active(lens *ui, flux_image *image, bool active) {
    return image_button_impl(ui, image, active);
}
