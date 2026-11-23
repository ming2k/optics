/*
 * Canvas rendering backend interface. Never installed.
 *
 * The canvas front end (canvas.c and the geometry/path/paint helpers) is
 * backend-neutral: it owns the transform/clip stack, flattens paths and
 * strokes into contours, batches axis-aligned rects and glyph quads into
 * triangle streams, and assembles the shared uniform block
 * (optics_gfx_canvas_push). Everything that touches the GPU — target
 * resources, render passes, pipeline binding and the draw itself — lives
 * behind this vtable.
 *
 * A backend keeps its private per-canvas state in
 * optics_gfx_canvas::backend_data (allocated in canvas_init, freed in
 * canvas_destroy); the front end never looks inside it, so struct
 * optics_gfx_canvas holds no Vulkan types.
 *
 * Two implementations exist, held to pixel parity (ADR-0019):
 *   - Vulkan (optics_gfx_canvas_backend_vk), the default: records into the
 *     frame's command buffer with cached pipelines and rasterizes contours
 *     with the GPU tile-compute coverage pipeline.
 *   - CPU (optics_gfx_canvas_backend_cpu): rasterizes the same streams and
 *     contours on the host, interpreting the same push block, into a host
 *     framebuffer. Used for headless rendering, golden-image tests and
 *     GPU-free environments.
 *
 * Scope is the canvas 2D layer only. scene/compute/effect and the
 * <optics/gfx/vulkan.h> escape hatch remain Vulkan-only by design.
 */
#ifndef OPTICS_GFX_CANVAS_BACKEND_H
#define OPTICS_GFX_CANVAS_BACKEND_H

#include "internal.h"

struct optics_gfx_canvas_backend {
    /* Diagnostic label ("vulkan", "cpu", ...). */
    const char *name;

    /* Create/destroy the backend's private per-canvas state. canvas_init
     * runs once at optics_gfx_canvas_create with the canvas fully populated except
     * backend_data (which it sets); it also validates the surface format and
     * warms any program cache. canvas_destroy releases everything, including
     * waiting for in-flight frames if the backend owns frame-referenced
     * resources. */
    optics_gfx_result (*canvas_init)(const optics_gfx_canvas_backend *self, optics_gfx_canvas *c);
    void (*canvas_destroy)(const optics_gfx_canvas_backend *self, optics_gfx_canvas *c);

    /* Open a render pass. `target` selects the destination: NULL renders to
     * the canvas's surface (swapchain), non-NULL renders into an offscreen
     * optics_gfx_image (ADR-0017). `config` is the normalized clear and
     * dirty-render-area policy parsed once by the front end from the public
     * descriptor. The backend sets up attachments/barriers and primes the
     * render-area scissor. On success the caller sets c->recording/pass_active. */
    optics_gfx_result (*begin_pass)(const optics_gfx_canvas_backend *self, optics_gfx_canvas *c,
                                    optics_gfx_frame *f, optics_gfx_image *target,
                                    const canvas_pass_config *config);

    /* Close the pass opened by begin_pass, emitting any trailing transition
     * (e.g. an offscreen target back to shader-read layout). */
    void (*end_pass)(const optics_gfx_canvas_backend *self, optics_gfx_canvas *c);

    /* Apply `clip` (the active scissor, in physical pixels) to the live pass.
     * Called on clip changes and save/restore. No-op when no pass is active. */
    void (*set_scissor)(const optics_gfx_canvas_backend *self, optics_gfx_canvas *c,
                        optics_gfx_recti clip);

    /* Save/restore isolated compositing layer (ADR-0091). */
    optics_gfx_result (*save_layer)(const optics_gfx_canvas_backend *self, optics_gfx_canvas *c,
                                    const optics_gfx_rect *bounds, float opacity);
    void (*restore_layer)(const optics_gfx_canvas_backend *self, optics_gfx_canvas *c);

    /* Ensure the program for `id` at the canvas's colour format is ready and
     * selected for subsequent draws. GPU: build/cache + bind the VkPipeline
     * (idempotent). CPU: select the raster routine. Returns false when
     * unavailable (the caller then drops the draw). submit() calls this
     * internally; it is also exposed for call sites that bind once and emit
     * several batches (a chunked glyph run). */
    bool (*bind_program)(const optics_gfx_canvas_backend *self, optics_gfx_canvas *c,
                         canvas_pipe_id id);

    /* Emit one triangle batch. `push` holds the fully-populated, backend-
     * neutral uniforms MINUS the vertex source: the backend owns vertex
     * transport (GPU fills push.verts_address; CPU reads `verts`). `verts`/
     * `vertex_count` is the triangle stream in physical pixels; the active
     * scissor (c->states[c->state_top].scissor) clips it. `push` is not
     * mutated. On resource exhaustion the batch is dropped and
     * c->dropped_draws is incremented. */
    void (*submit)(const optics_gfx_canvas_backend *self, optics_gfx_canvas *c, canvas_pipe_id id,
                   const optics_gfx_canvas_push *push, const optics_gfx_canvas_vertex *verts,
                   uint32_t vertex_count);

    /* Required: analytic-coverage fill of contours (ADR-0119). Every path
     * fill and every stroke outline reaches the backend here, as continuous
     * per-pixel area coverage. `pts` are in PHYSICAL PIXELS (the front end has
     * already applied the canvas transform), `cons` index runs within them,
     * `rule` selects non-zero or even-odd, and `clip` bounds the work. `paint`
     * is the brush (kind, colour, gradient stops); a backend returns
     * OPTICS_GFX_ERROR_UNSUPPORTED for a brush it cannot evaluate and the
     * front end records the failure on the canvas. */
    optics_gfx_result (*submit_cover)(const optics_gfx_canvas_backend *self, optics_gfx_canvas *c,
                                      const optics_gfx_point *pts,
                                      const optics_gfx_canvas_contour *cons, uint32_t ncons,
                                      optics_gfx_fill_rule rule, const canvas_paint *paint,
                                      optics_gfx_recti clip);

    /* Optional (nullable) pixel snapshot: return premultiplied RGBA8 for the
     * whole canvas. The CPU backend implements it (its framebuffer); the GPU
     * backend leaves it NULL (readback goes through an offscreen target). */
    const uint8_t *(*read_pixels)(const optics_gfx_canvas_backend *self, optics_gfx_canvas *c,
                                  uint32_t *w, uint32_t *h, uint32_t *stride);

    /* Optional (nullable) single-channel alpha-coverage snapshot: the software
     * rasteriser's resolved coverage per pixel, which is what a baked coverage
     * mask is made of. Only the CPU backend implements it (a GPU canvas would
     * have to resolve an MSAA attachment and read it back); the GPU backend
     * leaves it NULL, and optics_gfx_canvas_cpu_coverage returns NULL there. */
    const uint8_t *(*read_coverage)(const optics_gfx_canvas_backend *self, optics_gfx_canvas *c,
                                    uint32_t *w, uint32_t *h, uint32_t *stride);
};

/* The default GPU backend. Stateless singleton; safe to share across
 * canvases, devices and threads. */
const optics_gfx_canvas_backend *optics_gfx_canvas_backend_vk(void);

/* The software (CPU) backend. Stateless singleton. Used by headless canvases
 * created with optics_gfx_canvas_create (CPU backend) (see gfx/canvas_cpu.h). */
const optics_gfx_canvas_backend *optics_gfx_canvas_backend_cpu(void);

#endif /* OPTICS_GFX_CANVAS_BACKEND_H */
