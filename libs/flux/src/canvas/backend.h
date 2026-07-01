/*
 * Canvas rendering backend interface. Never installed.
 *
 * The canvas front end (canvas.c and the geometry/path/paint helpers) is
 * backend-neutral: it owns the transform/clip stack, tessellates verbs into
 * triangle batches, and assembles the shared uniform block (flux_canvas_push).
 * Everything that touches the GPU — target/MSAA/stencil resources, render
 * passes, pipeline binding, and the draw itself — lives behind this vtable.
 *
 * A backend keeps its private per-canvas state in flux_canvas::backend_data
 * (allocated in canvas_init, freed in canvas_destroy); the front end never
 * looks inside it. struct flux_canvas therefore holds no Vulkan types.
 *
 * Two implementations are envisaged (Skia-style CPU/GPU parity):
 *   - Vulkan (flux_canvas_backend_vk): records into the frame's command
 *     buffer via cached pipelines, resolving 4x MSAA to the surface/target.
 *     This is the default.
 *   - CPU raster (future): rasterizes the same batches on the host, selecting
 *     a scanline routine per canvas_pipe_id and interpreting the same push
 *     block; canvas_init/begin_pass allocate a host framebuffer instead.
 *
 * Scope is the canvas 2D layer only. scene/compute/effect and the
 * <flux/vulkan.h> escape hatch remain Vulkan-only by design.
 */
#ifndef FLUX_CANVAS_BACKEND_H
#define FLUX_CANVAS_BACKEND_H

#include "internal.h"

struct flux_canvas_backend {
    /* Diagnostic label ("vulkan", "cpu", ...). */
    const char *name;

    /* Create/destroy the backend's private per-canvas state. canvas_init
     * runs once at flux_canvas_create with the canvas fully populated except
     * backend_data (which it sets); it also validates the surface format and
     * warms any program cache. canvas_destroy releases everything, including
     * waiting for in-flight frames if the backend owns frame-referenced
     * resources. */
    flux_result (*canvas_init)(const flux_canvas_backend *self, flux_canvas *c);
    void (*canvas_destroy)(const flux_canvas_backend *self, flux_canvas *c);

    /* Open a render pass. `target` selects the destination: NULL renders to
     * the canvas's surface (swapchain), non-NULL renders into an offscreen
     * flux_image (ADR-0017). `clear` is an optional clear colour. The backend
     * sets up attachments/barriers, binds shared descriptor state, primes the
     * full-extent scissor, and sets c->stencil_available for the geometry
     * fallback. On success the caller sets c->recording/pass_active. */
    flux_result (*begin_pass)(const flux_canvas_backend *self, flux_canvas *c, flux_frame *f,
                              flux_image *target, const flux_color *clear);

    /* Close the pass opened by begin_pass, emitting any trailing transition
     * (e.g. an offscreen target back to shader-read layout). */
    void (*end_pass)(const flux_canvas_backend *self, flux_canvas *c);

    /* Apply `clip` (the active scissor, in physical pixels) to the live pass.
     * Called on clip changes and save/restore. No-op when no pass is active. */
    void (*set_scissor)(const flux_canvas_backend *self, flux_canvas *c, flux_recti clip);

    /* Ensure the program for `id` at the canvas's colour format is ready and
     * selected for subsequent draws. GPU: build/cache + bind the VkPipeline
     * (idempotent). CPU: select the raster routine. Returns false when
     * unavailable (the caller then drops the draw). submit() calls this
     * internally; it is also exposed for call sites that bind once and emit
     * several batches (a chunked glyph run). */
    bool (*bind_program)(const flux_canvas_backend *self, flux_canvas *c, canvas_pipe_id id);

    /* Emit one triangle batch. `push` holds the fully-populated, backend-
     * neutral uniforms MINUS the vertex source: the backend owns vertex
     * transport (GPU fills push.verts_address; CPU reads `verts`). `verts`/
     * `vertex_count` is the tessellated stream in physical pixels; the active
     * scissor (c->states[c->state_top].scissor) clips it. `push` is not
     * mutated. On resource exhaustion the batch is dropped and
     * c->dropped_draws is incremented. */
    void (*submit)(const flux_canvas_backend *self, flux_canvas *c, canvas_pipe_id id,
                   const flux_canvas_push *push, const flux_canvas_vertex *verts,
                   uint32_t vertex_count);

    /* Optional (nullable) pixel snapshot: return premultiplied RGBA8 for the
     * whole canvas. The CPU backend implements it (its framebuffer); the GPU
     * backend leaves it NULL (readback goes through an offscreen target). */
    const uint8_t *(*read_pixels)(const flux_canvas_backend *self, flux_canvas *c, uint32_t *w,
                                  uint32_t *h, uint32_t *stride);
};

/* The default GPU backend. Stateless singleton; safe to share across
 * canvases, devices and threads. */
const flux_canvas_backend *flux_canvas_backend_vk(void);

/* The software (CPU) backend. Stateless singleton. Used by headless canvases
 * created with flux_canvas_create_cpu (see flux/canvas_cpu.h). */
const flux_canvas_backend *flux_canvas_backend_cpu(void);

#endif /* FLUX_CANVAS_BACKEND_H */
