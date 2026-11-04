/*
 * view_frosted.c — fullscreen Frosted Glass showcase:
 * macOS-style vibrancy dual-Kawase blur and saturation boost.
 */

#include "gallery.h"

void gallery_view_frosted_prepare(gallery_app *app, flux_frame *frame, flux_image *blurred,
                                  const gallery_layout *l) {
    flux_rect bounds = {l->margin_x, l->header_h, l->avail_w, l->avail_h};
    float radius = 24.0f * l->scale;
    float saturation = app->frost_vibrancy ? 1.45f : 1.0f;
    gallery_render_frosted(app, frame, blurred, bounds, radius, saturation);
}
