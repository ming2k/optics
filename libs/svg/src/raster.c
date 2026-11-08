/* raster.c — headless rasterization through the software canvas.
 *
 * This is the path a caller takes when it needs pixels rather than draw
 * commands: a DRM/KMS hardware-cursor dumb buffer, a coverage mask, a headless
 * snapshot in CI. It uses flux's device-less CPU canvas, so it needs no Vulkan
 * device, no surface, and no window, and it is safe off the UI thread.
 *
 * The coverage readback is flux's own box-filtered alpha plane, not a
 * re-derivation: a mask baked here resolves exactly as many grey levels as the
 * canvas's supersample factor allows, which is the property mask caches key on. */

#include "internal.h"

#include <flux/canvas_cpu.h>

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

flux_result svg_rasterize(const svg_rasterize_opts *opts, svg_pixmap *out) {
    if (!opts || !opts->document || !out)
        return FLUX_ERROR_INVALID_ARGUMENT;
    if (opts->width == 0 || opts->height == 0)
        return FLUX_ERROR_INVALID_ARGUMENT;
    if (opts->format > SVG_PIXELS_COVERAGE)
        return FLUX_ERROR_INVALID_ARGUMENT;
    if (opts->paint != SVG_PAINT_THEME && opts->paint != SVG_PAINT_MONOCHROME)
        return FLUX_ERROR_INVALID_ARGUMENT;
    /* The extent is bounded here, in this library, rather than relying on the
     * CPU canvas rejecting it later: `row_bytes * height` and `width * height`
     * below are allocations this function performs, so their overflow is this
     * function's invariant to state. 4 bytes per pixel for the RGBA/BGRA
     * layouts, 1 for coverage. */
    const size_t bytes_per_pixel = opts->format == SVG_PIXELS_COVERAGE ? 1u : 4u;
    if ((size_t)opts->width > SIZE_MAX / bytes_per_pixel / (size_t)opts->height)
        return FLUX_ERROR_OUT_OF_RANGE;
    *out = (svg_pixmap){};

    flux_canvas *canvas = nullptr;
    flux_result result =
        flux_canvas_create_cpu_ss(opts->width, opts->height, 1.0f, opts->supersample, &canvas);
    if (result != FLUX_OK)
        return result;

    /* Transparent clear: a cursor sprite and an icon mask both need transparent
     * background as the *only* correct default. Clear is premultiplied zero. */
    flux_color transparent = 0;
    flux_canvas_pass_desc pass = {
        .type = FLUX_TYPE_CANVAS_PASS_DESC,
        .clear_color = &transparent,
    };
    result = flux_canvas_begin(canvas, &pass);
    if (result != FLUX_OK) {
        flux_canvas_release(canvas);
        return result;
    }

    const svg_draw_opts draw = {
        .document = opts->document,
        .canvas = canvas,
        .encoder = nullptr,
        .bounds = (flux_rect){0.0f, 0.0f, (float)opts->width, (float)opts->height},
        .fit = SVG_FIT_CONTAIN,
        .paint = opts->paint,
        .color = opts->color,
        .opacity = 0.0f,
        .stroke_width = opts->stroke_width,
    };
    result = svg_draw(&draw);
    flux_result end_result = flux_canvas_end(canvas);
    if (result == FLUX_OK)
        result = end_result;
    if (result != FLUX_OK) {
        flux_canvas_release(canvas);
        return result;
    }

    if (opts->format == SVG_PIXELS_COVERAGE) {
        uint32_t width = 0, height = 0, stride = 0;
        const uint8_t *coverage = flux_canvas_cpu_coverage(canvas, &width, &height, &stride);
        if (!coverage || width == 0 || height == 0) {
            flux_canvas_release(canvas);
            return FLUX_ERROR_UNSUPPORTED;
        }
        uint8_t *pixels = (uint8_t *)svg__alloc((size_t)width * (size_t)height);
        if (!pixels) {
            flux_canvas_release(canvas);
            return FLUX_ERROR_OUT_OF_MEMORY;
        }
        for (uint32_t y = 0; y < height; ++y)
            memcpy(pixels + (size_t)y * width, coverage + (size_t)y * stride, width);
        *out = (svg_pixmap){
            .width = width,
            .height = height,
            .stride = width,
            .format = SVG_PIXELS_COVERAGE,
            .pixels = pixels,
        };
        flux_canvas_release(canvas);
        return FLUX_OK;
    }

    uint32_t width = 0, height = 0, stride = 0;
    const uint8_t *rgba = flux_canvas_cpu_pixels(canvas, &width, &height, &stride);
    if (!rgba || width == 0 || height == 0) {
        flux_canvas_release(canvas);
        return FLUX_ERROR_UNSUPPORTED;
    }
    const size_t row_bytes = (size_t)width * 4u;
    uint8_t *pixels = (uint8_t *)svg__alloc(row_bytes * (size_t)height);
    if (!pixels) {
        flux_canvas_release(canvas);
        return FLUX_ERROR_OUT_OF_MEMORY;
    }
    if (opts->format == SVG_PIXELS_BGRA8_PREMUL) {
        /* A channel swap, not a second rasterization: the pixel values are
         * identical, only their order differs, and that order is what a KMS
         * cursor plane and a B8G8R8A8_UNORM sample read. */
        for (uint32_t y = 0; y < height; ++y) {
            const uint8_t *src = rgba + (size_t)y * stride;
            uint8_t *dst = pixels + (size_t)y * row_bytes;
            for (uint32_t x = 0; x < width; ++x) {
                dst[x * 4 + 0] = src[x * 4 + 2];
                dst[x * 4 + 1] = src[x * 4 + 1];
                dst[x * 4 + 2] = src[x * 4 + 0];
                dst[x * 4 + 3] = src[x * 4 + 3];
            }
        }
    } else {
        for (uint32_t y = 0; y < height; ++y)
            memcpy(pixels + (size_t)y * row_bytes, rgba + (size_t)y * stride, row_bytes);
    }
    *out = (svg_pixmap){
        .width = width,
        .height = height,
        .stride = (uint32_t)row_bytes,
        .format = opts->format,
        .pixels = pixels,
    };
    flux_canvas_release(canvas);
    return FLUX_OK;
}

void svg_pixmap_deinit(svg_pixmap *pixmap) {
    if (!pixmap)
        return;
    svg__free(pixmap->pixels);
    *pixmap = (svg_pixmap){};
}
