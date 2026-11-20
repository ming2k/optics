/*
 * view_acrylic.c — fullscreen Acrylic showcase:
 * Windows Fluent-style dual-Kawase blur, luminance plate balancing,
 * SDF border highlight, and procedural noise grain.
 */

#include "gallery.h"

void gallery_view_acrylic_prepare(gallery_app *app, optics_gfx_frame *frame,
                                  optics_gfx_image *blurred, const gallery_layout *l) {
    optics_gfx_rect bounds = {l->margin_x, l->header_h, l->avail_w, l->avail_h};
    float radius = 24.0f * l->scale;
    optics_ui_material_recipe r =
        optics_ui_material_recipe_default(OPTICS_UI_MATERIAL_ACRYLIC, app->dark_mode);
    float noise = app->acrylic_grain ? r.noise_intensity : 0.0f;
    gallery_render_acrylic(app, frame, blurred, bounds, radius, l->polarity, noise,
                           1.5f * l->scale);
}
