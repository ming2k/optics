#include "internal.h"

#include <string.h>

flux_paint flux_paint_default(void) {
    return (flux_paint){
        .kind = FLUX_PAINT_SOLID,
        .color = 0xFF000000,
        .stroke_width = 1.0f,
        .miter_limit = 4.0f,
        .cap = FLUX_CAP_BUTT,
        .join = FLUX_JOIN_MITER,
        .fill_rule = FLUX_FILL_NON_ZERO,
        .blend = FLUX_BLEND_SRC_OVER,
    };
}

flux_paint flux_paint_solid(flux_color color) {
    flux_paint p = flux_paint_default();
    p.color = color;
    return p;
}

static void copy_stops(flux_gradient_stops *dst, const flux_gradient_stop *src, uint32_t n) {
    if (n > FLUX_GRADIENT_MAX_STOPS)
        n = FLUX_GRADIENT_MAX_STOPS;
    dst->count = n;
    if (n > 0 && src)
        memcpy(dst->stops, src, n * sizeof(*src));
}

flux_paint flux_paint_linear_gradient(flux_point from, flux_point to,
                                      const flux_gradient_stop *stops, uint32_t stop_count) {
    flux_paint p = flux_paint_default();
    p.kind = FLUX_PAINT_LINEAR_GRADIENT;
    p.gradient.linear.from = from;
    p.gradient.linear.to = to;
    copy_stops(&p.gradient.linear.stops, stops, stop_count);
    return p;
}

flux_paint flux_paint_radial_gradient(flux_point center, float radius,
                                      const flux_gradient_stop *stops, uint32_t stop_count) {
    flux_paint p = flux_paint_default();
    p.kind = FLUX_PAINT_RADIAL_GRADIENT;
    p.gradient.radial.center = center;
    p.gradient.radial.radius = radius;
    copy_stops(&p.gradient.radial.stops, stops, stop_count);
    return p;
}
