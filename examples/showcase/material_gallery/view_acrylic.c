/*
 * view_acrylic.c — fullscreen Acrylic showcase:
 * Windows Fluent-style dual-Kawase blur, luminance plate balancing,
 * SDF border highlight, and procedural noise grain.
 */

#include "gallery.h"

void gallery_view_acrylic_prepare(gallery_app *app, flux_frame *frame, flux_image *blurred,
                                  const gallery_layout *l) {
    (void)app;
    lens_material_recipe r = lens_material_recipe_default(LENS_MATERIAL_ACRYLIC, app->dark_mode);
    prism_acrylic_group group = PRISM_ACRYLIC_GROUP_INIT;
    group.shape = (prism_acrylic_shape){
        .bounds = {l->margin_x, l->header_h, l->avail_w, l->avail_h},
        .corner_radius = 24.0f * l->scale,
    };
    group.tint_color = r.tint & 0x00FFFFFFu;
    group.shadow_alpha = r.shadow_alpha;
    group.shadow_blur = r.shadow_blur * l->scale;
    group.shadow_offset_y = 10.0f * l->scale;

    prism_acrylic_desc desc = PRISM_ACRYLIC_DESC_INIT;
    desc.input = app->capture;
    desc.blurred_input = blurred;
    desc.groups = &group;
    desc.group_count = 1;
    desc.luminance_plate = l->polarity;
    desc.tint_strength = r.tint_opacity;
    desc.noise_intensity = app->acrylic_grain ? r.noise_intensity : 0.0f;
    desc.border_width = 1.5f * l->scale;
    desc.border_alpha = r.border_highlight;
    (void)prism_acrylic_filter_apply(app->acrylic_filter, frame, &desc, &app->acrylic_out);
}
