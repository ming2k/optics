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
 *   flux_canvas_release(c);
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
 * flux_canvas_release.
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
 * MSAA_4X select the 2x supersampled default (4 samples/px); NONE selects the
 * aliased 1-sample buffer (1 sample/px). */
FLUX_NODISCARD FLUX_API flux_result flux_canvas_create_cpu_aa(uint32_t width, uint32_t height,
                                                              float scale,
                                                              flux_canvas_antialias antialias,
                                                              flux_canvas **out);

/* flux_canvas_create_cpu with an explicit supersampling factor per axis
 * (ss x ss samples per pixel). `supersample == 0` keeps the default (2 = 4
 * samples/px); 1 selects 1 sample/px (aliased); 2..FLUX_CANVAS_SUPERSAMPLE_MAX
 * sets the factor. Out of range fails with FLUX_ERROR_OUT_OF_RANGE.
 *
 * Higher factors buy grey levels: N samples resolve N+1 coverage steps (ss=4 ->
 * 17, ss=8 -> 65). This is the direct constructor for coverage-mask bakers
 * (flux_canvas_cpu_coverage); display canvases should keep the default. */
FLUX_NODISCARD FLUX_API flux_result flux_canvas_create_cpu_ss(uint32_t width, uint32_t height,
                                                              float scale, uint32_t supersample,
                                                              flux_canvas **out);

/* Return the premultiplied-RGBA8 framebuffer (row-major, tightly packed:
 * *stride == width*4). The buffer is refreshed from the internal float
 * framebuffer on each call and remains owned by the canvas. Returns NULL if `c`
 * is not a CPU canvas. `width`/`height`/`stride` are optional out-params. */
FLUX_API const uint8_t *flux_canvas_cpu_pixels(const flux_canvas *c, uint32_t *width,
                                               uint32_t *height, uint32_t *stride);

/* Return a single-channel alpha-coverage plane of the software framebuffer:
 * width*height bytes, one 8-bit coverage value per output pixel, row-major,
 * tightly packed. This is the mask readback that coverage caches are baked
 * from — clear to transparent, render the geometry once, read this plane, and
 * the bytes are an exact resolution-specific coverage mask for that geometry
 * at that size.
 *
 * The value is the box-filtered mean of the supersampled alpha channel (the
 * same resolve flux_canvas_cpu_pixels performs), so the number of distinct
 * coverage levels is bounded by the canvas's supersample factor rather than by
 * 8-bit quantisation: ss=1 gives {0,1}, ss=4 gives 17, ss=8 gives 65. Request
 * the factor through flux_canvas_supersample_desc (<flux/canvas.h>).
 *
 * The buffer is refreshed on each call and owned by the canvas; a later draw
 * into the canvas invalidates it. Returns NULL if `c` is not a CPU canvas.
 * `width`/`height`/`stride` are optional out-params (stride == width). */
FLUX_API const uint8_t *flux_canvas_cpu_coverage(const flux_canvas *c, uint32_t *width,
                                                 uint32_t *height, uint32_t *stride);

#ifdef __cplusplus
}
#endif

#endif /* FLUX_CANVAS_CPU_H */
