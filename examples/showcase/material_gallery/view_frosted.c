/*
 * view_frosted.c — fullscreen Frosted Glass showcase:
 * macOS-style vibrancy dual-Kawase blur and saturation boost.
 */

#include "gallery.h"

void gallery_view_frosted_prepare(gallery_app *app, flux_frame *frame, flux_image *blurred,
                                  const gallery_layout *l) {
    (void)app;
    lens_material_recipe r = lens_material_recipe_default(LENS_MATERIAL_FROSTED, app->dark_mode);
    prism_frosted_group group = PRISM_FROSTED_GROUP_INIT;
    group.shape = (prism_frosted_shape){
        .bounds = {l->margin_x, l->header_h, l->avail_w, l->avail_h},
        .corner_radius = 24.0f * l->scale,
    };
    group.tint_color = r.tint & 0x00FFFFFFu;
    group.shadow_alpha = r.shadow_alpha;
    group.shadow_blur = r.shadow_blur * l->scale;
    group.shadow_offset_y = 10.0f * l->scale;

    prism_frosted_desc desc = PRISM_FROSTED_DESC_INIT;
    desc.input = app->capture;
    desc.blurred_input = blurred;
    desc.groups = &group;
    desc.group_count = 1;
    desc.saturation = app->frost_vibrancy ? 1.45f : 1.0f;
    desc.tint_strength = r.tint_opacity;
    (void)prism_frosted_filter_apply(app->frosted_filter, frame, &desc, &app->frost_out);
}
