/*
 * view_mica.c — fullscreen Windows 11 Mica & Mica Alt showcase:
 * wallpaper-anchored foundation material with soft blur, theme tinting,
 * mineral dither, and inactive fallback state.
 */

#include "gallery.h"

void gallery_view_mica_prepare(gallery_app *app, optics_gfx_frame *frame, optics_gfx_image *blurred,
                               const gallery_layout *l) {
    optics_gfx_rect bounds = {l->margin_x, l->header_h, l->avail_w, l->avail_h};
    float radius = 24.0f * l->scale;
    optics_material_mica_kind kind =
        app->mica_alt ? OPTICS_MATERIAL_MICA_ALT : OPTICS_MATERIAL_MICA_BASE;
    gallery_render_mica(app, frame, blurred, bounds, radius, kind, l->polarity,
                        app->inactive_fallback);
}
