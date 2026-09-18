/*
 * view_mica.c — fullscreen Windows 11 Mica & Mica Alt showcase:
 * wallpaper-anchored foundation material with soft blur, theme tinting,
 * mineral dither, and inactive fallback state.
 */

#include "gallery.h"

void gallery_view_mica_prepare(gallery_app *app, flux_frame *frame, flux_image *blurred,
                               const gallery_layout *l) {
    prism_mica_group group = PRISM_MICA_GROUP_INIT;
    group.shape = (prism_mica_shape){
        .bounds = {l->margin_x, l->header_h, l->avail_w, l->avail_h},
        .corner_radius = 24.0f * l->scale,
    };
    group.shadow_alpha = 0.30f;
    group.shadow_blur = 24.0f * l->scale;
    group.shadow_offset_y = 10.0f * l->scale;

    prism_mica_desc desc = PRISM_MICA_DESC_INIT;
    desc.wallpaper = app->capture;
    desc.blurred_wallpaper = blurred;
    desc.groups = &group;
    desc.group_count = 1;
    desc.kind = PRISM_MICA_BASE;
    desc.screen_width = 0.0f;
    desc.screen_height = 0.0f;
    desc.luminosity_plate = l->polarity;
    desc.tint_color = app->dark_mode ? 0x1E2B3E : 0xEFF3F8;
    desc.fallback_color = app->dark_mode ? 0x202020 : 0xF3F3F3;
    desc.fallback_weight = app->inactive_fallback ? 1.0f : 0.0f;
    (void)prism_mica_filter_apply(app->mica_filter, frame, &desc, &app->mica_out);
}
