/*
 * flux/canvas_helpers.h — Zero-cost inline convenience layer (RFC-0094 / ADR-0087).
 *
 * Orthogonal Core with Inline Layers: Dynamic libraries (.so / .dylib / .dll)
 * export ONLY orthogonal unified drawing primitives (flux_canvas_draw and
 * flux_canvas_draw_glyph_run). All specialized convenience functions are provided
 * as header-only static inline wrappers, guaranteeing zero-cost abstraction without
 * polluting the dynamic symbol table.
 */

#ifndef FLUX_CANVAS_HELPERS_H
#define FLUX_CANVAS_HELPERS_H

#include <flux/canvas.h>

#ifdef __cplusplus
extern "C" {
#endif

/* =============================================================================
 * Algebraic Geometry & Brush Helpers (ADR-0088 Clean-Break)
 * ============================================================================= */

static inline flux_brush flux_brush_solid(flux_color color) {
    return (flux_brush){
        .kind = FLUX_BRUSH_SOLID,
        .blend = FLUX_BLEND_SRC_OVER,
        .opacity = 1.0f,
        .solid = {.color = color},
    };
}

static inline flux_geometry flux_geom_rect(flux_rect r) {
    return (flux_geometry){
        .kind = FLUX_GEOM_RECT,
        .stroke_width = 0.0f,
        .rect = {.rect = r},
    };
}

static inline flux_geometry flux_geom_rrect(flux_rect r, float radius) {
    return (flux_geometry){
        .kind = FLUX_GEOM_RRECT,
        .stroke_width = 0.0f,
        .rrect = {.rect = r, .radius = radius},
    };
}

static inline flux_geometry flux_geom_squircle(flux_rect r, float radius, float curvature) {
    return (flux_geometry){
        .kind = FLUX_GEOM_SQUIRCLE,
        .stroke_width = 0.0f,
        .squircle = {.rect = r, .radius = radius, .curvature = curvature},
    };
}

static inline flux_geometry flux_geom_circle(float cx, float cy, float radius) {
    return (flux_geometry){
        .kind = FLUX_GEOM_CIRCLE,
        .stroke_width = 0.0f,
        .circle = {.cx = cx, .cy = cy, .radius = radius},
    };
}

static inline flux_geometry flux_geom_line(float x0, float y0, float x1, float y1) {
    return (flux_geometry){
        .kind = FLUX_GEOM_LINE,
        .stroke_width = 1.0f,
        .line = {.x0 = x0, .y0 = y0, .x1 = x1, .y1 = y1},
    };
}

static inline flux_geometry flux_geom_path(const flux_path *path) {
    return (flux_geometry){
        .kind = FLUX_GEOM_PATH,
        .stroke_width = 0.0f,
        .path = {.path = path, .fill_rule = FLUX_FILL_NON_ZERO},
    };
}

/* =============================================================================
 * Drawing convenience functions (Header-only static inline)
 *
 * All helpers construct clean-break Geometry x Brush representations and
 * route exclusively through flux_canvas_draw_geometry (ADR-0089 / ADR-0091).
 * ============================================================================= */

static inline flux_brush flux_brush_from_paint(const flux_paint *paint) {
    if (!paint)
        return flux_brush_solid(0xFF000000u);
    flux_brush b = {
        .blend = paint->blend,
        .opacity = 1.0f,
    };
    if (paint->kind == FLUX_PAINT_SOLID) {
        b.kind = FLUX_BRUSH_SOLID;
        b.solid.color = paint->color;
    } else if (paint->kind == FLUX_PAINT_LINEAR_GRADIENT) {
        b.kind = FLUX_BRUSH_LINEAR_GRADIENT;
        b.gradient.start = paint->gradient.linear.from;
        b.gradient.end = paint->gradient.linear.to;
        b.gradient.stops = paint->gradient.linear.stops;
    } else if (paint->kind == FLUX_PAINT_RADIAL_GRADIENT) {
        b.kind = FLUX_BRUSH_RADIAL_GRADIENT;
        b.gradient.start = paint->gradient.radial.center;
        b.gradient.radius = paint->gradient.radial.radius;
        b.gradient.stops = paint->gradient.radial.stops;
    }
    return b;
}

static inline void flux_canvas_fill_rect(flux_canvas *c, flux_rect r, const flux_paint *paint) {
    flux_geometry g = flux_geom_rect(r);
    flux_brush b = flux_brush_from_paint(paint);
    flux_canvas_draw_geometry(c, &g, &b);
}

static inline void flux_canvas_fill_rect_color(flux_canvas *c, flux_rect r, flux_color color) {
    flux_geometry g = flux_geom_rect(r);
    flux_brush b = flux_brush_solid(color);
    flux_canvas_draw_geometry(c, &g, &b);
}

static inline void flux_canvas_fill_rrect(flux_canvas *c, flux_rect r, float radius, flux_color color) {
    flux_geometry g = flux_geom_rrect(r, radius);
    flux_brush b = flux_brush_solid(color);
    flux_canvas_draw_geometry(c, &g, &b);
}

static inline void flux_canvas_stroke_rrect(flux_canvas *c, flux_rect r, float radius,
                                           flux_color color, float width) {
    flux_geometry g = flux_geom_rrect(r, radius);
    g.stroke_width = width;
    flux_brush b = flux_brush_solid(color);
    flux_canvas_draw_geometry(c, &g, &b);
}

static inline void flux_canvas_fill_circle(flux_canvas *c, float cx, float cy, float radius,
                                          flux_color color) {
    flux_geometry g = flux_geom_circle(cx, cy, radius);
    flux_brush b = flux_brush_solid(color);
    flux_canvas_draw_geometry(c, &g, &b);
}

static inline void flux_canvas_stroke_circle(flux_canvas *c, float cx, float cy, float radius,
                                            flux_color color, float width) {
    flux_geometry g = flux_geom_circle(cx, cy, radius);
    g.stroke_width = width;
    flux_brush b = flux_brush_solid(color);
    flux_canvas_draw_geometry(c, &g, &b);
}

static inline void flux_canvas_fill_path(flux_canvas *c, const flux_path *p, const flux_paint *paint) {
    flux_geometry g = {
        .kind = FLUX_GEOM_PATH,
        .path = {.path = p, .fill_rule = paint ? paint->fill_rule : FLUX_FILL_NON_ZERO},
    };
    flux_brush b = flux_brush_from_paint(paint);
    flux_canvas_draw_geometry(c, &g, &b);
}

static inline void flux_canvas_stroke_path(flux_canvas *c, const flux_path *p, const flux_paint *paint) {
    flux_geometry g = {
        .kind = FLUX_GEOM_PATH,
        .path = {.path = p},
        .stroke_width = paint ? paint->stroke_width : 1.0f,
    };
    flux_brush b = flux_brush_from_paint(paint);
    flux_canvas_draw_geometry(c, &g, &b);
}

static inline void flux_canvas_draw_image(flux_canvas *c, flux_image *image, flux_rect dst,
                                         const flux_paint *optional_paint) {
    flux_geometry g = flux_geom_rect(dst);
    flux_brush b = {
        .kind = FLUX_BRUSH_IMAGE_PATTERN,
        .blend = optional_paint ? optional_paint->blend : FLUX_BLEND_SRC_OVER,
        .opacity = 1.0f,
        .image = {.image = image},
    };
    flux_canvas_draw_geometry(c, &g, &b);
}

static inline void flux_canvas_draw_image_opaque(flux_canvas *c, flux_image *image, flux_rect dst) {
    flux_geometry g = flux_geom_rect(dst);
    flux_brush b = {
        .kind = FLUX_BRUSH_IMAGE_PATTERN,
        .blend = FLUX_BLEND_SRC_OVER,
        .opacity = 1.0f,
        .image = {.image = image, .opaque_only = true},
    };
    flux_canvas_draw_geometry(c, &g, &b);
}

static inline void flux_canvas_draw_image_rrect(flux_canvas *c, flux_image *image, flux_rect dst,
                                               float radius, const flux_paint *optional_paint) {
    flux_geometry g = flux_geom_rrect(dst, radius);
    flux_brush b = {
        .kind = FLUX_BRUSH_IMAGE_PATTERN,
        .blend = optional_paint ? optional_paint->blend : FLUX_BLEND_SRC_OVER,
        .opacity = 1.0f,
        .image = {.image = image},
    };
    flux_canvas_draw_geometry(c, &g, &b);
}

static inline void flux_canvas_draw_image_clipped_rrect(
    flux_canvas *c,
    flux_image *image,
    flux_rect dst,
    flux_rect clip,
    float radius,
    const flux_paint *optional_paint
) {
    flux_geometry g = flux_geom_rect(dst);
    flux_brush b = {
        .kind = FLUX_BRUSH_IMAGE_PATTERN,
        .blend = optional_paint ? optional_paint->blend : FLUX_BLEND_SRC_OVER,
        .opacity = 1.0f,
        .image = {
            .image = image,
            .clip_rect = clip,
            .clip_radius = radius,
            .tint = optional_paint ? optional_paint->color : 0xFFFFFFFFu,
        },
    };
    flux_canvas_draw_geometry(c, &g, &b);
}

static inline void flux_canvas_draw_image_sub(flux_canvas *c, flux_image *image, flux_rect dst,
                                             flux_rect src) {
    flux_geometry g = flux_geom_rect(dst);
    flux_brush b = {
        .kind = FLUX_BRUSH_IMAGE_PATTERN,
        .blend = FLUX_BLEND_SRC_OVER,
        .opacity = 1.0f,
        .image = {.image = image, .src_rect = src, .tint = 0xFFFFFFFFu},
    };
    flux_canvas_draw_geometry(c, &g, &b);
}

static inline void flux_canvas_draw_image_opaque_sub(flux_canvas *c, flux_image *image, flux_rect dst,
                                                    flux_rect src) {
    flux_geometry g = flux_geom_rect(dst);
    flux_brush b = {
        .kind = FLUX_BRUSH_IMAGE_PATTERN,
        .blend = FLUX_BLEND_SRC_OVER,
        .opacity = 1.0f,
        .image = {.image = image, .src_rect = src, .opaque_only = true, .tint = 0xFFFFFFFFu},
    };
    flux_canvas_draw_geometry(c, &g, &b);
}

static inline void flux_canvas_draw_image_sampled(flux_canvas *c, flux_image *image,
                                                 flux_sampler *sampler, flux_rect dst,
                                                 const flux_paint *optional_paint) {
    flux_geometry g = flux_geom_rect(dst);
    flux_brush b = {
        .kind = FLUX_BRUSH_IMAGE_PATTERN,
        .blend = optional_paint ? optional_paint->blend : FLUX_BLEND_SRC_OVER,
        .opacity = 1.0f,
        .image = {
            .image = image,
            .sampler = sampler,
            .tint = optional_paint ? optional_paint->color : 0xFFFFFFFFu,
        },
    };
    flux_canvas_draw_geometry(c, &g, &b);
}

#ifdef __cplusplus
}
#endif

#endif /* FLUX_CANVAS_HELPERS_H */
