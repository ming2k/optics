#include "../internal.h"

/* Raster image widget — the texture-backed sibling of lens_icon. The host
 * owns the flux_image (typically a cached, decoded application icon) and
 * borrows it for the frame; it must remain valid until lens_render. `w`/`h`
 * are the desired logical size; the image is scaled to fill the measured
 * box. A zero dimension adopts the other to keep the box square; both zero
 * falls back to the theme font size. Identity is stack-derived like
 * lens_icon, so repeated calls in a loop (dock tiles, launcher rows) each
 * resolve to a distinct node. */
static void image_impl(lens *ui, flux_image *image, float w, float h, flux_color tint,
                       lens_foreground_outline outline) {
    ui->next_disabled = false;
    ui->next_error = false;
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
        w = ui->theme.font_size;
        h = w;
    }
    if (n->fixed_w > 0.0f)
        w = n->fixed_w;
    if (n->fixed_h > 0.0f)
        h = n->fixed_h;
    n->measured = (flux_point){w, h};

    lensi_drawlist_push(ui, n,
                        (lens_draw_cmd){
                            .kind = LENS_DRAW_IMAGE,
                            .rel = {0, 0, w, h},
                            .color = tint,
                            .outline_color = outline.color,
                            .outline_width = outline.width > 0.0f ? outline.width : 0.0f,
                            .image = image,
                        });
}

LENS_API void lens_image(lens *ui, flux_image *image, float w, float h) {
    image_impl(ui, image, w, h, flux_color_rgba_premul(255, 255, 255, 255),
               (lens_foreground_outline){0});
}

LENS_API void lens_image_tinted(lens *ui, flux_image *image, float w, float h, flux_color tint) {
    image_impl(ui, image, w, h, tint, (lens_foreground_outline){0});
}

LENS_API void lens_image_tinted_outlined(lens *ui, flux_image *image, float w, float h,
                                         flux_color tint, lens_foreground_outline outline) {
    image_impl(ui, image, w, h, tint, outline);
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

    char label[40];
    snprintf(label, sizeof(label), "##img%lx", (unsigned long)(uintptr_t)image);
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
    if (n->fixed_w > 0.0f)
        w = n->fixed_w;
    if (n->fixed_h > 0.0f)
        h = n->fixed_h;
    n->measured = (flux_point){w, h};

    float s = (w < h ? w : h) - 2.0f * pad;
    if (s < 1.0f)
        s = 1.0f;

    lens_response r = lensi_interact(ui, n, true, disabled);
    uint32_t sem_flags = (r.focused ? LENS_A11Y_FOCUSED : 0) | (disabled ? LENS_A11Y_DISABLED : 0) |
                         (active ? LENS_A11Y_CHECKED : 0);
    lensi_node_semantics(ui, n, LENS_ROLE_BUTTON, label, NULL, sem_flags);

    float dt = ui->input.dt_seconds;
    if (!disabled)
        n->hover_t = lensi_approach(ui, n->hover_t, r.hovered ? 1.f : 0.f, dt, 12.f);

    float fill = active ? 1.0f : n->hover_t * 0.6f;
    /* A blank image button still needs the standard surface. For a real
     * image, draw interaction feedback after the texture instead; otherwise
     * the opaque image hides the hover/active treatment completely. */
    if (!image && fill > 0.001f) {
        flux_color bg = lensi_lerp_color(t->color_bg, t->color_hover, fill);
        lensi_drawlist_push(
            ui, n,
            (lens_draw_cmd){
                .kind = LENS_DRAW_RECT, .rel = {0, 0, 0, 0}, .color = bg, .radius = 0.0f});
    }

    float indicator_w = active ? t->active_indicator_width : 0.0f;
    if (indicator_w > 0.0f)
        lensi_drawlist_push(ui, n,
                            (lens_draw_cmd){.kind = LENS_DRAW_RECT,
                                            .rel = {0, 0, indicator_w, 0},
                                            .color = t->color_accent,
                                            .radius = 0.0f});

    if (image) {
        float ix = (w - s) * 0.5f;
        float iy = (h - s) * 0.5f;
        lensi_drawlist_push(ui, n,
                            (lens_draw_cmd){.kind = LENS_DRAW_IMAGE,
                                            .rel = {ix, iy, s, s},
                                            .color = flux_color_rgba_premul(255, 255, 255, 255),
                                            .image = image});

        float feedback = active ? 0.65f + n->hover_t * 0.35f : n->hover_t;
        if (feedback > 0.001f) {
            /* A restrained dark veil remains visible over both light and
             * dark photography. The accent outline carries selected/hover
             * state without obscuring the artwork. */
            float veil = active ? 18.0f + n->hover_t * 34.0f : n->hover_t * 52.0f;
            uint8_t veil_alpha = (uint8_t)veil;
            lensi_drawlist_push(ui, n,
                                (lens_draw_cmd){.kind = LENS_DRAW_RECT,
                                                .rel = {ix, iy, s, s},
                                                .color = flux_color_rgba(0, 0, 0, veil_alpha),
                                                .radius = t->corner_radius});
            lensi_drawlist_push(
                ui, n,
                (lens_draw_cmd){
                    .kind = LENS_DRAW_BORDER,
                    .rel = {ix, iy, s, s},
                    .color = lensi_lerp_color(t->color_border, t->color_accent, feedback * 0.75f),
                    .radius = t->corner_radius,
                    .width = t->border_width > 1.5f ? t->border_width : 1.5f});
        }
    }

    ui->last_response = r;
    return r.clicked;
}

LENS_API bool lens_image_button(lens *ui, flux_image *image) {
    return image_button_impl(ui, image, false);
}
LENS_API bool lens_image_button_active(lens *ui, flux_image *image, bool active) {
    return image_button_impl(ui, image, active);
}
