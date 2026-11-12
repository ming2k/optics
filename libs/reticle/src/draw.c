/* draw.c — painting a document onto a canvas or an encoder.
 *
 * The painter is deliberately thin: the document already holds resolved paths
 * in user space and resolved paint values, so drawing is (1) compute the fit
 * transform, (2) for each shape emit geometry + brush. There is no scene graph
 * walk and no state machine, which is what makes a per-shape recolour
 * (monochrome mask bake, cursor silhouette) a paint-mode flag rather than a
 * second code path.
 *
 * Group opacity is folded into each shape's opacity at parse time (see the
 * header's boundary note). That means this painter never needs a save_layer, so
 * it produces identical output on the GPU canvas, the CPU canvas, and an
 * encoder — the three backends have no opacity-group surface of their own. */

#include "internal.h"

#include <math.h>

static bool resolve_fill_color(flux_color theme, const reticle_paint *paint,
                               reticle_paint_mode mode, float global_alpha, flux_color *out) {
    if (mode == RETICLE_PAINT_MONOCHROME) {
        /* Monochrome keeps coverage, not colour: the theme colour replaces the
         * paint entirely, but the paint's own alpha still multiplies, so a
         * half-transparent detail stays half-transparent.
         *
         * "Not painted" still means not painted: an unpainted half must return
         * false before the mode substitution, otherwise every fill-only shape
         * gains a full-weight stroke in its own colour and covers the art it
         * was supposed to be. */
        if (paint->kind == RETICLE_PAINT_NONE)
            return false;
        uint8_t r = 0, g = 0, b = 0, a = 0;
        flux_color_unpack(theme, &r, &g, &b, &a);
        float alpha = (float)a / 255.0f * paint->opacity * global_alpha;
        if (alpha <= 0.0f)
            return false;
        uint8_t quantized = (uint8_t)lrintf(fminf(alpha, 1.0f) * 255.0f);
        *out = flux_color_rgba_premul(r, g, b, quantized);
        return true;
    }
    switch (paint->kind) {
    case RETICLE_PAINT_NONE:
        return false;
    case RETICLE_PAINT_CURRENT_COLOR: {
        uint8_t r = 0, g = 0, b = 0, a = 0;
        flux_color_unpack(theme, &r, &g, &b, &a);
        float alpha = (float)a / 255.0f * paint->opacity * global_alpha;
        if (alpha <= 0.0f)
            return false;
        uint8_t quantized = (uint8_t)lrintf(fminf(alpha, 1.0f) * 255.0f);
        *out = flux_color_rgba_premul(r, g, b, quantized);
        return true;
    }
    case RETICLE_PAINT_SOLID: {
        uint8_t r = 0, g = 0, b = 0, a = 0;
        flux_color_unpack(paint->color, &r, &g, &b, &a);
        float alpha = (float)a / 255.0f * paint->opacity * global_alpha;
        if (alpha <= 0.0f)
            return false;
        uint8_t quantized = (uint8_t)lrintf(fminf(alpha, 1.0f) * 255.0f);
        *out = flux_color_rgba_premul(r, g, b, quantized);
        return true;
    }
    default:
        return false; /* gradients never reach the solid path */
    }
}

/* Build the brush for one paint. Gradients are already in user space, so they
 * need the same fit transform the geometry gets; the two endpoints are
 * transformed here and flux interpolates in the destination space. */
static bool build_brush(const reticle_document *doc, const reticle_paint *paint,
                        reticle_paint_mode mode, flux_color theme, float global_alpha,
                        svg_transform fit, flux_brush *out) {
    flux_color solid = 0;
    if (resolve_fill_color(theme, paint, mode, global_alpha, &solid)) {
        *out = flux_brush_solid(solid);
        return true;
    }
    if (mode == RETICLE_PAINT_MONOCHROME)
        return false;
    if (paint->kind != RETICLE_PAINT_LINEAR_GRADIENT &&
        paint->kind != RETICLE_PAINT_RADIAL_GRADIENT)
        return false;

    reticle_gradient_view gradient = {};
    if (reticle_gradient_at(doc, paint->gradient, &gradient) != FLUX_OK)
        return false;

    /* Gradients must be unique per shape, so they are not actually shared in the
     * flux brush: each draw gets its own value. */
    svg_gradient resolved = {};
    resolved.stop_count = gradient.stop_count;
    for (uint32_t i = 0; i < gradient.stop_count; ++i)
        resolved.stops[i] = gradient.stops[i];

    float sx = 0.0f, sy = 0.0f, ex = 0.0f, ey = 0.0f;
    svg__xform_point(fit, gradient.start.x, gradient.start.y, &sx, &sy);
    svg__xform_point(fit, gradient.end.x, gradient.end.y, &ex, &ey);
    resolved.start = (flux_point){sx, sy};
    resolved.end = (flux_point){ex, ey};
    /* A radial gradient's radius scales with the fit's uniform factor; the fit
     * is uniform by construction for both CONTAIN and STRETCH (the stretch case
     * uses the geometry transform, not the brush). */
    float scale = sqrtf(fabsf(fit.a * fit.d - fit.b * fit.c));
    if (!(scale > 0.0f))
        scale = 1.0f;
    resolved.radius = gradient.radius * scale;

    const reticle_paint_kind kind = paint->kind;
    if (kind == RETICLE_PAINT_LINEAR_GRADIENT) {
        *out = flux_brush_linear_gradient(resolved.start, resolved.end, resolved.stops,
                                          resolved.stop_count);
    } else {
        *out = flux_brush_radial_gradient(resolved.start, resolved.radius, resolved.stops,
                                          resolved.stop_count);
    }
    if (global_alpha < 1.0f) {
        /* flux's brush opacity is a separate multiplier; using it keeps gradient
         * stop colours from having to be rebuilt per frame. */
        out->opacity = global_alpha;
    }
    return true;
}

/* The transform mapping the document viewport into `bounds`. */
static svg_transform fit_transform(flux_rect viewport, flux_rect bounds, reticle_fit fit,
                                   float *scale_out) {
    float view_w = viewport.w > 0.0f ? viewport.w : 1.0f;
    float view_h = viewport.h > 0.0f ? viewport.h : 1.0f;
    if (fit == RETICLE_FIT_STRETCH) {
        float sx = bounds.w / view_w;
        float sy = bounds.h / view_h;
        if (scale_out)
            *scale_out = 0.5f * (sx + sy);
        svg_transform transform =
            svg__xform_mul(svg__xform_translate(bounds.x, bounds.y),
                           svg__xform_mul(svg__xform_scale(sx, sy),
                                          svg__xform_translate(-viewport.x, -viewport.y)));
        return transform;
    }
    float scale = view_w > 0.0f ? bounds.w / view_w : 1.0f;
    float by_h = view_h > 0.0f ? bounds.h / view_h : 1.0f;
    if (by_h < scale)
        scale = by_h;
    if (scale_out)
        *scale_out = scale;
    float drawn_w = view_w * scale;
    float drawn_h = view_h * scale;
    float offset_x = bounds.x + (bounds.w - drawn_w) * 0.5f;
    float offset_y = bounds.y + (bounds.h - drawn_h) * 0.5f;
    return svg__xform_mul(svg__xform_translate(offset_x, offset_y),
                          svg__xform_mul(svg__xform_scale(scale, scale),
                                         svg__xform_translate(-viewport.x, -viewport.y)));
}

typedef struct svg_sink {
    flux_canvas *canvas;
    flux_encoder *encoder;
} svg_sink;

static void sink_draw(svg_sink sink, const flux_geometry *geometry, const flux_brush *brush) {
    if (sink.canvas)
        flux_canvas_draw_geometry(sink.canvas, geometry, brush);
    else
        flux_encoder_draw_geometry(sink.encoder, geometry, brush);
}

static void sink_transform(svg_sink sink, svg_transform transform) {
    flux_mat3x2 matrix = {
        .m = {transform.a, transform.b, transform.c, transform.d, transform.e, transform.f},
    };
    if (sink.canvas)
        flux_canvas_transform(sink.canvas, matrix);
    else
        flux_encoder_transform(sink.encoder, matrix);
}

static void sink_save(svg_sink sink) {
    if (sink.canvas)
        flux_canvas_save(sink.canvas);
    else
        flux_encoder_save(sink.encoder);
}

static void sink_restore(svg_sink sink) {
    if (sink.canvas)
        flux_canvas_restore(sink.canvas);
    else
        flux_encoder_restore(sink.encoder);
}

static void sink_clip(svg_sink sink, flux_rect rect) {
    if (sink.canvas)
        flux_canvas_clip_rect(sink.canvas, rect);
    else
        flux_encoder_clip_rect(sink.encoder, rect);
}

flux_result reticle_draw(const reticle_draw_opts *opts) {
    if (!opts || !opts->document)
        return FLUX_ERROR_INVALID_ARGUMENT;
    if ((opts->canvas == nullptr) == (opts->encoder == nullptr))
        return FLUX_ERROR_INVALID_ARGUMENT;
    if (!(opts->bounds.w > 0.0f) || !(opts->bounds.h > 0.0f))
        return FLUX_ERROR_INVALID_ARGUMENT;
    if (opts->fit != RETICLE_FIT_CONTAIN && opts->fit != RETICLE_FIT_STRETCH)
        return FLUX_ERROR_INVALID_ARGUMENT;
    if (opts->paint != RETICLE_PAINT_THEME && opts->paint != RETICLE_PAINT_MONOCHROME)
        return FLUX_ERROR_INVALID_ARGUMENT;

    const reticle_document *doc = opts->document;
    if (doc->shape_count == 0)
        return FLUX_OK;

    float global_alpha = opts->opacity;
    if (!(global_alpha > 0.0f))
        global_alpha = 1.0f;
    if (global_alpha > 1.0f)
        global_alpha = 1.0f;

    /* `reticle_draw` owns its arena: the paths it creates are per-call, and a caller
     * must not have to provision one for a convenience draw. */
    flux_arena arena;
    flux_result result = flux_arena_init(&arena, 64u * 1024u, nullptr);
    if (result != FLUX_OK)
        return result;

    const flux_rect viewport = doc->viewport;
    float uniform_scale = 1.0f;
    const svg_transform fit = fit_transform(viewport, opts->bounds, opts->fit, &uniform_scale);

    svg_sink sink = {.canvas = opts->canvas, .encoder = opts->encoder};
    sink_save(sink);
    /* Containment box: a document must never paint outside the box it was asked
     * to fill, even when a shape's stroke or a mis-declared viewBox reaches
     * past it. */
    sink_clip(sink, opts->bounds);
    sink_transform(sink, fit);

    for (uint32_t i = 0; i < doc->shape_count; ++i) {
        const reticle_shape_view *shape = &doc->shapes[i];
        /* Element and group opacity is a per-shape property, so it multiplies
         * here rather than in a document-wide constant. Folding it into the
         * caller's global alpha is exact for the non-overlapping case and is the
         * documented boundary (no isolated group compositing): a shape's own
         * alpha must never be silently discarded, which is what using the bare
         * `global_alpha` for every shape would do. */
        const float shape_alpha = global_alpha * (shape->opacity > 0.0f ? shape->opacity : 1.0f);
        if (shape_alpha <= 0.0f)
            continue;

        flux_path *path = nullptr;
        if (flux_path_create(&path, &arena) != FLUX_OK) {
            result = FLUX_ERROR_OUT_OF_MEMORY;
            break;
        }
        reticle_path_emit(shape, path);

        flux_geometry geometry = flux_geom_path(path);
        geometry.path.fill_rule = shape->fill_rule;

        /* Fill first, then stroke over it: SVG's paint order. */
        flux_brush fill_brush = {};
        if (build_brush(doc, &shape->fill, opts->paint, opts->color, shape_alpha, fit,
                        &fill_brush)) {
            sink_draw(sink, &geometry, &fill_brush);
        }

        flux_brush stroke_brush = {};
        if (shape->stroke_width > 0.0f && build_brush(doc, &shape->stroke, opts->paint, opts->color,
                                                      shape_alpha, fit, &stroke_brush)) {
            /* Stroke width is in *user* units and must be handed to flux raw:
             * the canvas transform (already applied above) scales it to device
             * pixels. Multiplying by the fit scale here would scale it twice. */
            float width = shape->stroke_width;
            if (opts->stroke_width > 0.0f) {
                /* An explicit override is documented in destination units, so it
                 * is divided by the fit scale for the transform to undo. */
                width =
                    uniform_scale > 0.0f ? opts->stroke_width / uniform_scale : opts->stroke_width;
            }
            if (width > 0.0f) {
                flux_geometry stroked = geometry;
                stroked.stroke.width = width;
                stroked.stroke.cap = shape->cap;
                stroked.stroke.join = shape->join;
                stroked.stroke.miter_limit = shape->miter_limit;
                sink_draw(sink, &stroked, &stroke_brush);
            }
        }
    }

    sink_restore(sink);
    flux_arena_deinit(&arena);
    return result;
}
