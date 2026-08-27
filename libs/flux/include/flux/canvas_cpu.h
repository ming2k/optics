/*
 * flux/canvas_cpu.h — headless software (CPU) canvas.
 *
 * A flux_canvas that renders on the host into a premultiplied-RGBA framebuffer,
 * with no Vulkan device, surface, or GPU required. It shares the entire
 * flux_canvas drawing API (<flux/canvas.h>): once created, record with the same
 * flux_canvas_fill_rect / fill_rrect / fill_path / clip_rect / save / restore
 * verbs, bracketed by flux_canvas_cpu_begin / flux_canvas_cpu_end instead of the
 * frame-based flux_canvas_begin_frame / _end.
 *
 * Supported: solid fills, linear/radial gradients, rounded-rect fills and
 * strokes (SDF), arbitrary path fills and strokes, clipping, and glyph runs.
 * Anti-aliasing is 4x supersampled; blending is premultiplied SRC_OVER —
 * matching the GPU backend. Glyph runs use a host-resident R8 coverage atlas
 * (ADR-0019): pass flux_glyph_run_desc::host_coverage on a CPU canvas. NOT
 * supported: image draws (they need a GPU-resident texture) — silently ignored.
 *
 * Example:
 *   flux_canvas *c;
 *   flux_canvas_create_cpu(256, 256, 1.0f, &c);
 *   flux_color bg = flux_color_rgba_premul(0, 0, 0, 255);
 *   flux_canvas_cpu_begin(c, &bg);
 *   flux_canvas_fill_rrect(c, (flux_rect){32, 32, 192, 192}, 24.0f,
 *                          flux_color_rgba_premul(255, 80, 40, 255));
 *   flux_canvas_cpu_end(c);
 *   uint32_t w, h, stride;
 *   const uint8_t *px = flux_canvas_cpu_pixels(c, &w, &h, &stride); // RGBA8
 *   ... write px ...
 *   flux_canvas_destroy(c);
 */
#ifndef FLUX_CANVAS_CPU_H
#define FLUX_CANVAS_CPU_H

#include <flux/canvas.h>
#include <flux/core.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Create a headless CPU canvas with a `width`x`height` (physical pixels)
 * framebuffer. `scale` is the content/device-pixel ratio applied by the base
 * transform (pass 1.0 for logical == physical). Destroy with
 * flux_canvas_destroy.
 *
 * Antialiasing: the CPU rasterizer supersamples 2x per axis (4 samples per
 * pixel) by default, mirroring the GPU backend's 4x MSAA — that is what the
 * cross-platform consistency oracle relies on. The convenience entry point
 * keeps that default; pass FLUX_CANVAS_ANTIALIAS_NONE to
 * flux_canvas_create_cpu_aa for a 1-sample-per-pixel buffer when aliasing
 * is acceptable — one quarter of the sample-buffer memory and raster work
 * (a 4K canvas drops from ~531 MiB of float framebuffer to ~133 MiB). The
 * factor is fixed at create time. */
FLUX_NODISCARD FLUX_API flux_result flux_canvas_create_cpu(uint32_t width, uint32_t height,
                                                           float scale, flux_canvas **out);

/* flux_canvas_create_cpu with an explicit antialiasing request. AUTO and
 * MSAA_4X both select the supersampled default; NONE selects the aliased
 * 1-sample buffer. flux_canvas_create with backend CPU honours the
 * descriptor's antialias the same way. */
FLUX_NODISCARD FLUX_API flux_result flux_canvas_create_cpu_aa(uint32_t width, uint32_t height,
                                                              float scale,
                                                              flux_canvas_antialias antialias,
                                                              flux_canvas **out);

/* Begin a recording pass, clearing to `clear` (premultiplied; NULL = fully
 * transparent). The CPU analogue of flux_canvas_begin_frame — no frame needed. */
FLUX_NODISCARD FLUX_API flux_result flux_canvas_cpu_begin(flux_canvas *c, const flux_color *clear);

/* End the recording pass. Pixels are already resolved in the framebuffer. */
FLUX_API void flux_canvas_cpu_end(flux_canvas *c);

/* Return the premultiplied-RGBA8 framebuffer (row-major, tightly packed:
 * *stride == width*4). The buffer is refreshed from the internal float
 * framebuffer on each call and remains owned by the canvas. Returns NULL if `c`
 * is not a CPU canvas. `width`/`height`/`stride` are optional out-params. */
FLUX_API const uint8_t *flux_canvas_cpu_pixels(const flux_canvas *c, uint32_t *width,
                                               uint32_t *height, uint32_t *stride);

#ifdef __cplusplus
}
#endif

#endif /* FLUX_CANVAS_CPU_H */
