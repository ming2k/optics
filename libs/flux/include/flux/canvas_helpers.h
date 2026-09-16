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
        .stroke.width = 0.0f,
        .rect = {.rect = r},
    };
}

static inline flux_geometry flux_geom_rrect(flux_rect r, float radius) {
    return (flux_geometry){
        .kind = FLUX_GEOM_RRECT,
        .stroke.width = 0.0f,
        .rrect = {.rect = r, .radius = radius},
    };
}

static inline flux_geometry flux_geom_squircle(flux_rect r, float radius, float curvature) {
    return (flux_geometry){
        .kind = FLUX_GEOM_SQUIRCLE,
        .stroke.width = 0.0f,
        .squircle = {.rect = r, .radius = radius, .curvature = curvature},
    };
}

static inline flux_geometry flux_geom_circle(float cx, float cy, float radius) {
    return (flux_geometry){
        .kind = FLUX_GEOM_CIRCLE,
        .stroke.width = 0.0f,
        .circle = {.cx = cx, .cy = cy, .radius = radius},
    };
}

static inline flux_geometry flux_geom_line(float x0, float y0, float x1, float y1) {
    return (flux_geometry){
        .kind = FLUX_GEOM_LINE,
        .stroke.width = 1.0f,
        .line = {.x0 = x0, .y0 = y0, .x1 = x1, .y1 = y1},
    };
}

static inline flux_geometry flux_geom_path(const flux_path *path) {
    return (flux_geometry){
        .kind = FLUX_GEOM_PATH,
        .stroke.width = 0.0f,
        .path = {.path = path, .fill_rule = FLUX_FILL_NON_ZERO},
    };
}

/* =============================================================================
 * Drawing convenience functions (Header-only static inline)
 *
 * All helpers construct clean-break Geometry x Brush representations and
 * route exclusively through flux_canvas_draw_geometry (ADR-0089 / ADR-0091).
 * ============================================================================= */

/* Constructors preserve invalid counts for draw-time validation; no truncation
 * is presented as success. The fixed storage limit is part of the contract. */
static inline flux_brush flux_brush_linear_gradient(flux_point start, flux_point end,
                                                    const flux_gradient_stop *stops,
                                                    uint32_t count) {
    flux_brush b = {.kind = FLUX_BRUSH_LINEAR_GRADIENT,
                    .opacity = 1.0f,
                    .gradient = {.start = start, .end = end, .stops.count = count}};
    if (stops && count <= FLUX_GRADIENT_MAX_STOPS)
        for (uint32_t i = 0; i < count; ++i)
            b.gradient.stops.stops[i] = stops[i];
    else
        b.gradient.stops.count = UINT32_MAX;
    return b;
}
static inline flux_brush flux_brush_radial_gradient(flux_point center, float radius,
                                                    const flux_gradient_stop *stops,
                                                    uint32_t count) {
    flux_brush b = flux_brush_linear_gradient(center, (flux_point){}, stops, count);
    b.kind = FLUX_BRUSH_RADIAL_GRADIENT;
    b.gradient.radius = radius;
    return b;
}

static inline void flux_canvas_fill_rect(flux_canvas *c, flux_rect r, const flux_brush *brush) {
    flux_geometry g = flux_geom_rect(r);
    flux_brush b = brush ? *brush : flux_brush_solid(0xFF000000u);
    flux_canvas_draw_geometry(c, &g, &b);
}

static inline void flux_canvas_fill_rect_color(flux_canvas *c, flux_rect r, flux_color color) {
    flux_geometry g = flux_geom_rect(r);
    flux_brush b = flux_brush_solid(color);
    flux_canvas_draw_geometry(c, &g, &b);
}

static inline void flux_canvas_fill_rrect(flux_canvas *c, flux_rect r, float radius,
                                          flux_color color) {
    flux_geometry g = flux_geom_rrect(r, radius);
    flux_brush b = flux_brush_solid(color);
    flux_canvas_draw_geometry(c, &g, &b);
}

static inline void flux_canvas_stroke_rrect(flux_canvas *c, flux_rect r, float radius,
                                            flux_color color, float width) {
    flux_geometry g = flux_geom_rrect(r, radius);
    g.stroke.width = width;
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
    g.stroke.width = width;
    flux_brush b = flux_brush_solid(color);
    flux_canvas_draw_geometry(c, &g, &b);
}

static inline void flux_canvas_fill_path(flux_canvas *c, const flux_path *path, flux_fill_rule rule,
                                         const flux_brush *brush) {
    flux_geometry geom = flux_geom_path(path);
    geom.path.fill_rule = rule;
    flux_canvas_draw_geometry(c, &geom, brush);
}
static inline void flux_canvas_stroke_path(flux_canvas *c, const flux_path *path,
                                           const flux_stroke_style *stroke,
                                           const flux_brush *brush) {
    flux_geometry geom = flux_geom_path(path);
    geom.stroke = stroke ? *stroke : (flux_stroke_style){.width = 1.0f};
    flux_canvas_draw_geometry(c, &geom, brush);
}

/* Image modulation is explicit; a null style selects white, full opacity,
 * SRC_OVER. A zero tint is transparent, not a sentinel. */
typedef struct flux_image_style {
    flux_color tint;
    float opacity;
    flux_blend_mode blend;
} flux_image_style;
#define FLUX_IMAGE_STYLE_INIT {.tint = 0xFFFFFFFFu, .opacity = 1.0f}

static inline void flux_canvas_draw_image(flux_canvas *c, flux_image *image, flux_rect dst,
                                          const flux_image_style *style) {
    flux_geometry g = flux_geom_rect(dst);
    flux_brush b = {
        .kind = FLUX_BRUSH_IMAGE_PATTERN,
        .blend = style ? style->blend : FLUX_BLEND_SRC_OVER,
        .opacity = style ? style->opacity : 1.0f,
        .image = {.image = image, .tint = style ? style->tint : 0xFFFFFFFFu},
    };
    flux_canvas_draw_geometry(c, &g, &b);
}

static inline void flux_canvas_draw_image_opaque(flux_canvas *c, flux_image *image, flux_rect dst) {
    flux_geometry g = flux_geom_rect(dst);
    flux_brush b = {
        .kind = FLUX_BRUSH_IMAGE_PATTERN,
        .blend = FLUX_BLEND_SRC_OVER,
        .opacity = 1.0f,
        .image = {.image = image, .opaque_only = true, .tint = 0xFFFFFFFFu},
    };
    flux_canvas_draw_geometry(c, &g, &b);
}

static inline void flux_canvas_draw_image_rrect(flux_canvas *c, flux_image *image, flux_rect dst,
                                                float radius, const flux_image_style *style) {
    flux_geometry g = flux_geom_rrect(dst, radius);
    flux_brush b = {
        .kind = FLUX_BRUSH_IMAGE_PATTERN,
        .blend = style ? style->blend : FLUX_BLEND_SRC_OVER,
        .opacity = style ? style->opacity : 1.0f,
        .image = {.image = image, .tint = style ? style->tint : 0xFFFFFFFFu},
    };
    flux_canvas_draw_geometry(c, &g, &b);
}

static inline void flux_canvas_draw_image_clipped_rrect(flux_canvas *c, flux_image *image,
                                                        flux_rect dst, flux_rect clip, float radius,
                                                        const flux_image_style *style) {
    flux_geometry g = flux_geom_rect(dst);
    flux_brush b = {
        .kind = FLUX_BRUSH_IMAGE_PATTERN,
        .blend = style ? style->blend : FLUX_BLEND_SRC_OVER,
        .opacity = style ? style->opacity : 1.0f,
        .image =
            {
                .image = image,
                .clip_rect = clip,
                .clip_radius = radius,
                .tint = style ? style->tint : 0xFFFFFFFFu,
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

static inline void flux_canvas_draw_image_opaque_sub(flux_canvas *c, flux_image *image,
                                                     flux_rect dst, flux_rect src) {
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
                                                  const flux_image_style *style) {
    flux_geometry g = flux_geom_rect(dst);
    flux_brush b = {
        .kind = FLUX_BRUSH_IMAGE_PATTERN,
        .blend = style ? style->blend : FLUX_BLEND_SRC_OVER,
        .opacity = style ? style->opacity : 1.0f,
        .image =
            {
                .image = image,
                .sampler = sampler,
                .tint = style ? style->tint : 0xFFFFFFFFu,
            },
    };
    flux_canvas_draw_geometry(c, &g, &b);
}

#ifdef __cplusplus
}
#endif

#endif /* FLUX_CANVAS_HELPERS_H */
