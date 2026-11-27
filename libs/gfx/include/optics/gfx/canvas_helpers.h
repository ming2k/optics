/*
 * gfx/canvas_helpers.h — Zero-cost inline convenience layer (RFC-0094 / ADR-0087).
 *
 * Orthogonal Core with Inline Layers: Dynamic libraries (.so / .dylib / .dll)
 * export ONLY orthogonal unified drawing primitives (optics_gfx_canvas_draw_geometry
 * and optics_gfx_canvas_draw_glyph_run). All specialized convenience functions are provided
 * as header-only static inline wrappers, guaranteeing zero-cost abstraction without
 * polluting the dynamic symbol table.
 */

#ifndef OPTICS_GFX_CANVAS_HELPERS_H
#define OPTICS_GFX_CANVAS_HELPERS_H

#include <optics/gfx/canvas.h>

#ifdef __cplusplus
extern "C" {
#endif

/* =============================================================================
 * Algebraic Geometry & Brush Helpers (ADR-0088 Clean-Break)
 * ============================================================================= */

static inline optics_gfx_brush optics_gfx_brush_solid(optics_gfx_color color) {
    return (optics_gfx_brush){
        .kind = OPTICS_GFX_BRUSH_SOLID,
        .blend = OPTICS_GFX_BLEND_SRC_OVER,
        .opacity = 1.0f,
        .solid = {.color = color},
    };
}

static inline optics_gfx_geometry optics_gfx_geom_rect(optics_gfx_rect r) {
    return (optics_gfx_geometry){
        .kind = OPTICS_GFX_GEOM_RECT,
        .stroke.width = 0.0f,
        .rect = {.rect = r},
    };
}

static inline optics_gfx_geometry
optics_gfx_geom_rrect(optics_gfx_rect r, optics_gfx_corner_radii radii, float curvature) {
    return (optics_gfx_geometry){
        .kind = OPTICS_GFX_GEOM_RRECT,
        .stroke.width = 0.0f,
        .rrect = {.rect = r, .radii = radii, .curvature = curvature},
    };
}

static inline optics_gfx_geometry optics_gfx_geom_circle(float cx, float cy, float radius) {
    return (optics_gfx_geometry){
        .kind = OPTICS_GFX_GEOM_CIRCLE,
        .stroke.width = 0.0f,
        .circle = {.cx = cx, .cy = cy, .radius = radius},
    };
}

static inline optics_gfx_geometry optics_gfx_geom_line(float x0, float y0, float x1, float y1) {
    return (optics_gfx_geometry){
        .kind = OPTICS_GFX_GEOM_LINE,
        .stroke.width = 1.0f,
        .line = {.x0 = x0, .y0 = y0, .x1 = x1, .y1 = y1},
    };
}

static inline optics_gfx_geometry optics_gfx_geom_path(const optics_gfx_path *path) {
    return (optics_gfx_geometry){
        .kind = OPTICS_GFX_GEOM_PATH,
        .stroke.width = 0.0f,
        .path = {.path = path, .fill_rule = OPTICS_GFX_FILL_NON_ZERO},
    };
}

/* =============================================================================
 * Drawing convenience functions (Header-only static inline)
 *
 * All helpers construct clean-break Geometry x Brush representations and
 * route exclusively through optics_gfx_canvas_draw_geometry(ADR-0089 / ADR-0091).
 * ============================================================================= */

/* Constructors preserve invalid counts for draw-time validation; no truncation
 * is presented as success. The fixed storage limit is part of the contract. */
static inline optics_gfx_brush
optics_gfx_brush_linear_gradient(optics_gfx_point start, optics_gfx_point end,
                                 const optics_gfx_gradient_stop *stops, uint32_t count) {
    optics_gfx_brush b = {.kind = OPTICS_GFX_BRUSH_LINEAR_GRADIENT,
                          .opacity = 1.0f,
                          .gradient = {.start = start, .end = end, .stops.count = count}};
    if (stops && count <= OPTICS_GFX_GRADIENT_MAX_STOPS)
        for (uint32_t i = 0; i < count; ++i)
            b.gradient.stops.stops[i] = stops[i];
    else
        b.gradient.stops.count = UINT32_MAX;
    return b;
}
static inline optics_gfx_brush
optics_gfx_brush_radial_gradient(optics_gfx_point center, float radius,
                                 const optics_gfx_gradient_stop *stops, uint32_t count) {
    optics_gfx_brush b =
        optics_gfx_brush_linear_gradient(center, (optics_gfx_point){}, stops, count);
    b.kind = OPTICS_GFX_BRUSH_RADIAL_GRADIENT;
    b.gradient.radius = radius;
    return b;
}

static inline void optics_gfx_canvas_fill_rect(optics_gfx_canvas *c, optics_gfx_rect r,
                                               const optics_gfx_brush *brush) {
    optics_gfx_geometry g = optics_gfx_geom_rect(r);
    optics_gfx_brush b = brush ? *brush : optics_gfx_brush_solid(0xFF000000u);
    optics_gfx_canvas_draw_geometry(
        c, &(optics_gfx_canvas_draw_geometry_opts){.geom = &g, .brush = &b});
}

static inline void optics_gfx_canvas_fill_rect_color(optics_gfx_canvas *c, optics_gfx_rect r,
                                                     optics_gfx_color color) {
    optics_gfx_geometry g = optics_gfx_geom_rect(r);
    optics_gfx_brush b = optics_gfx_brush_solid(color);
    optics_gfx_canvas_draw_geometry(
        c, &(optics_gfx_canvas_draw_geometry_opts){.geom = &g, .brush = &b});
}

static inline void optics_gfx_canvas_fill_rrect(optics_gfx_canvas *c, optics_gfx_rect r,
                                                optics_gfx_corner_radii radii, float curvature,
                                                optics_gfx_color color) {
    optics_gfx_geometry g = optics_gfx_geom_rrect(r, radii, curvature);
    optics_gfx_brush b = optics_gfx_brush_solid(color);
    optics_gfx_canvas_draw_geometry(
        c, &(optics_gfx_canvas_draw_geometry_opts){.geom = &g, .brush = &b});
}

static inline void optics_gfx_canvas_stroke_rrect(optics_gfx_canvas *c, optics_gfx_rect r,
                                                  optics_gfx_corner_radii radii, float curvature,
                                                  optics_gfx_color color, float width) {
    optics_gfx_geometry g = optics_gfx_geom_rrect(r, radii, curvature);
    g.stroke.width = width;
    optics_gfx_brush b = optics_gfx_brush_solid(color);
    optics_gfx_canvas_draw_geometry(
        c, &(optics_gfx_canvas_draw_geometry_opts){.geom = &g, .brush = &b});
}

static inline void optics_gfx_canvas_fill_circle(optics_gfx_canvas *c, float cx, float cy,
                                                 float radius, optics_gfx_color color) {
    optics_gfx_geometry g = optics_gfx_geom_circle(cx, cy, radius);
    optics_gfx_brush b = optics_gfx_brush_solid(color);
    optics_gfx_canvas_draw_geometry(
        c, &(optics_gfx_canvas_draw_geometry_opts){.geom = &g, .brush = &b});
}

static inline void optics_gfx_canvas_stroke_circle(optics_gfx_canvas *c, float cx, float cy,
                                                   float radius, optics_gfx_color color,
                                                   float width) {
    optics_gfx_geometry g = optics_gfx_geom_circle(cx, cy, radius);
    g.stroke.width = width;
    optics_gfx_brush b = optics_gfx_brush_solid(color);
    optics_gfx_canvas_draw_geometry(
        c, &(optics_gfx_canvas_draw_geometry_opts){.geom = &g, .brush = &b});
}

static inline void optics_gfx_canvas_fill_path(optics_gfx_canvas *c, const optics_gfx_path *path,
                                               optics_gfx_fill_rule rule,
                                               const optics_gfx_brush *brush) {
    optics_gfx_geometry geom = optics_gfx_geom_path(path);
    geom.path.fill_rule = rule;
    optics_gfx_canvas_draw_geometry(
        c, &(optics_gfx_canvas_draw_geometry_opts){.geom = &geom, .brush = brush});
}
static inline void optics_gfx_canvas_stroke_path(optics_gfx_canvas *c, const optics_gfx_path *path,
                                                 const optics_gfx_stroke_style *stroke,
                                                 const optics_gfx_brush *brush) {
    optics_gfx_geometry geom = optics_gfx_geom_path(path);
    geom.stroke = stroke ? *stroke : (optics_gfx_stroke_style){.width = 1.0f};
    optics_gfx_canvas_draw_geometry(
        c, &(optics_gfx_canvas_draw_geometry_opts){.geom = &geom, .brush = brush});
}

/* Image modulation is explicit; a null style selects white, full opacity,
 * SRC_OVER. A zero tint is transparent, not a sentinel. */
typedef struct optics_gfx_image_style {
    optics_gfx_color tint;
    float opacity;
    optics_gfx_blend_mode blend;
} optics_gfx_image_style;
#define OPTICS_GFX_IMAGE_STYLE_INIT {.tint = 0xFFFFFFFFu, .opacity = 1.0f}

static inline void optics_gfx_canvas_draw_image(optics_gfx_canvas *c, optics_gfx_image *image,
                                                optics_gfx_rect dst,
                                                const optics_gfx_image_style *style) {
    optics_gfx_geometry g = optics_gfx_geom_rect(dst);
    optics_gfx_brush b = {
        .kind = OPTICS_GFX_BRUSH_IMAGE_PATTERN,
        .blend = style ? style->blend : OPTICS_GFX_BLEND_SRC_OVER,
        .opacity = style ? style->opacity : 1.0f,
        .image = {.image = image, .tint = style ? style->tint : 0xFFFFFFFFu},
    };
    optics_gfx_canvas_draw_geometry(
        c, &(optics_gfx_canvas_draw_geometry_opts){.geom = &g, .brush = &b});
}

static inline void optics_gfx_canvas_draw_image_opaque(optics_gfx_canvas *c,
                                                       optics_gfx_image *image,
                                                       optics_gfx_rect dst) {
    optics_gfx_geometry g = optics_gfx_geom_rect(dst);
    optics_gfx_brush b = {
        .kind = OPTICS_GFX_BRUSH_IMAGE_PATTERN,
        .blend = OPTICS_GFX_BLEND_SRC_OVER,
        .opacity = 1.0f,
        .image = {.image = image, .opaque_only = true, .tint = 0xFFFFFFFFu},
    };
    optics_gfx_canvas_draw_geometry(
        c, &(optics_gfx_canvas_draw_geometry_opts){.geom = &g, .brush = &b});
}

static inline void optics_gfx_canvas_draw_image_rrect(optics_gfx_canvas *c, optics_gfx_image *image,
                                                      optics_gfx_rect dst,
                                                      optics_gfx_corner_radii radii,
                                                      float curvature,
                                                      const optics_gfx_image_style *style) {
    optics_gfx_geometry g = optics_gfx_geom_rrect(dst, radii, curvature);
    optics_gfx_brush b = {
        .kind = OPTICS_GFX_BRUSH_IMAGE_PATTERN,
        .blend = style ? style->blend : OPTICS_GFX_BLEND_SRC_OVER,
        .opacity = style ? style->opacity : 1.0f,
        .image = {.image = image, .tint = style ? style->tint : 0xFFFFFFFFu},
    };
    optics_gfx_canvas_draw_geometry(
        c, &(optics_gfx_canvas_draw_geometry_opts){.geom = &g, .brush = &b});
}

static inline void optics_gfx_canvas_draw_image_clipped_rrect(
    optics_gfx_canvas *c, optics_gfx_image *image, optics_gfx_rect dst, optics_gfx_rect clip,
    optics_gfx_corner_radii radii, float curvature, const optics_gfx_image_style *style) {
    optics_gfx_geometry g = optics_gfx_geom_rect(dst);
    optics_gfx_brush b = {
        .kind = OPTICS_GFX_BRUSH_IMAGE_PATTERN,
        .blend = style ? style->blend : OPTICS_GFX_BLEND_SRC_OVER,
        .opacity = style ? style->opacity : 1.0f,
        .image =
            {
                .image = image,
                .clip_rect = clip,
                .clip_radii = radii,
                .clip_curvature = curvature,
                .tint = style ? style->tint : 0xFFFFFFFFu,
            },
    };
    optics_gfx_canvas_draw_geometry(
        c, &(optics_gfx_canvas_draw_geometry_opts){.geom = &g, .brush = &b});
}

static inline void optics_gfx_canvas_draw_image_sub(optics_gfx_canvas *c, optics_gfx_image *image,
                                                    optics_gfx_rect dst, optics_gfx_rect src) {
    optics_gfx_geometry g = optics_gfx_geom_rect(dst);
    optics_gfx_brush b = {
        .kind = OPTICS_GFX_BRUSH_IMAGE_PATTERN,
        .blend = OPTICS_GFX_BLEND_SRC_OVER,
        .opacity = 1.0f,
        .image = {.image = image, .src_rect = src, .tint = 0xFFFFFFFFu},
    };
    optics_gfx_canvas_draw_geometry(
        c, &(optics_gfx_canvas_draw_geometry_opts){.geom = &g, .brush = &b});
}

static inline void optics_gfx_canvas_draw_image_opaque_sub(optics_gfx_canvas *c,
                                                           optics_gfx_image *image,
                                                           optics_gfx_rect dst,
                                                           optics_gfx_rect src) {
    optics_gfx_geometry g = optics_gfx_geom_rect(dst);
    optics_gfx_brush b = {
        .kind = OPTICS_GFX_BRUSH_IMAGE_PATTERN,
        .blend = OPTICS_GFX_BLEND_SRC_OVER,
        .opacity = 1.0f,
        .image = {.image = image, .src_rect = src, .opaque_only = true, .tint = 0xFFFFFFFFu},
    };
    optics_gfx_canvas_draw_geometry(
        c, &(optics_gfx_canvas_draw_geometry_opts){.geom = &g, .brush = &b});
}

static inline void optics_gfx_canvas_draw_image_sampled(optics_gfx_canvas *c,
                                                        optics_gfx_image *image,
                                                        optics_gfx_sampler *sampler,
                                                        optics_gfx_rect dst,
                                                        const optics_gfx_image_style *style) {
    optics_gfx_geometry g = optics_gfx_geom_rect(dst);
    optics_gfx_brush b = {
        .kind = OPTICS_GFX_BRUSH_IMAGE_PATTERN,
        .blend = style ? style->blend : OPTICS_GFX_BLEND_SRC_OVER,
        .opacity = style ? style->opacity : 1.0f,
        .image =
            {
                .image = image,
                .sampler = sampler,
                .tint = style ? style->tint : 0xFFFFFFFFu,
            },
    };
    optics_gfx_canvas_draw_geometry(
        c, &(optics_gfx_canvas_draw_geometry_opts){.geom = &g, .brush = &b});
}

#ifdef __cplusplus
}
#endif

#endif /* OPTICS_GFX_CANVAS_HELPERS_H */
