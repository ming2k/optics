/* skin/button.c — default button skin (ADR-0059).
 * Handles default, primary, subtle, and link variants, with text, icon, and image content. */

#include "../internal.h"
#include <math.h>

static float lensi_channel_luminance(float c) {
    return c <= 0.04045f ? c / 12.92f : powf((c + 0.055f) / 1.055f, 2.4f);
}

static float lensi_color_luminance(flux_color col) {
    uint8_t r, g, b, a;
    flux_color_unpack(col, &r, &g, &b, &a);
    if (a == 0)
        return 0.0f;
    float fa = (float)a / 255.0f;
    float fr = (float)r / 255.0f / fa;
    float fg = (float)g / 255.0f / fa;
    float fb = (float)b / 255.0f / fa;
    return 0.2126f * lensi_channel_luminance(fr) + 0.7152f * lensi_channel_luminance(fg) +
           0.0722f * lensi_channel_luminance(fb);
}

void lensi_skin_button(lens *ui, lens_node *n, const lens_widget_record *rec) {
    const lens_style_resolved *rs = &rec->style;
    bool disabled = (rec->state & LENS_STATE_DISABLED) != 0;
    bool active = (rec->state & LENS_STATE_ACTIVE) != 0;
    float w = rec->bounds.w;
    float h = rec->bounds.h;

    /* Variant: LINK */
    if (rec->content.variant == LENS_BUTTON_LINK) {
        float tm_h = rec->content.text.height;
        float text_y = fmaxf((h - tm_h) * 0.5f - 1.0f, 0.0f);
        flux_color fg = disabled ? rs->disabled : (rec->hover_t > 0.001f ? rs->accent : rs->fg);

        if (rec->content.label && rec->content.label[0]) {
            lensi_drawlist_push(ui, n,
                                (lens_draw_cmd){
                                    .kind = LENS_DRAW_TEXT,
                                    .rel = {rs->padding, text_y, 0, 0},
                                    .color = fg,
                                    .text = rec->content.label,
                                    .text_size = rs->font_size,
                                });
            if (rec->hover_t > 0.001f && !disabled) {
                flux_color line_col = lensi_lerp_color(0, rs->accent, rec->hover_t);
                lensi_drawlist_push(
                    ui, n,
                    (lens_draw_cmd){
                        .kind = LENS_DRAW_RECT,
                        .rel = {rs->padding, text_y + tm_h + 1.0f, rec->content.text.width, 1.0f},
                        .color = line_col,
                        .radius = 0.0f,
                    });
            }
        }
        return;
    }

    /* Variant: SUBTLE, PRIMARY, DEFAULT background resolution */
    flux_color bg;
    if (disabled) {
        bg = rs->disabled;
    } else if (rec->content.variant == LENS_BUTTON_PRIMARY) {
        bg = active
                 ? rs->bg_pressed
                 : (rec->hover_t > 0.001f ? lensi_lerp_color(rs->accent, rs->bg_hover, rec->hover_t)
                                          : rs->accent);
    } else if (rec->content.variant == LENS_BUTTON_SUBTLE) {
        bg = active ? rs->bg_pressed : lensi_lerp_color(0, rs->bg_hover, rec->hover_t);
    } else {
        bg = active ? rs->bg_pressed : lensi_lerp_color(rs->bg, rs->bg_hover, rec->hover_t);
    }

    /* Background and border */
    if ((bg & 0xFF000000u) != 0) {
        lensi_drawlist_push(ui, n,
                            (lens_draw_cmd){
                                .kind = LENS_DRAW_RECT,
                                .rel = {0, 0, w, h},
                                .color = bg,
                                .radius = rs->corner_radius,
                            });
    }

    if (rs->border_width > 0.0f && (rs->border & 0xFF000000u) != 0 &&
        rec->content.variant != LENS_BUTTON_SUBTLE) {
        lensi_drawlist_push(ui, n,
                            (lens_draw_cmd){
                                .kind = LENS_DRAW_BORDER,
                                .rel = {0, 0, w, h},
                                .color = rs->border,
                                .width = rs->border_width,
                                .radius = rs->corner_radius,
                            });
    }

    /* Content layout & drawing */
    float glyph_s = rec->content.glyph_size > 0.0f ? rec->content.glyph_size : rs->font_size;
    bool has_icon = rec->content.icon != LENS_ICON_INVALID && rec->content.icon != 0;
    bool has_image = rec->content.image != NULL;
    bool has_label = rec->content.label && rec->content.label[0];

    flux_color text_col;
    if (disabled) {
        text_col = rs->disabled;
    } else if (rec->content.variant == LENS_BUTTON_PRIMARY) {
        float lum = lensi_color_luminance(bg);
        text_col = lum > 0.5f ? flux_color_rgba_premul(0, 0, 0, 255)
                              : flux_color_rgba_premul(255, 255, 255, 255);
    } else {
        text_col = rs->fg;
    }

    if (has_image) {
        float is = fminf(glyph_s, fminf(w, h) - 2.0f * rs->padding);
        float ix = (w - is) * 0.5f;
        float iy = (h - is) * 0.5f;
        lensi_drawlist_push(ui, n,
                            (lens_draw_cmd){
                                .kind = LENS_DRAW_IMAGE,
                                .rel = {ix, iy, is, is},
                                .color = flux_color_rgba_premul(255, 255, 255, 255),
                                .image = rec->content.image,
                            });
    } else if (has_icon && !has_label) {
        float ix = (w - glyph_s) * 0.5f;
        float iy = (h - glyph_s) * 0.5f;
        lensi_drawlist_push(ui, n,
                            (lens_draw_cmd){
                                .kind = LENS_DRAW_ICON,
                                .rel = {ix, iy, glyph_s, glyph_s},
                                .color = text_col,
                                .width = 2.0f * (glyph_s / 24.0f),
                                .icon_id = rec->content.icon,
                            });
    } else if (has_label) {
        float text_y = fmaxf((h - rec->content.text.height) * 0.5f - 1.0f, 0.0f);
        float text_x = rs->padding;
        if (has_icon) {
            float ix = rs->padding;
            float iy = (h - glyph_s) * 0.5f;
            lensi_drawlist_push(ui, n,
                                (lens_draw_cmd){
                                    .kind = LENS_DRAW_ICON,
                                    .rel = {ix, iy, glyph_s, glyph_s},
                                    .color = text_col,
                                    .width = 2.0f * (glyph_s / 24.0f),
                                    .icon_id = rec->content.icon,
                                });
            text_x += glyph_s + 6.0f;
        } else {
            text_x = fmaxf((w - rec->content.text.width) * 0.5f, rs->padding);
        }

        lensi_drawlist_push(ui, n,
                            (lens_draw_cmd){
                                .kind = LENS_DRAW_TEXT,
                                .rel = {text_x, text_y, 0, 0},
                                .color = text_col,
                                .text = rec->content.label,
                                .text_size = rs->font_size,
                            });
    }
}
