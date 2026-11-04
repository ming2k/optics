/*
 * view_mica.c — fullscreen Windows 11 Mica & Mica Alt showcase:
 * wallpaper-anchored foundation material with soft blur, theme tinting,
 * mineral dither, and inactive fallback state.
 */

#include "gallery.h"

void gallery_view_mica_prepare(gallery_app *app, flux_frame *frame, flux_image *blurred,
                               const gallery_layout *l) {
    flux_rect bounds = {l->margin_x, l->header_h, l->avail_w, l->avail_h};
    float radius = 24.0f * l->scale;
    prism_mica_kind kind = app->mica_alt ? PRISM_MICA_ALT : PRISM_MICA_BASE;
    gallery_render_mica(app, frame, blurred, bounds, radius, kind, l->polarity,
                        app->inactive_fallback);
}
