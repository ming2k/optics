#include "internal.h"

#include <math.h>
#include <string.h>

canvas_paint canvas_paint_default(void) {
    return (canvas_paint){
        .kind = CANVAS_PAINT_SOLID,
        .color = 0xFF000000,
        .stroke_width = 1.0f,
        .miter_limit = 4.0f,
        .cap = FLUX_CAP_BUTT,
        .join = FLUX_JOIN_MITER,
        .fill_rule = FLUX_FILL_NON_ZERO,
        .blend = FLUX_BLEND_SRC_OVER,
    };
}

bool canvas_finite_rect(flux_rect r) {
    return isfinite(r.x) && isfinite(r.y) && isfinite(r.w) && isfinite(r.h) && r.w >= 0 &&
           r.h >= 0 && isfinite(r.x + r.w) && isfinite(r.y + r.h);
}

bool canvas_valid_geometry(const flux_geometry *g) {
    if (!g || !isfinite(g->stroke.width) || g->stroke.width < 0 ||
        !isfinite(g->stroke.miter_limit) || g->stroke.miter_limit < 0 ||
        g->stroke.cap < FLUX_CAP_BUTT || g->stroke.cap > FLUX_CAP_SQUARE ||
        g->stroke.join < FLUX_JOIN_MITER || g->stroke.join > FLUX_JOIN_BEVEL)
        return false;
    switch (g->kind) {
    case FLUX_GEOM_RECT:
        return canvas_finite_rect(g->rect.rect);
    case FLUX_GEOM_RRECT:
        return canvas_finite_rect(g->rrect.rect) && isfinite(g->rrect.radius) &&
               g->rrect.radius >= 0;
    case FLUX_GEOM_SQUIRCLE:
        return canvas_finite_rect(g->squircle.rect) && isfinite(g->squircle.radius) &&
               g->squircle.radius >= 0 && isfinite(g->squircle.curvature) &&
               g->squircle.curvature >= 0 && g->squircle.curvature <= 1;
    case FLUX_GEOM_CIRCLE:
        return isfinite(g->circle.cx) && isfinite(g->circle.cy) && isfinite(g->circle.radius) &&
               g->circle.radius >= 0;
    case FLUX_GEOM_LINE:
        return isfinite(g->line.x0) && isfinite(g->line.y0) && isfinite(g->line.x1) &&
               isfinite(g->line.y1);
    case FLUX_GEOM_PATH: {
        const flux_path *p = g->path.path;
        if (g->path.fill_rule > FLUX_FILL_EVEN_ODD || !p || p->dropped ||
            (p->count && !p->segments))
            return false;
        for (uint32_t i = 0; i < p->count; i++) {
            const flux_path_segment *seg = &p->segments[i];
            if (seg->op > FLUX_PATH_CLOSE)
                return false;
            unsigned n = seg->op == FLUX_PATH_CLOSE   ? 0
                         : seg->op == FLUX_PATH_CUBIC ? 6
                         : seg->op == FLUX_PATH_QUAD  ? 4
                                                      : 2;
            for (unsigned j = 0; j < n; j++)
                if (!isfinite(seg->pts[j]))
                    return false;
        }
        return true;
    }
    default:
        return false;
    }
}

bool canvas_valid_brush(const flux_brush *b) {
    if (!isfinite(b->opacity) || b->opacity < 0 || b->opacity > 1 ||
        b->blend < FLUX_BLEND_SRC_OVER || b->blend > FLUX_BLEND_MULTIPLY)
        return false;
    if (b->kind == FLUX_BRUSH_SOLID)
        return true;
    if (b->kind == FLUX_BRUSH_IMAGE_PATTERN)
        return b->image.image && canvas_finite_rect(b->image.src_rect);
    if (b->kind != FLUX_BRUSH_LINEAR_GRADIENT && b->kind != FLUX_BRUSH_RADIAL_GRADIENT)
        return false;
    const flux_brush_gradient_data *g = &b->gradient;
    if (!isfinite(g->start.x) || !isfinite(g->start.y) || !isfinite(g->end.x) ||
        !isfinite(g->end.y) || !isfinite(g->radius) || g->radius < 0 || !g->stops.count ||
        g->stops.count > FLUX_GRADIENT_MAX_STOPS)
        return false;
    float prev = 0;
    for (uint32_t i = 0; i < g->stops.count; i++) {
        float t = g->stops.stops[i].t;
        if (!isfinite(t) || t < prev || t > 1)
            return false;
        prev = t;
    }
    return true;
}
