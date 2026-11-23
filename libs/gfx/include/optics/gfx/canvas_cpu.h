/*
 * gfx/canvas_cpu.h — headless software (CPU) canvas.
 *
 * A optics_gfx_canvas that renders on the host into a premultiplied-RGBA framebuffer,
 * with no Vulkan device, surface, or GPU required. It shares the entire
 * optics_gfx_canvas drawing API (<optics/gfx/canvas.h>): once created, record with the same
 * optics_gfx_canvas_fill_rect / fill_rrect / fill_path / clip_rect / save / restore
 * verbs, bracketed by optics_gfx_canvas_begin / optics_gfx_canvas_end instead of the
 * frame-based optics_gfx_canvas_begin / _end.
 *
 * Supported: solid fills, linear/radial gradients, rounded-rect fills and
 * strokes (SDF), arbitrary path fills and strokes, clipping, and glyph runs.
 * Vector geometry renders at 1x resolution with continuous analytic coverage
 * across all four blend modes (ADR-0119/0120). Glyph runs use a host-resident
 * R8 coverage atlas (ADR-0019): pass optics_gfx_glyph_run_desc::host_coverage on a
 * CPU canvas. NOT supported: image draws (need a GPU-resident texture).
 *
 * Example:
 *   optics_gfx_canvas *c;
 *   optics_gfx_canvas_create(&(optics_gfx_canvas_desc){.type = OPTICS_GFX_TYPE_CANVAS_DESC,
 * .backend = OPTICS_GFX_CANVAS_BACKEND_CPU, .scale = 1.0f, .width = 256, .height = 256}, &c);
 * optics_gfx_color bg = optics_gfx_color_rgba_premul(0, 0, 0, 255); optics_gfx_canvas_begin(c,
 * &bg); optics_gfx_canvas_fill_rrect(c, (optics_gfx_rect){32, 32, 192, 192}, 24.0f,
 * optics_gfx_color_rgba_premul(255, 80, 40, 255)); optics_gfx_canvas_end(c); uint32_t w, h, stride;
 *   const uint8_t *px = optics_gfx_canvas_cpu_pixels(c, &w, &h, &stride); // RGBA8
 *   ... write px ...
 *   optics_gfx_canvas_release(c);
 */
#ifndef OPTICS_GFX_CANVAS_CPU_H
#define OPTICS_GFX_CANVAS_CPU_H

#include <optics/gfx/canvas.h>
#include <optics/gfx/core.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Return the premultiplied-RGBA8 framebuffer (row-major, tightly packed:
 * *stride == width*4). The buffer is refreshed from the internal float
 * framebuffer on each call and remains owned by the canvas. Returns NULL if `c`
 * is not a CPU canvas. `width`/`height`/`stride` are optional out-params. */
OPTICS_GFX_API const uint8_t *optics_gfx_canvas_cpu_pixels(const optics_gfx_canvas *c,
                                                           uint32_t *width, uint32_t *height,
                                                           uint32_t *stride);

/* Return a single-channel alpha-coverage plane of the software framebuffer:
 * width*height bytes, one 8-bit coverage value per output pixel, row-major,
 * tightly packed. This is the mask readback that coverage caches are baked
 * from — clear to transparent, render the geometry once, read this plane, and
 * the bytes are an exact resolution-specific coverage mask for that geometry
 * at that size.
 *
 * The value is the 1x continuous analytic coverage alpha quantised to 8 bits
 * (up to 256 distinct coverage levels, matching optics_gfx_canvas_cpu_pixels' alpha
 * channel).
 *
 * The buffer is refreshed on each call and owned by the canvas; a later draw
 * into the canvas invalidates it. Returns NULL if `c` is not a CPU canvas.
 * `width`/`height`/`stride` are optional out-params (stride == width). */
OPTICS_GFX_API const uint8_t *optics_gfx_canvas_cpu_coverage(const optics_gfx_canvas *c,
                                                             uint32_t *width, uint32_t *height,
                                                             uint32_t *stride);

#ifdef __cplusplus
}
#endif

#endif /* OPTICS_GFX_CANVAS_CPU_H */
