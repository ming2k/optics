/* raster.c — headless rasterization through the software canvas.
 *
 * This is the path a caller takes when it needs pixels rather than draw
 * commands: a DRM/KMS hardware-cursor dumb buffer, a coverage mask, a headless
 * snapshot in CI. It uses gfx's device-less CPU canvas, so it needs no Vulkan
 * device, no surface, and no window, and it is safe off the UI thread.
 *
 * The coverage readback is gfx's 1x continuous analytic coverage alpha plane
 * (matching the alpha channel of RGBA8 readback). */

#include "internal.h"

#include <optics/gfx/canvas_cpu.h>

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

optics_gfx_result optics_svg_rasterize(const optics_svg_rasterize_opts *opts,
                                       optics_svg_pixmap *out) {
    if (!opts || !opts->document || !out)
        return OPTICS_GFX_ERROR_INVALID_ARGUMENT;
    if (opts->width == 0 || opts->height == 0)
        return OPTICS_GFX_ERROR_INVALID_ARGUMENT;
    if (opts->format > OPTICS_SVG_PIXELS_COVERAGE)
        return OPTICS_GFX_ERROR_INVALID_ARGUMENT;
    if (opts->paint != OPTICS_SVG_PAINT_THEME && opts->paint != OPTICS_SVG_PAINT_MONOCHROME)
        return OPTICS_GFX_ERROR_INVALID_ARGUMENT;
    /* The extent is bounded here, in this library, rather than relying on the
     * CPU canvas rejecting it later: `row_bytes * height` and `width * height`
     * below are allocations this function performs, so their overflow is this
     * function's invariant to state. 4 bytes per pixel for the RGBA/BGRA
     * layouts, 1 for coverage. */
    const size_t bytes_per_pixel = opts->format == OPTICS_SVG_PIXELS_COVERAGE ? 1u : 4u;
    if ((size_t)opts->width > SIZE_MAX / bytes_per_pixel / (size_t)opts->height)
        return OPTICS_GFX_ERROR_OUT_OF_RANGE;
    *out = (optics_svg_pixmap){};

    optics_gfx_canvas *canvas = nullptr;
    optics_gfx_result result =
        optics_gfx_canvas_create(&(optics_gfx_canvas_desc){.type = OPTICS_GFX_TYPE_CANVAS_DESC,
                                                           .backend = OPTICS_GFX_CANVAS_BACKEND_CPU,
                                                           .scale = 1.0f,
                                                           .width = opts->width,
                                                           .height = opts->height},
                                 &canvas);
    if (result != OPTICS_GFX_OK)
        return result;

    /* Transparent clear: a cursor sprite and an icon mask both need transparent
     * background as the *only* correct default. Clear is premultiplied zero. */
    optics_gfx_color transparent = 0;
    optics_gfx_canvas_pass_desc pass = {
        .type = OPTICS_GFX_TYPE_CANVAS_PASS_DESC,
        .clear_color = &transparent,
    };
    result = optics_gfx_canvas_begin(canvas, &pass);
    if (result != OPTICS_GFX_OK) {
        optics_gfx_canvas_release(canvas);
        return result;
    }

    const optics_svg_draw_opts draw = {
        .document = opts->document,
        .canvas = canvas,
        .encoder = nullptr,
        .bounds = (optics_gfx_rect){0.0f, 0.0f, (float)opts->width, (float)opts->height},
        .fit = OPTICS_SVG_FIT_CONTAIN,
        .paint = opts->paint,
        .color = opts->color,
        .opacity = 0.0f,
        .stroke_width = opts->stroke_width,
    };
    result = optics_svg_draw(&draw);
    optics_gfx_result end_result = optics_gfx_canvas_end(canvas);
    if (result == OPTICS_GFX_OK)
        result = end_result;
    if (result != OPTICS_GFX_OK) {
        optics_gfx_canvas_release(canvas);
        return result;
    }

    if (opts->format == OPTICS_SVG_PIXELS_COVERAGE) {
        uint32_t width = 0, height = 0, stride = 0;
        const uint8_t *coverage = optics_gfx_canvas_cpu_coverage(canvas, &width, &height, &stride);
        if (!coverage || width == 0 || height == 0) {
            optics_gfx_canvas_release(canvas);
            return OPTICS_GFX_ERROR_UNSUPPORTED;
        }
        uint8_t *pixels = (uint8_t *)optics_svg_priv_alloc((size_t)width * (size_t)height);
        if (!pixels) {
            optics_gfx_canvas_release(canvas);
            return OPTICS_GFX_ERROR_OUT_OF_MEMORY;
        }
        for (uint32_t y = 0; y < height; ++y)
            memcpy(pixels + (size_t)y * width, coverage + (size_t)y * stride, width);
        *out = (optics_svg_pixmap){
            .width = width,
            .height = height,
            .stride = width,
            .format = OPTICS_SVG_PIXELS_COVERAGE,
            .pixels = pixels,
        };
        optics_gfx_canvas_release(canvas);
        return OPTICS_GFX_OK;
    }

    uint32_t width = 0, height = 0, stride = 0;
    const uint8_t *rgba = optics_gfx_canvas_cpu_pixels(canvas, &width, &height, &stride);
    if (!rgba || width == 0 || height == 0) {
        optics_gfx_canvas_release(canvas);
        return OPTICS_GFX_ERROR_UNSUPPORTED;
    }
    const size_t row_bytes = (size_t)width * 4u;
    uint8_t *pixels = (uint8_t *)optics_svg_priv_alloc(row_bytes * (size_t)height);
    if (!pixels) {
        optics_gfx_canvas_release(canvas);
        return OPTICS_GFX_ERROR_OUT_OF_MEMORY;
    }
    if (opts->format == OPTICS_SVG_PIXELS_BGRA8_PREMUL) {
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
    *out = (optics_svg_pixmap){
        .width = width,
        .height = height,
        .stride = (uint32_t)row_bytes,
        .format = opts->format,
        .pixels = pixels,
    };
    optics_gfx_canvas_release(canvas);
    return OPTICS_GFX_OK;
}

void optics_svg_pixmap_deinit(optics_svg_pixmap *pixmap) {
    if (!pixmap)
        return;
    optics_svg_priv_free(pixmap->pixels);
    *pixmap = (optics_svg_pixmap){};
}
