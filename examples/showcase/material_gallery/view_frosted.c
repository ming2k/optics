/*
 * view_frosted.c — fullscreen Frosted Glass showcase:
 * macOS-style vibrancy dual-Kawase blur and saturation boost.
 */

#include "gallery.h"

void gallery_view_frosted_prepare(gallery_app *app, flux_frame *frame, flux_image *blurred,
                                  const gallery_layout *l) {
    prism_frosted_group group = PRISM_FROSTED_GROUP_INIT;
    group.shape = (prism_frosted_shape){
        .bounds = {l->margin_x, l->header_h, l->avail_w, l->avail_h},
        .corner_radius = 24.0f * l->scale,
    };
    group.tint_color = app->dark_mode ? 0x1A2540 : 0xEAF2FF;
    group.shadow_alpha = 0.30f;
    group.shadow_blur = 24.0f * l->scale;
    group.shadow_offset_y = 10.0f * l->scale;

    prism_frosted_desc desc = PRISM_FROSTED_DESC_INIT;
    desc.input = app->capture;
    desc.blurred_input = blurred;
    desc.groups = &group;
    desc.group_count = 1;
    desc.saturation = 1.45f;
    desc.tint_strength = 0.20f;
    (void)prism_frosted_filter_apply(app->frosted_filter, frame, &desc, &app->frost_out);
}
