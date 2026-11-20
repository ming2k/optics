/*
 * Canvas rendering backend interface. Never installed.
 *
 * The canvas front end (canvas.c and the geometry/path/paint helpers) is
 * backend-neutral: it owns the transform/clip stack, tessellates verbs into
 * triangle batches, and assembles the shared uniform block (flux_canvas_push).
 * Everything that touches the GPU — target resources, render passes,
 * pipeline binding, and the draw itself — lives behind this vtable.
 *
 * A backend keeps its private per-canvas state in flux_canvas::backend_data
 * (allocated in canvas_init, freed in canvas_destroy); the front end never
 * looks inside it. struct flux_canvas therefore holds no Vulkan types.
 *
 * Two implementations exist (Skia-style CPU/GPU parity):
 *   - Vulkan (flux_canvas_backend_vk): records into the frame's command
 *     buffer via cached 1x pipelines and GPU compute tile rasterization.
 *     This is the default.
 *   - CPU raster (flux_canvas_backend_cpu): rasterizes the same batches on
 *     the host, selecting a scanline routine per canvas_pipe_id and
 *     interpreting the same push block; canvas_init/begin_pass allocate a
 *     host framebuffer instead. Used for headless rendering, golden-image
 *     tests, and GPU-free environments (ADR-0019).
 *
 * Scope is the canvas 2D layer only. scene/compute/effect and the
 * <flux/vulkan.h> escape hatch remain Vulkan-only by design.
 */
#ifndef OPTICS_GFX_CANVAS_BACKEND_H
#define OPTICS_GFX_CANVAS_BACKEND_H

#include "internal.h"

struct optics_gfx_canvas_backend {
    /* Diagnostic label ("vulkan", "cpu", ...). */
    const char *name;

    /* Create/destroy the backend's private per-canvas state. canvas_init
     * runs once at flux_canvas_create with the canvas fully populated except
     * backend_data (which it sets); it also validates the surface format and
     * warms any program cache. canvas_destroy releases everything, including
     * waiting for in-flight frames if the backend owns frame-referenced
     * resources. */
    optics_gfx_result (*canvas_init)(const optics_gfx_canvas_backend *self, optics_gfx_canvas *c);
    void (*canvas_destroy)(const optics_gfx_canvas_backend *self, optics_gfx_canvas *c);

    /* Open a render pass. `target` selects the destination: NULL renders to
     * the canvas's surface (swapchain), non-NULL renders into an offscreen
     * flux_image (ADR-0017). `config` is the normalized clear and
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
     * `vertex_count` is the tessellated stream in physical pixels; the active
     * scissor (c->states[c->state_top].scissor) clips it. `push` is not
     * mutated. On resource exhaustion the batch is dropped and
     * c->dropped_draws is incremented. */
    void (*submit)(const optics_gfx_canvas_backend *self, optics_gfx_canvas *c, canvas_pipe_id id,
                   const optics_gfx_canvas_push *push, const optics_gfx_canvas_vertex *verts,
                   uint32_t vertex_count);

    /* Optional (nullable) analytic-coverage fill (ADR-0119). A backend that can
     * rasterize contours directly consumes the geometry instead of a tessellated
     * triangle stream and produces continuous coverage — no ear clipping, no
     * stencil attachment, no sample count. `pts` are in PHYSICAL PIXELS (the
     * front end has already applied the canvas transform) and `cons` index runs
     * within them; `clip` bounds the work. `paint` is the draw's brush — kind, colour and gradient
     * stops — so a backend evaluates exactly the brushes it would on the triangle path. A backend
     * returns FLUX_ERROR_UNSUPPORTED for a brush it cannot evaluate, and the
     * front end falls back to tessellation for that draw.
     * A backend that leaves this NULL simply keeps the triangle path, so the
     * two presentations coexist per draw rather than per build.
     *
     * Geometry is the geometry; the brush is the brush. Coverage answers *where*
     * a pixel is painted, so it applies to every paint kind the backend can
     * evaluate — that is what lets gradient fills reach it too. */
    optics_gfx_result (*submit_cover)(const optics_gfx_canvas_backend *self, optics_gfx_canvas *c,
                                      const optics_gfx_point *pts,
                                      const optics_gfx_canvas_contour *cons, uint32_t ncons,
                                      optics_gfx_fill_rule rule, const canvas_paint *paint,
                                      optics_gfx_recti clip);

    /* Optional (nullable): the same coverage route for geometry that already is
     * a triangle list — stroke expansions and the fill fallback. `verts` are in
     * physical pixels, three per triangle, any orientation; shared edges cancel
     * exactly under area coverage, so the mesh fills without seams. Returns
     * FLUX_ERROR_UNSUPPORTED to decline (paint kind, size bounds); the caller
     * then draws the triangles directly. */
    optics_gfx_result (*submit_cover_triangles)(const optics_gfx_canvas_backend *self,
                                                optics_gfx_canvas *c,
                                                const optics_gfx_canvas_vertex *verts,
                                                uint32_t vertex_count, const canvas_paint *paint,
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
     * leaves it NULL, and flux_canvas_cpu_coverage returns NULL there. */
    const uint8_t *(*read_coverage)(const optics_gfx_canvas_backend *self, optics_gfx_canvas *c,
                                    uint32_t *w, uint32_t *h, uint32_t *stride);
};

/* The default GPU backend. Stateless singleton; safe to share across
 * canvases, devices and threads. */
const optics_gfx_canvas_backend *optics_gfx_canvas_backend_vk(void);

/* The software (CPU) backend. Stateless singleton. Used by headless canvases
 * created with flux_canvas_create (CPU backend) (see flux/canvas_cpu.h). */
const optics_gfx_canvas_backend *optics_gfx_canvas_backend_cpu(void);

#endif /* FLUX_CANVAS_BACKEND_H */
