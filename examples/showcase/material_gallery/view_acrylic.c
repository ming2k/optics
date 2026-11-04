/*
 * view_acrylic.c — fullscreen Acrylic showcase:
 * Windows Fluent-style dual-Kawase blur, luminance plate balancing,
 * SDF border highlight, and procedural noise grain.
 */

#include "gallery.h"

void gallery_view_acrylic_prepare(gallery_app *app, flux_frame *frame, flux_image *blurred,
                                  const gallery_layout *l) {
    flux_rect bounds = {l->margin_x, l->header_h, l->avail_w, l->avail_h};
    float radius = 24.0f * l->scale;
    lens_material_recipe r = lens_material_recipe_default(LENS_MATERIAL_ACRYLIC, app->dark_mode);
    float noise = app->acrylic_grain ? r.noise_intensity : 0.0f;
    gallery_render_acrylic(app, frame, blurred, bounds, radius, l->polarity, noise,
                           1.5f * l->scale);
}
