/*
 * flux/canvas.h — 2D immediate-mode canvas.
 *
 * Design contract:
 *   - One canvas per surface.
 *   - flux_path, flux_paint are value types owned by a flux_arena.
 *   - flux_image is refcounted.
 *   - Per-frame:
 *       flux_surface_begin_frame  ->
 *       flux_canvas_begin (clear or load) ->
 *       record draws ->
 *       flux_canvas_end ->
 *       flux_frame_submit / flux_frame_present
 */

#ifndef FLUX_CANVAS_H
#define FLUX_CANVAS_H

#include <flux/core.h>
#include <flux/math.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct flux_canvas flux_canvas;

/* flux_sampler lives in <flux/vulkan.h>; forward-declare here so
 * flux_canvas_draw_image_sampled compiles without dragging the
 * Vulkan header into every canvas.h consumer. Mirrors the existing
 * forward declaration of flux_image inside <flux/vulkan.h>. */
typedef struct flux_sampler flux_sampler;
typedef struct flux_image flux_image;

/* ------------------------------------------------------------------ */
/*  Paint                                                             */
/* ------------------------------------------------------------------ */

typedef enum flux_line_cap {
    FLUX_CAP_BUTT = 0,
    FLUX_CAP_ROUND = 1,
    FLUX_CAP_SQUARE = 2
} flux_line_cap;
typedef enum flux_line_join {
    FLUX_JOIN_MITER = 0,
    FLUX_JOIN_ROUND = 1,
    FLUX_JOIN_BEVEL = 2
} flux_line_join;
typedef enum flux_fill_rule { FLUX_FILL_NON_ZERO = 0, FLUX_FILL_EVEN_ODD = 1 } flux_fill_rule;
typedef enum flux_blend_mode {
    FLUX_BLEND_SRC_OVER = 0,
    FLUX_BLEND_SRC = 1,
    FLUX_BLEND_PLUS = 2,
    FLUX_BLEND_MULTIPLY = 3
} flux_blend_mode;

/* Paint kind selects how the surface colour is computed.
 *   SOLID  - flat colour from `color`
 *   LINEAR - colour interpolated between stops along
 *            line (linear.from -> linear.to), positions in pixel space
 *   RADIAL - colour interpolated between stops by distance from
 *            radial.center, normalized by radial.radius (pixel space) */
typedef enum flux_paint_kind {
    FLUX_PAINT_SOLID = 0,
    FLUX_PAINT_LINEAR_GRADIENT = 1,
    FLUX_PAINT_RADIAL_GRADIENT = 2,
} flux_paint_kind;

#define FLUX_GRADIENT_MAX_STOPS 8

typedef struct flux_gradient_stop {
    float t;          /* [0, 1] along the gradient axis */
    flux_color color; /* premultiplied */
} flux_gradient_stop;

typedef struct flux_gradient_stops {
    uint32_t count;
    flux_gradient_stop stops[FLUX_GRADIENT_MAX_STOPS];
} flux_gradient_stops;

/* Discriminated by `kind`. The `gradient` union is meaningful only
 * for LINEAR/RADIAL — read it via flux_paint_linear_gradient /
 * flux_paint_radial_gradient (defined below) or directly. */
typedef struct flux_paint {
    flux_paint_kind kind;
    flux_color color; /* SOLID: fill colour; gradient kinds: ignored */

    /* Stroke parameters — apply to every stroke_path regardless of kind. */
    float stroke_width;
    float miter_limit;
    flux_line_cap cap;
    flux_line_join join;
    flux_fill_rule fill_rule;
    flux_blend_mode blend;

    /* Gradient parameters (pixel space, pre-transform). */
    union {
        struct {
            flux_point from;
            flux_point to;
            flux_gradient_stops stops;
        } linear;
        struct {
            flux_point center;
            float radius;
            flux_gradient_stops stops;
        } radial;
    } gradient;
} flux_paint;

FLUX_API flux_paint flux_paint_default(void);

/* Convenience constructors. All return a paint with sensible stroke
 * defaults (miter limit 4, cap butt, join miter, src-over). */
FLUX_API flux_paint flux_paint_solid(flux_color color);
FLUX_API flux_paint flux_paint_linear_gradient(flux_point from, flux_point to,
                                               const flux_gradient_stop *stops,
                                               uint32_t stop_count);
FLUX_API flux_paint flux_paint_radial_gradient(flux_point center, float radius,
                                               const flux_gradient_stop *stops,
                                               uint32_t stop_count);

/* ------------------------------------------------------------------ */
/*  Path (arena-owned)                                                */
/* ------------------------------------------------------------------ */

typedef struct flux_path flux_path; /* opaque-in-this-header; see implementation */

FLUX_NODISCARD FLUX_API flux_result flux_path_create(flux_path **out, flux_arena *arena);
FLUX_API void flux_path_move_to(flux_path *p, float x, float y);
FLUX_API void flux_path_line_to(flux_path *p, float x, float y);
FLUX_API void flux_path_quad_to(flux_path *p, float cx, float cy, float x, float y);
FLUX_API void flux_path_cubic_to(flux_path *p, float c1x, float c1y, float c2x, float c2y, float x,
                                 float y);
FLUX_API void flux_path_close(flux_path *p);
FLUX_API void flux_path_add_rect(flux_path *p, flux_rect r);
FLUX_API void flux_path_add_round_rect(flux_path *p, flux_rect r, float radius);
FLUX_API void flux_path_add_circle(flux_path *p, float cx, float cy, float radius);

/* Number of segments rejected due to arena exhaustion. Non-zero means
 * subsequent mutators silently dropped data. Path mutators are
 * void-returning (Cairo/Skia-style); this accessor lets a caller
 * detect overflow without consulting flux_get_last_error. */
FLUX_API uint32_t flux_path_dropped_count(const flux_path *p);

/* ------------------------------------------------------------------ */
/*  Image                                                             */
/* ------------------------------------------------------------------ */

typedef struct flux_image_desc {
    flux_struct_type type; /* FLUX_TYPE_IMAGE_DESC */
    const void *next;
    uint32_t width;
    uint32_t height;
    flux_format format;       /* must be an 8-bit colour format */
    const void *initial_data; /* optional; size = w*h*bytes_per_pixel */
} flux_image_desc;

#define FLUX_IMAGE_DESC_INIT {.type = FLUX_TYPE_IMAGE_DESC}

FLUX_NODISCARD FLUX_API flux_result flux_image_create(flux_device *d, const flux_image_desc *desc,
                                                      flux_image **out);

/* Create a render-target image for flux_canvas_begin_target (ADR-0017):
 * COLOR_ATTACHMENT | SAMPLED usage, 1 sample, with undefined initial contents.
 * The first target pass performs its initial layout transition in the frame;
 * after that pass finishes the image is sampleable. Released with
 * flux_image_release. */
FLUX_NODISCARD FLUX_API flux_result flux_image_create_render_target(flux_device *d, uint32_t width,
                                                                    uint32_t height,
                                                                    flux_format fmt,
                                                                    flux_image **out);
FLUX_NODISCARD FLUX_API flux_image *flux_image_retain(flux_image *i);
FLUX_API void flux_image_release(flux_image *i);
FLUX_API uint32_t flux_image_width(const flux_image *i);
FLUX_API uint32_t flux_image_height(const flux_image *i);
FLUX_API flux_format flux_image_format(const flux_image *i);

/* Transition a new or sampleable render-target image into colour-attachment
 * state for a caller-recorded pass, then restore it to sampleable state. Prepare before
 * flux_frame_begin_pass and finish immediately after flux_frame_end_pass.
 * The image must come from flux_image_create_render_target. */
FLUX_NODISCARD FLUX_API flux_result flux_frame_prepare_image_target(flux_frame *f,
                                                                    flux_image *target);
FLUX_NODISCARD FLUX_API flux_result flux_frame_finish_image_target(flux_frame *f,
                                                                   flux_image *target);

/* Upload `bytes` of `data` into a sub-region of an existing image.
 * The region must be in-bounds; `bytes` must be at least
 * w * h * bytes_per_pixel. The image is stalled briefly while the
 * upload completes (synchronous, one-shot command buffer). Bindless
 * handle and image view remain valid across the update. */
FLUX_NODISCARD FLUX_API flux_result flux_image_update_region(flux_image *image, uint32_t x,
                                                             uint32_t y, uint32_t w, uint32_t h,
                                                             const void *data, size_t bytes);

/* ------------------------------------------------------------------ */
/*  Canvas lifecycle + drawing                                        */
/* ------------------------------------------------------------------ */

/* Backend selection for flux_canvas_create (Skia SkSurface-style: the factory
 * binds the backend, then the same drawing API is used regardless). AUTO picks
 * GPU when a surface is provided, otherwise the software (CPU) backend. */
typedef enum flux_canvas_backend_kind {
    FLUX_CANVAS_BACKEND_AUTO = 0,
    FLUX_CANVAS_BACKEND_GPU = 1,
    FLUX_CANVAS_BACKEND_CPU = 2,
} flux_canvas_backend_kind;

typedef struct flux_canvas_desc {
    flux_struct_type type; /* FLUX_TYPE_CANVAS_DESC */
    const void *next;
    flux_surface *surface;            /* GPU: required (retained). CPU: ignored. */
    float scale;                      /* content scale (device-pixel ratio); 0 => 1.0.
                                         The canvas draws in logical units scaled onto
                                         the physical surface by this factor; flux_text
                                         reads it to rasterise glyphs crisply at HiDPI.
                                         Change later with flux_canvas_set_scale. */
    flux_canvas_backend_kind backend; /* default AUTO */
    uint32_t width, height;           /* CPU framebuffer size (physical px);
                                         ignored for the GPU backend. */
} flux_canvas_desc;

#define FLUX_CANVAS_DESC_INIT {.type = FLUX_TYPE_CANVAS_DESC}

/* GPU attachment-antialiasing policy for one Canvas pass. AUTO preserves the
 * pre-v0.0.5 behaviour: a clearing pass uses 4x MSAA and a loading pass
 * renders directly into its one-sample destination. NONE always uses the
 * one-sample GPU target. MSAA_4X requires a clear colour because a
 * multisample attachment cannot load a one-sample resolve destination. The
 * CPU backend accepts the policy and retains its native software rasterizer. */
typedef enum flux_canvas_antialias {
    FLUX_CANVAS_ANTIALIAS_AUTO = 0,
    FLUX_CANVAS_ANTIALIAS_NONE = 1,
    FLUX_CANVAS_ANTIALIAS_MSAA_4X = 2,
} flux_canvas_antialias;

typedef struct flux_canvas_pass_desc {
    flux_struct_type type; /* FLUX_TYPE_CANVAS_PASS_DESC */
    const void *next;
    const flux_color *clear_color; /* non-NULL: clear; NULL: load */
    flux_canvas_antialias antialias;
    /* Dirty rectangle for a partial-frame pass, in framebuffer pixels.
     * `render_width/height == 0` selects the full surface extent (legacy
     * behaviour); the offset then must also be zero. When non-zero the
     * dynamic-rendering renderArea is constrained to this rectangle, so a
     * `clear_color`-driven clear touches only the dirty region and the rest of
     * the image is preserved (the caller must arrange the surrounding image to
     * already hold valid contents). */
    int32_t render_offset_x;
    int32_t render_offset_y;
    uint32_t render_width;
    uint32_t render_height;
} flux_canvas_pass_desc;

#define FLUX_CANVAS_PASS_DESC_INIT {.type = FLUX_TYPE_CANVAS_PASS_DESC}

/* Create a canvas on the selected backend. GPU canvases need desc->surface;
 * CPU canvases need desc->width/height (see flux/canvas_cpu.h for a convenience
 * wrapper and pixel readback). Destroy with flux_canvas_destroy. */
FLUX_NODISCARD FLUX_API flux_result flux_canvas_create(const flux_canvas_desc *desc,
                                                       flux_canvas **out);
FLUX_API void flux_canvas_destroy(flux_canvas *c);

/* Content scale (device-pixel ratio). flux_canvas_set_scale makes each
 * flux_canvas_begin start with this as the base transform, so all drawing is
 * in logical units mapped onto the physical surface. flux_canvas_get_scale
 * returns the *effective* scale of the active transform (the base content
 * scale composed with any flux_canvas_scale on the stack); flux_text reads it
 * to rasterise glyphs at the device resolution. Set the content scale once
 * when the surface scale changes. */
FLUX_API void flux_canvas_set_scale(flux_canvas *c, float scale);
FLUX_API float flux_canvas_get_scale(const flux_canvas *c);

/* Begin / end a recording session. `f` is the open frame for a GPU canvas
 * (from flux_surface_begin_frame); for a CPU canvas pass NULL. This is the
 * unified, backend-agnostic pass bracket — the same drawing code runs on
 * either backend between begin_frame and end_frame.
 * clear_color: if non-NULL, the target is cleared to it; else loaded. */
FLUX_NODISCARD FLUX_API flux_result flux_canvas_begin_frame(flux_canvas *c, flux_frame *f,
                                                            const flux_color *clear_color);
FLUX_API void flux_canvas_end_frame(flux_canvas *c);

/* Descriptor form of flux_canvas_begin_frame. This makes attachment load
 * semantics and antialiasing independent: compositor/image-heavy passes can
 * clear a one-sample target without allocating and resolving a 4x attachment,
 * while vector UI keeps the AUTO default. */
FLUX_NODISCARD FLUX_API flux_result flux_canvas_begin_pass(flux_canvas *c, flux_frame *f,
                                                           const flux_canvas_pass_desc *desc);

/* GPU-specific spelling of begin_frame/end_frame (f is required). Kept for
 * source compatibility; equivalent to flux_canvas_begin_frame with a frame. */
FLUX_NODISCARD FLUX_API flux_result flux_canvas_begin(flux_canvas *c, flux_frame *f,
                                                      const flux_color *clear_color);
FLUX_API void flux_canvas_end(flux_canvas *c);

/* Snapshot the canvas' pixels as premultiplied RGBA8 (row-major; *stride is
 * bytes/row). Backend-polymorphic: implemented by the CPU backend (returns its
 * framebuffer); returns NULL on the GPU backend (use flux_canvas_begin_target
 * to render into a readable flux_image instead). width/height/stride are
 * optional out-params. The buffer is owned by the canvas. */
FLUX_API const uint8_t *flux_canvas_read_pixels(flux_canvas *c, uint32_t *width, uint32_t *height,
                                                uint32_t *stride);

/* Render the draws between begin_target/end_target into `target`
 * (a flux_image from flux_image_create_render_target) instead of the
 * frame's swapchain image. This is the capture seam (ADR-0017): the
 * captured image is a regular sampleable flux_image, so it can feed
 * flux_effect_blur and be drawn back via flux_canvas_draw_image.
 *
 * `target` must match the canvas's colour format. Its extent selects the
 * offscreen pass extent and may be smaller than the surface for effects such
 * as downsampled backdrop blur. The canvas transitions target to
 * COLOR_ATTACHMENT_OPTIMAL on begin and back to SHADER_READ_ONLY_OPTIMAL on
 * end, so the following effect or draw needs no caller-side synchronisation.
 * Requires an open frame (flux_surface_begin_frame) but NOT an open
 * canvas_begin session
 * — a capture typically runs before the frame pass. A target pass may
 * not be nested inside the frame's own canvas_begin/canvas_end. */
FLUX_NODISCARD FLUX_API flux_result flux_canvas_begin_target(flux_canvas *c, flux_frame *f,
                                                             flux_image *target,
                                                             const flux_color *clear_color);
FLUX_NODISCARD FLUX_API flux_result flux_canvas_begin_target_pass(
    flux_canvas *c, flux_frame *f, flux_image *target, const flux_canvas_pass_desc *desc);
FLUX_API void flux_canvas_end_target(flux_canvas *c);

/* State stack. All draws and state mutators between flux_canvas_begin
 * and flux_canvas_end record into the bound frame; calls outside that
 * window are silent no-ops (consult flux_get_last_error). */
FLUX_API void flux_canvas_save(flux_canvas *c);
FLUX_API void flux_canvas_restore(flux_canvas *c);
/* Intersect the current clip with `r`. The rectangle is expressed in the
 * current logical coordinate system and transformed to the physical-pixel
 * scissor, including content scale and affine transforms. */
FLUX_API void flux_canvas_clip_rect(flux_canvas *c, flux_rect r);
FLUX_API void flux_canvas_translate(flux_canvas *c, float x, float y);
FLUX_API void flux_canvas_scale(flux_canvas *c, float sx, float sy);
FLUX_API void flux_canvas_rotate(flux_canvas *c, float radians);
FLUX_API void flux_canvas_transform(flux_canvas *c, flux_mat3x2 m);

/* Drawing. */
FLUX_API void flux_canvas_fill_rect(flux_canvas *c, flux_rect r, const flux_paint *paint);
FLUX_API void flux_canvas_fill_path(flux_canvas *c, const flux_path *p, const flux_paint *paint);
FLUX_API void flux_canvas_stroke_path(flux_canvas *c, const flux_path *p, const flux_paint *paint);

/* Rounded rectangles and circles via a signed-distance field: analytic,
 * resolution-independent anti-aliasing (crisp at any DPI, unlike tessellated
 * fills which rely on MSAA). `radius` is the corner radius in logical pixels,
 * clamped to half the shorter side; pass radius == min(w,h)/2 on a square for
 * a circle. Evaluated for axis-aligned rects under translation + uniform
 * scale (the UI case); rotation is not modelled. `color` is a packed
 * premultiplied flux_color. Prefer these over fill_path for UI shapes. */
FLUX_API void flux_canvas_fill_rrect(flux_canvas *c, flux_rect r, float radius, flux_color color);
FLUX_API void flux_canvas_stroke_rrect(flux_canvas *c, flux_rect r, float radius, flux_color color,
                                       float width);
/* Draw an RGBA image. When `optional_paint` is non-NULL, its premultiplied
 * solid `color` modulates the image; opaque white preserves it and white with
 * a lower alpha fades it. Other paint fields are currently ignored. Canvas
 * transforms, including rotation and non-uniform scale, apply to the quad. */
FLUX_API void flux_canvas_draw_image(flux_canvas *c, flux_image *image, flux_rect dst,
                                     const flux_paint *optional_paint);

/* Draw a sub-rectangle of `image` into `dst`. `src` is the sampled region
 * in NORMALISED texture coordinates {u, v, du, dv} where (0,0,1,1) is the
 * whole image. Used by compositors that need source-crop (Wayland
 * `wp_viewport.set_source`) without tinting or coverage-style alpha
 * handling. The plain-image pipeline already implements full sub-rect
 * remap in the fragment shader; this entry point only exposes it. */
FLUX_API void flux_canvas_draw_image_sub(flux_canvas *c, flux_image *image, flux_rect dst,
                                         flux_rect src);

/* Sample `image` with `sampler` instead of the canvas-internal linear
 * default. Pass FLUX_FILTER_NEAREST samplers for pixel-aligned blits
 * (e.g. glyph atlases) where the default bilinear filter blurs
 * sub-pixel positions. `sampler` is borrowed for the call; the caller
 * retains ownership and must keep it alive until the frame is
 * submitted. */
FLUX_API void flux_canvas_draw_image_sampled(flux_canvas *c, flux_image *image,
                                             flux_sampler *sampler, flux_rect dst,
                                             const flux_paint *optional_paint);

/* Convenience: fill a rectangle with a solid colour, no paint setup. */
FLUX_API void flux_canvas_fill_rect_color(flux_canvas *c, flux_rect r, flux_color color);

/* Draw `image` as a single-channel coverage glyph: the texture's R
 * channel is treated as alpha coverage and multiplied by `tint` (a
 * premultiplied flux_color). This lets one colour-independent R8
 * coverage texture be drawn in any colour, so glyph textures need not be
 * duplicated or re-uploaded per text run. */
FLUX_API void flux_canvas_draw_image_coverage(flux_canvas *c, flux_image *image, flux_rect dst,
                                              flux_color tint);

/* Coverage glyph from a sub-rectangle of `image` (a glyph atlas). `src` is
 * the sampled region in NORMALISED texture coordinates {u, v, du, dv}; `dst`
 * is the destination pixel rect. Identical to flux_canvas_draw_image_coverage
 * with src = {0,0,1,1}. One persistent atlas texture can thus back every glyph
 * — glyphs are uploaded once and reused, never rebuilt per text run. */
FLUX_API void flux_canvas_draw_image_coverage_sub(flux_canvas *c, flux_image *image, flux_rect dst,
                                                  flux_rect src, flux_color tint);

/* ------------------------------------------------------------------ */
/*  Glyph runs (ADR-0010)                                             */
/* ------------------------------------------------------------------ */

/* One glyph blit: a screen-space rectangle sampled from a texel
 * sub-rect of the run's atlas, tinted by a premultiplied colour.
 * Maps 1:1 onto a shaped glyph (e.g. one HarfBuzz glyph position). */
typedef struct flux_glyph_quad {
    float sx, sy;     /* screen-space top-left (canvas transform applies) */
    float sw, sh;     /* destination size in pixels */
    uint16_t ax, ay;  /* atlas top-left, texels */
    uint16_t aw, ah;  /* atlas extent,   texels */
    flux_color color; /* premultiplied per-glyph tint */
} flux_glyph_quad;

typedef struct flux_glyph_run_desc {
    flux_struct_type type; /* FLUX_TYPE_GLYPH_RUN_DESC */
    const void *next;
    flux_image *atlas; /* caller-owned; R8 coverage (.r × tint).
                        * NULL on a device-less CPU canvas — set
                        * host_coverage instead. */
    /* Host-resident R8 coverage atlas, the CPU-backend alternative to
     * `atlas` (ADR-0019): when non-NULL the CPU rasteriser samples
     * coverage straight from this buffer with no GPU image, so a
     * device-less canvas (flux_canvas_create_cpu) can render glyph runs.
     * Ignored when a device/atlas is present. `host_atlas_w/h` are the
     * buffer's texel extent (0 when unused). */
    const uint8_t *host_coverage;
    uint32_t host_atlas_w;
    uint32_t host_atlas_h;
    flux_sampler *sampler; /* optional; NULL = canvas default (linear).
                            * Pass a NEAREST sampler for crisp blits. */
    const flux_glyph_quad *quads;
    uint32_t quad_count;
} flux_glyph_run_desc;

#define FLUX_GLYPH_RUN_DESC_INIT {.type = FLUX_TYPE_GLYPH_RUN_DESC}

/* Draw a pre-shaped glyph run as a single batched draw call. The
 * library does no shaping, kerning, or atlas management (ADR-0010):
 * quads arrive positioned, the atlas is caller-owned and updated via
 * flux_image_update_region. The atlas's R channel is alpha coverage
 * multiplied by each quad's premultiplied tint, blended SRC_OVER —
 * the same contract as flux_canvas_draw_image_coverage, minus the
 * per-glyph draw-call cost.
 *
 * On a device-less CPU canvas (flux_canvas_create_cpu) there is no
 * GPU image to bind, so `atlas` is ignored and `host_coverage` is used
 * instead: a host-resident R8 coverage buffer of host_atlas_w×host_atlas_h
 * texels (ADR-0019). The CPU rasteriser samples coverage directly from it. */
FLUX_API void flux_canvas_draw_glyph_run(flux_canvas *c, const flux_glyph_run_desc *desc);

/* Cumulative count of draw calls dropped due to transient ring exhaustion
 * since canvas creation. Non-zero means the per-frame transient ring is too
 * small for the workload; check flux_frame_alloc_transient or increase the
 * ring size. */
FLUX_API uint64_t flux_canvas_dropped_draws(const flux_canvas *c);

/* Vulkan batching diagnostics, cumulative since canvas creation.
 * `submit_calls` counts front-end batches handed to the backend;
 * `recorded_draws` counts vkCmdDraw commands after consecutive compatible
 * submits are merged. Both are zero for the CPU backend. */
FLUX_API uint64_t flux_canvas_submit_calls(const flux_canvas *c);
FLUX_API uint64_t flux_canvas_recorded_draws(const flux_canvas *c);

/* ------------------------------------------------------------------ */
/*  Display-list segments (record / replay)                           */
/* ------------------------------------------------------------------ */

/* Opaque handle to a recorded draw segment. The segment is owned by the
 * canvas (fixed slot pool); the handle is a {slot, generation} pair so a
 * stale handle — released, LRU-evicted, or from another canvas — fails
 * validation instead of replaying the wrong draws. Zero-initialise for
 * the null record. */
typedef struct flux_canvas_record {
    void *slot;
    uint64_t generation;
} flux_canvas_record;

#define FLUX_CANVAS_RECORD_INIT {.slot = NULL, .generation = 0}

/* Start capturing every draw submitted between here and the matching
 * flux_canvas_end_record into a new segment. Recording is passive: draws
 * are still submitted live, so a recorded frame renders exactly as an
 * unrecorded one. Recordings nest (e.g. a re-recording parent subtree
 * around a recording child); every active recording captures every draw.
 * Returns false (and records nothing) when called outside
 * begin_frame/end_frame or when the nesting-depth cap is hit — the caller
 * then simply draws without recording and must not call end_record. */
FLUX_API bool flux_canvas_begin_record(flux_canvas *c);

/* Close the innermost recording and return its handle (null on budget
 * overflow or allocation failure — the live draws still happened, only
 * the recording is lost). The segment stays valid until
 * flux_canvas_record_release, LRU eviction under the canvas-wide byte
 * budget, or canvas destruction. */
FLUX_API flux_canvas_record flux_canvas_end_record(flux_canvas *c);

/* Re-submit a recorded segment. Replays only when the canvas state still
 * matches the recording exactly — same framebuffer extent, same absolute
 * transform, same incoming scissor — because vertex positions and push
 * constants are baked in physical pixels at record time. On any mismatch
 * (moved/scaled content, changed clip, resized target) nothing is drawn
 * and false is returned; the caller should re-emit and re-record. A
 * replay inside an active recording is itself captured, so a
 * re-recording ancestor stays complete when an unchanged child replays.
 * Images and custom samplers referenced by the segment are retained by
 * the canvas for the segment's lifetime. */
FLUX_API bool flux_canvas_replay(flux_canvas *c, flux_canvas_record rec);

/* Release a segment early (e.g. its subtree changed or died). Safe on a
 * null or stale handle. Unreleased segments are reclaimed by the canvas's
 * LRU byte budget and at canvas destruction. */
FLUX_API void flux_canvas_record_release(flux_canvas *c, flux_canvas_record rec);

/* Diagnostics: cumulative successful end_record / replay counts since
 * canvas creation. Tests diff these across a frame to assert that a
 * static frame replays and a changed frame re-records. */
FLUX_API uint64_t flux_canvas_records_created(const flux_canvas *c);
FLUX_API uint64_t flux_canvas_records_replayed(const flux_canvas *c);

#ifdef __cplusplus
}
#endif

#endif /* FLUX_CANVAS_H */
