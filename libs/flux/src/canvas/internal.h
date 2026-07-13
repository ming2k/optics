/*
 * Internal canvas layout. Never installed.
 */
#ifndef FLUX_CANVAS_INTERNAL_H
#define FLUX_CANVAS_INTERNAL_H

#include "../core/internal.h"
#include <flux/canvas.h>
#include <flux/vulkan.h>

#define FLUX_CANVAS_MAX_STATES 32
#define FLUX_CANVAS_PATH_SCRATCH_CAP 2048
#define FLUX_CANVAS_MAX_CONTOURS 64

/* Multisample count for canvas rendering. Vector fills (rounded rects,
 * circles, arbitrary paths) have no per-fragment coverage AA, so the canvas
 * renders to a 4x MSAA target and resolves to the surface. 4x is supported
 * everywhere and is the standard quality/cost point for 2D UI. The pipeline
 * sample count (renderer.c) and the owned color + stencil images must all
 * agree on this value. */
#define FLUX_CANVAS_SAMPLES VK_SAMPLE_COUNT_4_BIT

/* Integer clip rectangle in physical pixels. Backend-neutral (the Vulkan
 * backend converts it to VkRect2D at scissor time). */
typedef struct flux_recti {
    int32_t x, y;
    uint32_t w, h;
} flux_recti;

typedef struct flux_canvas_state {
    flux_mat3x2 transform;
    flux_recti scissor;
} flux_canvas_state;

/* Rendering backend vtable (defined in backend.h). The canvas holds a
 * borrowed pointer to a stateless singleton; see backend_vk.c. */
typedef struct flux_canvas_backend flux_canvas_backend;

/* Forward declared so flux_canvas can hold a pointer-array to it
 * without depending on geometry layout. */
typedef struct flux_canvas_contour flux_canvas_contour;
typedef struct stroke_frame stroke_frame;

/* Forward declaration so we can hold pointers without dragging in
 * the (intentionally private) vertex layout from canvas.c. */
typedef struct flux_canvas_vertex flux_canvas_vertex;

struct flux_canvas {
    atomic_uint ref_count;
    flux_device *device;   /* retained */
    flux_surface *surface; /* retained */

    /* Rendering backend (borrowed singleton) plus its private per-canvas
     * state. All GPU resources (pipelines, MSAA/stencil targets, formats)
     * live in backend_data; the front end never dereferences it. See
     * backend.h / backend_vk.c. */
    const flux_canvas_backend *backend;
    void *backend_data;

    /* Active recording state */
    flux_frame *frame;
    bool recording;
    bool pass_active;
    bool target_pass;   /* true: active pass renders into target, not the frame */
    flux_image *target; /* borrowed during begin_target..end_target */

    /* Stencil-then-cover availability (ADR-0014). Set by the backend at
     * begin_pass: true when a stencil attachment is present this pass, so
     * self-intersecting fills may use the stencil fallback; false keeps the
     * old bail-out behaviour. Replaces a direct peek at the GPU stencil view. */
    bool stencil_available;

    /* Physical framebuffer size of the active pass, set by the backend at
     * begin_pass. build_push reads it for the NDC transform, so the front end
     * needs no surface handle while recording (the CPU backend has none). */
    uint32_t fb_width, fb_height;

    /* Content scale (device-pixel ratio). The base transform at index 0 is
     * this scale, so callers draw in logical units; 1.0 means logical ==
     * physical. flux_text reads it via flux_canvas_get_scale. */
    float content_scale;

    /* Transform / clip stack. Index 0 is the content-scale base transform. */
    flux_canvas_state states[FLUX_CANVAS_MAX_STATES];
    uint32_t state_top;

    /* Per-instance scratch buffers for fill/stroke. Moved off any
     * static / thread_local area so two canvases on two threads can
     * record concurrently without corrupting each other. */
    flux_point *scratch_pts;               /* FLUX_CANVAS_PATH_SCRATCH_CAP */
    flux_canvas_vertex *scratch_verts;     /* FLUX_CANVAS_PATH_SCRATCH_CAP * 3 */
    flux_canvas_contour *scratch_contours; /* FLUX_CANVAS_MAX_CONTOURS */
    uint32_t *scratch_lnk_prev;            /* FLUX_CANVAS_PATH_SCRATCH_CAP */
    uint32_t *scratch_lnk_next;            /* FLUX_CANVAS_PATH_SCRATCH_CAP */
    stroke_frame *scratch_frames;          /* FLUX_CANVAS_PATH_SCRATCH_CAP */

    /* Diagnostics: cumulative count of draw calls dropped due to
     * transient ring exhaustion. Reset to 0 at canvas creation. */
    uint64_t dropped_draws;

    /* Transient host-resident glyph atlas for the CPU backend
     * (ADR-0019). draw_glyph_run sets this right before emitting GLYPH
     * batches so cpu_submit can sample coverage from a host R8 buffer on
     * a device-less canvas; it is cleared after the run and ignored by
     * the Vulkan backend. NULL outside a host glyph run. */
    const uint8_t *pending_host_atlas;
    uint32_t pending_host_atlas_w;
    uint32_t pending_host_atlas_h;
};

/* Vertex layout matches std430 buffer_reference in
 * src/canvas/shaders/canvas_solid.vert. The forward declaration above
 * uses this same name; the struct body is defined here. */
struct flux_canvas_vertex {
    float pos[2];
    uint32_t color;
    uint32_t _pad;
};

/* Push constants for the canvas pipelines. The solid pipeline only
 * touches the first 24 bytes (the rest is uninitialised but harmless).
 * The gradient pipeline reads everything. Layout must match the
 * std140-style block in src/canvas/shaders/canvas_gradient.frag. */
typedef struct flux_canvas_gradient_stop_pc {
    float t;
    uint32_t color;
} flux_canvas_gradient_stop_pc;

typedef struct flux_canvas_push {
    uint64_t verts_address;   /* offset  0 */
    float inv_window_size[2]; /* offset  8 */
    float _pad0[2];           /* offset 16 */
    uint32_t kind;            /* offset 24 (0 solid, 1 linear, 2 radial, 3 image) */
    uint32_t num_stops;       /* offset 28 */
    float grad_from[2];       /* offset 32 */
    float grad_to[2];         /* offset 40 */
    float grad_radius;        /* offset 48 */
    float _pad1;              /* offset 52 */
    flux_canvas_gradient_stop_pc stops[FLUX_GRADIENT_MAX_STOPS]; /* offset 56, size 64 */
    /* Image draw extension (read by the image fragment shader).
     * image_dst maps v_pos (pixel space) to UV in [0,1]:
     *   uv = (v_pos - image_dst.xy) / image_dst.zw */
    uint32_t image_handle;   /* offset 120 */
    uint32_t sampler_handle; /* offset 124 */
    float image_dst[4];      /* offset 128 — x, y, w, h pre-transform */
    /* image_src selects the sampled sub-rectangle in NORMALISED texture
     * coordinates (u, v, du, dv). The local [0,1] coverage of image_dst is
     * remapped into it: uv = image_src.xy + clamp(local) * image_src.zw.
     * For whole-image draws this is {0,0,1,1}; a glyph atlas passes the
     * glyph's sub-rect so one texture serves every glyph. */
    float image_src[4]; /* offset 144 — u, v, du, dv (normalised) */
} flux_canvas_push;

/* ------------------------------------------------------------------ */
/*  Image (internal)                                                  */
/*                                                                      */
/*  struct flux_image + flux_image_create_compute_writable live in     */
/*  image_internal.h so the effect module can depend on the image      */
/*  struct without dragging in canvas pipelines, push constants,       */
/*  geometry helpers, or path/paint internals.                         */
/* ------------------------------------------------------------------ */

#include "image_internal.h"

/* ------------------------------------------------------------------ */
/*  Path                                                              */
/* ------------------------------------------------------------------ */

typedef enum flux_path_op {
    FLUX_PATH_MOVE = 0,
    FLUX_PATH_LINE = 1,
    FLUX_PATH_QUAD = 2,
    FLUX_PATH_CUBIC = 3,
    FLUX_PATH_CLOSE = 4,
} flux_path_op;

typedef struct flux_path_segment {
    uint32_t op;  /* flux_path_op */
    float pts[6]; /* up to a cubic's 3 control points */
} flux_path_segment;

struct flux_path {
    flux_path_segment *segments;
    uint32_t capacity;
    uint32_t count;
    uint32_t dropped;
    float cursor_x;
    float cursor_y;
    flux_arena *arena;
};

/* ------------------------------------------------------------------ */
/*  Renderer (pipeline cache + draw submission)                       */
/* ------------------------------------------------------------------ */

/* Internal pipeline selector. SOLID/GRADIENT/IMAGE are the normal
 * color pipelines; STENCIL_WRITE accumulates nonzero winding into the
 * stencil attachment (color writes off); COVER_* draw the paint where
 * stencil != 0 and reset it to 0 (ADR-0014). */
typedef enum canvas_pipe_id {
    CANVAS_PIPE_SOLID = 0,
    CANVAS_PIPE_GRADIENT,
    CANVAS_PIPE_IMAGE,
    CANVAS_PIPE_STENCIL_WRITE,
    CANVAS_PIPE_COVER_SOLID,
    CANVAS_PIPE_COVER_GRADIENT,
    CANVAS_PIPE_GLYPH, /* batched glyph run (ADR-0010) */
    CANVAS_PIPE_SDF,   /* analytic rounded-rect / circle (fill + ring) */
    CANVAS_PIPE_COUNT,
} canvas_pipe_id;

flux_result get_canvas_pipeline(flux_device *device, VkFormat color_format,
                                VkSampleCountFlagBits samples, flux_paint_kind kind,
                                VkPipelineLayout *out_layout, VkPipeline *out_pipeline);
flux_result get_canvas_pipeline_id(flux_device *device, VkFormat color_format,
                                   VkSampleCountFlagBits samples, canvas_pipe_id id,
                                   VkPipelineLayout *out_layout, VkPipeline *out_pipeline);

/* Stencil format every canvas pipeline (and the canvas's stencil
 * attachment) uses on this device. Probed once: S8_UINT preferred,
 * combined depth-stencil fallbacks; VK_FORMAT_UNDEFINED when nothing
 * supports DEPTH_STENCIL_ATTACHMENT in optimal tiling. */
VkFormat flux_canvas_stencil_format(flux_device *d);

void *canvas_state_get_or_init(flux_device *d);

/* Allocation shims + scratch lifecycle shared by the GPU and CPU canvas
 * constructors (canvas.c). A NULL device routes to the C allocator. */
void *flux_canvas_alloc(flux_device *d, size_t n);
void flux_canvas_free(flux_device *d, void *p);
bool flux_canvas_alloc_scratch(flux_canvas *c);
void flux_canvas_free_scratch(flux_canvas *c);

void push_vertex(flux_canvas_vertex *v, flux_point p, flux_mat3x2 tx, flux_color c);
void build_push(flux_canvas *c, const flux_paint *paint, flux_canvas_push *out);
bool ensure_pipeline_bound(flux_canvas *c, flux_paint_kind kind);
bool ensure_pipeline_bound_id(flux_canvas *c, canvas_pipe_id id);
void submit_triangles(flux_canvas *c, const flux_paint *paint, const flux_canvas_vertex *verts,
                      uint32_t vertex_count);
void submit_triangles_id(flux_canvas *c, const flux_paint *paint, canvas_pipe_id id,
                         const flux_canvas_vertex *verts, uint32_t vertex_count);

/* ------------------------------------------------------------------ */
/*  Flattening                                                        */
/* ------------------------------------------------------------------ */

typedef struct flux_canvas_contour {
    uint32_t start; /* index in the points array */
    uint32_t count;
    bool closed;
    float first_x, first_y;
} flux_canvas_contour;

typedef struct flatten_multi {
    uint32_t point_count;
    uint32_t contour_count;
} flatten_multi;

flatten_multi flatten_path_to_contours(const flux_path *p, float pixel_scale, flux_point *out_pts,
                                       uint32_t pts_cap, flux_canvas_contour *out_cons,
                                       uint32_t cons_cap);

/* Operator-norm upper bound of `m`'s 2×2 linear part. Callers pass
 * the result as `pixel_scale` to the flatteners so the per-pixel
 * tolerance survives non-identity transforms. */
float flux_canvas_mat3x2_pixel_scale(flux_mat3x2 m);

/* ------------------------------------------------------------------ */
/*  Tessellation                                                      */
/* ------------------------------------------------------------------ */

float signed_area(const flux_point *pts, uint32_t n);

/* Returns false when the clip stalled (self-intersecting input
 * tripped the bounded-step guard) — the emitted triangles are then
 * incomplete and the caller should fall back to stencil-then-cover
 * (ADR-0014). */
bool ear_clip_contour(flux_canvas_vertex *verts, uint32_t *v_count, uint32_t verts_cap,
                      flux_mat3x2 tx, flux_color color, flux_point *pts, uint32_t *prev,
                      uint32_t *next, uint32_t n);

/* ------------------------------------------------------------------ */
/*  Stroke                                                            */
/* ------------------------------------------------------------------ */

typedef struct stroke_frame {
    flux_point left;   /* offset by +half along the bisector (or n_in for endpoints) */
    flux_point right;  /* offset by -half */
    flux_point n_in;   /* left normal of incoming segment */
    flux_point n_out;  /* left normal of outgoing segment */
    bool miter_ok;     /* true → left/right are a single bisector point */
    bool turning_left; /* outside of the turn is on the left side */
} stroke_frame;

stroke_frame compute_frame(flux_point prev, flux_point pt, flux_point next, float half,
                           float miter_limit, bool has_prev, bool has_next);

void emit_arc(flux_canvas_vertex *verts, uint32_t *count, uint32_t cap, flux_mat3x2 tx,
              flux_color color, flux_point centre, float radius, float start_angle, float sweep,
              uint32_t steps);

/* ------------------------------------------------------------------ */
/*  Point helpers                                                     */
/* ------------------------------------------------------------------ */

static inline flux_point pt_add(flux_point a, flux_point b) {
    return (flux_point){a.x + b.x, a.y + b.y};
}
static inline flux_point pt_sub(flux_point a, flux_point b) {
    return (flux_point){a.x - b.x, a.y - b.y};
}
static inline flux_point pt_scale(flux_point a, float k) {
    return (flux_point){a.x * k, a.y * k};
}

/* ------------------------------------------------------------------ */
/*  Vertex helpers (defined in canvas.c)                              */
/* ------------------------------------------------------------------ */

void emit_tri(flux_canvas_vertex *verts, uint32_t *count, uint32_t cap, flux_mat3x2 tx,
              flux_color color, flux_point a, flux_point b, flux_point e);

#endif /* FLUX_CANVAS_INTERNAL_H */
