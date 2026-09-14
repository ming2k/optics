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

/* =============================================================================
 * Shape construction helpers (stack compound literals)
 * ============================================================================= */

static inline flux_shape flux_shape_rect(flux_rect r) {
    return (flux_shape){
        .kind = FLUX_SHAPE_RECT,
        .rect = r,
        .stroke_width = 0.0f,
    };
}

static inline flux_shape flux_shape_rrect(flux_rect r, float radius) {
    return (flux_shape){
        .kind = FLUX_SHAPE_RRECT,
        .rect = r,
        .radius = radius,
        .stroke_width = 0.0f,
    };
}

static inline flux_shape flux_shape_stroke_rrect(flux_rect r, float radius, float width) {
    return (flux_shape){
        .kind = FLUX_SHAPE_RRECT,
        .rect = r,
        .radius = radius,
        .stroke_width = width,
    };
}

static inline flux_shape flux_shape_circle(float cx, float cy, float radius) {
    return (flux_shape){
        .kind = FLUX_SHAPE_CIRCLE,
        .rect = (flux_rect){cx - radius, cy - radius, radius * 2.0f, radius * 2.0f},
        .radius = radius,
        .stroke_width = 0.0f,
    };
}

static inline flux_shape flux_shape_stroke_circle(float cx, float cy, float radius, float width) {
    return (flux_shape){
        .kind = FLUX_SHAPE_CIRCLE,
        .rect = (flux_rect){cx - radius, cy - radius, radius * 2.0f, radius * 2.0f},
        .radius = radius,
        .stroke_width = width,
    };
}

static inline flux_shape flux_shape_path(const flux_path *path) {
    return (flux_shape){
        .kind = FLUX_SHAPE_PATH,
        .path = path,
        .stroke_width = 0.0f,
    };
}

static inline flux_shape flux_shape_image(flux_image *image, flux_rect dst) {
    return (flux_shape){
        .kind = FLUX_SHAPE_IMAGE,
        .rect = dst,
        .image = image,
    };
}

static inline flux_shape flux_shape_image_clipped_rrect(flux_image *image, flux_rect dst,
                                                        flux_rect clip, float radius) {
    return (flux_shape){
        .kind = FLUX_SHAPE_IMAGE,
        .rect = dst,
        .image = image,
        .clip_rect = clip,
        .clip_radius = radius,
    };
}

/* =============================================================================
 * Drawing convenience functions (Header-only static inline)
 *
 * All helpers construct clean-break Geometry x Brush representations and
 * route exclusively through flux_canvas_draw_geometry (ADR-0089 / ADR-0091).
 * ============================================================================= */

static inline void flux_canvas_fill_rect(flux_canvas *c, flux_rect r, const flux_paint *paint) {
    flux_shape shape = {
        .kind = FLUX_SHAPE_RECT,
        .rect = r,
        .stroke_width = 0.0f
    };
    flux_canvas_draw(c, &shape, paint);
}

static inline void flux_canvas_fill_rect_color(flux_canvas *c, flux_rect r, flux_color color) {
    flux_paint paint = flux_paint_solid(color);
    flux_canvas_fill_rect(c, r, &paint);
}

static inline void flux_canvas_fill_rrect(flux_canvas *c, flux_rect r, float radius, flux_color color) {
    flux_paint paint = flux_paint_solid(color);
    flux_shape shape = {
        .kind = FLUX_SHAPE_RRECT,
        .rect = r,
        .radius = radius,
        .stroke_width = 0.0f
    };
    flux_canvas_draw(c, &shape, &paint);
}

static inline void flux_canvas_stroke_rrect(flux_canvas *c, flux_rect r, float radius,
                                           flux_color color, float width) {
    flux_paint paint = flux_paint_solid(color);
    flux_shape shape = {
        .kind = FLUX_SHAPE_RRECT,
        .rect = r,
        .radius = radius,
        .stroke_width = width
    };
    flux_canvas_draw(c, &shape, &paint);
}

static inline void flux_canvas_fill_circle(flux_canvas *c, float cx, float cy, float radius,
                                          flux_color color) {
    flux_paint paint = flux_paint_solid(color);
    flux_shape shape = {
        .kind = FLUX_SHAPE_CIRCLE,
        .rect = (flux_rect){cx - radius, cy - radius, radius * 2.0f, radius * 2.0f},
        .radius = radius,
        .stroke_width = 0.0f
    };
    flux_canvas_draw(c, &shape, &paint);
}

static inline void flux_canvas_stroke_circle(flux_canvas *c, float cx, float cy, float radius,
                                            flux_color color, float width) {
    flux_paint paint = flux_paint_solid(color);
    flux_shape shape = {
        .kind = FLUX_SHAPE_CIRCLE,
        .rect = (flux_rect){cx - radius, cy - radius, radius * 2.0f, radius * 2.0f},
        .radius = radius,
        .stroke_width = width
    };
    flux_canvas_draw(c, &shape, &paint);
}

static inline void flux_canvas_fill_path(flux_canvas *c, const flux_path *p, const flux_paint *paint) {
    flux_shape shape = {
        .kind = FLUX_SHAPE_PATH,
        .path = p,
        .stroke_width = 0.0f
    };
    flux_canvas_draw(c, &shape, paint);
}

static inline void flux_canvas_stroke_path(flux_canvas *c, const flux_path *p, const flux_paint *paint) {
    flux_shape shape = {
        .kind = FLUX_SHAPE_PATH,
        .path = p,
        .stroke_width = paint ? paint->stroke_width : 1.0f
    };
    flux_canvas_draw(c, &shape, paint);
}

static inline void flux_canvas_draw_image(flux_canvas *c, flux_image *image, flux_rect dst,
                                         const flux_paint *optional_paint) {
    flux_shape shape = {
        .kind = FLUX_SHAPE_IMAGE,
        .rect = dst,
        .image = image
    };
    flux_canvas_draw(c, &shape, optional_paint);
}

static inline void flux_canvas_draw_image_opaque(flux_canvas *c, flux_image *image, flux_rect dst) {
    flux_shape shape = {
        .kind = FLUX_SHAPE_IMAGE,
        .rect = dst,
        .image = image,
        .opaque_only = true
    };
    flux_canvas_draw(c, &shape, nullptr);
}

static inline void flux_canvas_draw_image_rrect(flux_canvas *c, flux_image *image, flux_rect dst,
                                               float radius, const flux_paint *optional_paint) {
    flux_shape shape = {
        .kind = FLUX_SHAPE_IMAGE,
        .rect = dst,
        .image = image,
        .radius = radius
    };
    flux_canvas_draw(c, &shape, optional_paint);
}

static inline void flux_canvas_draw_image_clipped_rrect(
    flux_canvas *c,
    flux_image *image,
    flux_rect dst,
    flux_rect clip,
    float radius,
    const flux_paint *optional_paint
) {
    flux_shape shape = {
        .kind = FLUX_SHAPE_IMAGE,
        .rect = dst,
        .image = image,
        .clip_rect = clip,
        .clip_radius = radius
    };
    flux_canvas_draw(c, &shape, optional_paint);
}

static inline void flux_canvas_draw_image_sub(flux_canvas *c, flux_image *image, flux_rect dst,
                                             flux_rect src) {
    flux_shape shape = {
        .kind = FLUX_SHAPE_IMAGE,
        .rect = dst,
        .image = image,
        .src_rect = src
    };
    flux_canvas_draw(c, &shape, nullptr);
}

static inline void flux_canvas_draw_image_opaque_sub(flux_canvas *c, flux_image *image, flux_rect dst,
                                                    flux_rect src) {
    flux_shape shape = {
        .kind = FLUX_SHAPE_IMAGE,
        .rect = dst,
        .image = image,
        .src_rect = src,
        .opaque_only = true
    };
    flux_canvas_draw(c, &shape, nullptr);
}

static inline void flux_canvas_draw_image_sampled(flux_canvas *c, flux_image *image,
                                                 flux_sampler *sampler, flux_rect dst,
                                                 const flux_paint *optional_paint) {
    flux_shape shape = {
        .kind = FLUX_SHAPE_IMAGE,
        .rect = dst,
        .image = image,
        .sampler = sampler
    };
    flux_canvas_draw(c, &shape, optional_paint);
}

#ifdef __cplusplus
}
#endif

#endif /* FLUX_CANVAS_HELPERS_H */
