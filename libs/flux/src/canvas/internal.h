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

/* ADR-0069: the canvas always renders into this working-space
 * intermediate (linear light, scRGB scale); the output transform
 * pass converts to the destination's color space. */
#define FLUX_CANVAS_LINEAR_FORMAT VK_FORMAT_R16G16B16A16_SFLOAT

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

/* Path verb stream. Kept above struct flux_canvas so the tessellation
 * cache can embed a copy of the stream in its entries. The full
 * struct flux_path stays in the Path section below. */
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

/* ------------------------------------------------------------------ */
/*  Display-list segments (record / replay)                           */
/*                                                                    */
/*  A segment captures every backend submit (pipeline id, push        */
/*  constants, scissor, blend, vertex bytes) emitted between          */
/*  flux_canvas_begin_record / flux_canvas_end_record so an unchanged */
/*  subtree can be re-submitted later without re-running the emitter  */
/*  (text shaping, tessellation, measure). Recording is passive: the  */
/*  draws are submitted live as usual while being captured.           */
/*                                                                    */
/*  Ownership: slots live in a fixed pool owned by the canvas, freed  */
/*  at flux_canvas_destroy. The public handle is a {slot, generation} */
/*  pair; release or LRU eviction bumps the generation so stale       */
/*  handles fail validation instead of replaying a reused slot. Total */
/*  recorded bytes are capped (LRU eviction), as is each segment.     */
/* ------------------------------------------------------------------ */

#define FLUX_CANVAS_RECORD_DEPTH_CAP 16u              /* nested recordings  */
#define FLUX_CANVAS_RECORD_SLOT_CAP 1024u             /* pool size          */
#define FLUX_CANVAS_RECORD_TOTAL_BUDGET (16u << 20)   /* bytes, all segs    */
#define FLUX_CANVAS_RECORD_SEG_BUDGET (2u << 20)      /* bytes, one segment */
#define FLUX_CANVAS_RECORD_IMG_CAP 32u                /* retained images    */
#define FLUX_CANVAS_RECORD_SAMPLER_CAP 16u            /* retained samplers  */

/* Host-atlas generation registry size (one entry per live producer) and
 * the sentinel recorded when a glyph run carried no generation
 * extension — such ops replay unchecked, matching producers that never
 * rearrange their buffer in place. */
#define FLUX_CANVAS_HOST_ATLAS_GEN_CAP 8u
#define FLUX_HOST_ATLAS_UNVERSIONED UINT64_MAX

typedef struct flux_host_atlas_gen {
    const uint8_t *ptr;
    uint64_t gen;
} flux_host_atlas_gen;

enum flux_record_slot_state {
    FLUX_RECORD_SLOT_FREE = 0,
    FLUX_RECORD_SLOT_RECORDING,
    FLUX_RECORD_SLOT_VALID,
    FLUX_RECORD_SLOT_POISONED, /* recording aborted (budget/alloc); end_record discards */
};

/* Full definitions sit below flux_canvas_push (flux_record_op embeds it).
 * struct flux_canvas only holds pointers to these. */
typedef struct flux_record_op flux_record_op;
typedef struct flux_canvas_record_slot flux_canvas_record_slot;

/* ------------------------------------------------------------------ */
/*  Tessellation result cache                                         */
/*                                                                    */
/*  fill_path / stroke_path re-flatten (recursive cubic subdivision), */
/*  classify, and ear-clip the same paths every frame — for icons on  */
/*  a static UI this is pure waste. The cache keeps the resulting     */
/*  path-space triangle soup (pre canvas-transform) keyed by the full */
/*  verb stream plus every parameter the tessellation depends on      */
/*  (pixel_scale drives the flatten tolerance; stroke width/cap/join/ */
/*  miter drive the outline). A hit only re-transforms and submits    */
/*  the cached vertices, skipping flatten + ear-clip entirely.        */
/*                                                                    */
/*  Storage is inline in each entry: no heap churn, and struct        */
/*  flux_canvas is zero-initialised at create so all entries start    */
/*  invalid with nothing to free at destroy. Paths whose verb stream  */
/*  or triangle output exceed the inline caps bypass the cache.       */
/*  Stalled ear-clips, EVEN_ODD fills, and the stencil fallback are   */
/*  never cached — their output is not a pure function of the key.    */
/* ------------------------------------------------------------------ */

#define FLUX_TESS_CACHE_CAP 32u         /* bounded LRU entries per canvas */
#define FLUX_TESS_CACHE_MAX_SEGS 128u   /* verb-stream copy cap           */
#define FLUX_TESS_CACHE_MAX_VERTS 1024u /* path-space vertex cap        */

typedef struct flux_tess_cache_entry {
    uint64_t hash;      /* FNV-1a over the verb stream + parameters */
    uint64_t last_used; /* LRU tick; 0 == invalid                   */
    uint32_t seg_count;
    uint32_t vert_count;
    float pixel_scale;  /* flatten tolerance input           */
    float stroke_width; /* stroke entries; 0 on fills        */
    float miter_limit;  /* effective limit (default 4 baked) */
    uint32_t is_stroke;
    uint32_t cap;       /* flux_line_cap   (stroke) */
    uint32_t join;      /* flux_line_join  (stroke) */
    uint32_t fill_rule; /* flux_fill_rule  (fill)   */
    /* Secondary full-stream compare guards against hash collisions. */
    flux_path_segment segs[FLUX_TESS_CACHE_MAX_SEGS];
    flux_point verts[FLUX_TESS_CACHE_MAX_VERTS]; /* path space */
} flux_tess_cache_entry;

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

    /* True only for an explicitly requested no-stencil pass. Geometry that
     * cannot be represented without the stencil-then-cover fallback must
     * diagnose and drop the draw instead of silently substituting different
     * fill semantics. CPU and Vulkan backends set this identically. */
    bool stencil_forbidden;

    /* Sticky error for the active pass. Void draw calls cannot return a
     * stencil-contract violation directly, so checked pass termination exposes
     * it without relying on thread-local diagnostic polling. First error wins. */
    flux_result pass_error;

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

    /* Tessellation result cache (see above). Self-contained inline
     * storage; zero-initialised with the canvas, no destroy cleanup. */
    flux_tess_cache_entry tess_cache[FLUX_TESS_CACHE_CAP];
    uint64_t tess_cache_tick;
    /* Diagnostics: hit/miss/store counters, cumulative since create. */
    uint64_t tess_cache_hits, tess_cache_misses, tess_cache_stores;

    /* Diagnostics: cumulative count of draw calls dropped due to
     * transient ring exhaustion. Reset to 0 at canvas creation. */
    uint64_t dropped_draws;

    /* Diagnostics (Vulkan backend only): cumulative submit() calls vs
     * draw commands actually recorded. Consecutive submits with
     * identical pipeline/scissor/push constants batch into a single
     * vkCmdDraw, so recorded_draws <= submit_calls; the gap measures
     * batching efficiency. Both stay 0 on the CPU backend (1:1, no
     * command buffer). Reset to 0 at canvas creation. */
    uint64_t submit_calls, recorded_draws;

    /* Transient host-resident glyph atlas for the CPU backend
     * (ADR-0019). draw_glyph_run sets this right before emitting GLYPH
     * batches so cpu_submit can sample coverage from a host R8 buffer on
     * a device-less canvas; it is cleared after the run and ignored by
     * the Vulkan backend. NULL outside a host glyph run. */
    const uint8_t *pending_host_atlas;
    uint32_t pending_host_atlas_w;
    uint32_t pending_host_atlas_h;
    /* Producer content generation for pending_host_atlas, from the
     * flux_glyph_run_host_atlas_desc extension; FLUX_HOST_ATLAS_UNVERSIONED
     * when the run carried none. Captured into record ops so replay can
     * refuse segments whose baked UVs predate an in-place rearrange. */
    uint64_t pending_host_atlas_gen;

    /* Newest content generation seen per host atlas buffer (see
     * flux_glyph_run_host_atlas_desc). draw_glyph_run refreshes an entry,
     * flux_canvas_replay refuses a segment whose recorded generation is
     * older. Fixed-size, round-robin: one entry per live producer. */
    flux_host_atlas_gen host_atlas_gens[FLUX_CANVAS_HOST_ATLAS_GEN_CAP];
    uint32_t host_atlas_gen_next; /* round-robin insert slot */

    /* Active blend mode for the next submit, set by submit_triangles*
     * from the paint (ADR: canvas blend modes). The GPU backend uses
     * it to key into the per-blend pipeline cache; the CPU backend
     * uses it to pick the compositing routine. STENCIL_WRITE ignores
     * it (color writes are off). Defaults to SRC_OVER. */
    flux_blend_mode pending_blend;

    /* Display-list segments (see "Display-list segments" above). The
     * slot pool is allocated lazily on the first flux_canvas_begin_record
     * and freed at destroy; the stack tracks nested recordings. */
    flux_canvas_record_slot *record_slots;
    flux_canvas_record_slot *record_stack[FLUX_CANVAS_RECORD_DEPTH_CAP];
    uint32_t record_depth;
    uint64_t record_tick; /* LRU clock */
    size_t record_bytes;  /* total bytes across VALID segments */
    /* Diagnostics: cumulative successful end_record / replay counts. */
    uint64_t records_created, records_replayed;
};

/* Internal, ABI-independent pass policy. The public descriptor's pNext chain
 * is parsed exactly once by canvas.c; backends consume this normalized value
 * and never inspect extension memory themselves. */
typedef struct canvas_pass_config {
    const flux_color *clear_color;
    flux_canvas_antialias antialias;
    int32_t render_offset_x;
    int32_t render_offset_y;
    uint32_t render_width;
    uint32_t render_height;
    bool skip_stencil;
} canvas_pass_config;

/* Resolve the public zero-means-full render-area convention against one
 * concrete framebuffer. Both backends use this so CPU and Vulkan reject the
 * same malformed/out-of-bounds descriptors and begin with the same scissor. */
static inline bool canvas_pass_render_area(const canvas_pass_config *config, uint32_t fb_width,
                                           uint32_t fb_height, flux_recti *out) {
    if (!config || !out || fb_width == 0 || fb_height == 0)
        return false;
    bool default_extent = config->render_width == 0 && config->render_height == 0;
    if (default_extent) {
        if (config->render_offset_x != 0 || config->render_offset_y != 0)
            return false;
        *out = (flux_recti){0, 0, fb_width, fb_height};
        return true;
    }
    if (config->render_width == 0 || config->render_height == 0 || config->render_offset_x < 0 ||
        config->render_offset_y < 0)
        return false;
    uint32_t x = (uint32_t)config->render_offset_x;
    uint32_t y = (uint32_t)config->render_offset_y;
    if (x >= fb_width || y >= fb_height || config->render_width > fb_width - x ||
        config->render_height > fb_height - y)
        return false;
    *out = (flux_recti){config->render_offset_x, config->render_offset_y, config->render_width,
                        config->render_height};
    return true;
}

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
    uint64_t color_params_address; /* offset 16 — flux_image_color_params BDA,
                                    * 0 = format-default content space (ADR-0069) */
    uint32_t kind;            /* offset 24 (0 solid, 1 linear, 2 radial, 3 image) */
    uint32_t num_stops;       /* offset 28 */
    float grad_from[2];       /* offset 32 */
    float grad_to[2];         /* offset 40 */
    float grad_radius;        /* offset 48 */
    float _pad1;              /* offset 52 */
    flux_canvas_gradient_stop_pc stops[FLUX_GRADIENT_MAX_STOPS]; /* offset 56, size 64 */
    /* Image draw extension (read by the image and SDF fragment shaders).
     * Image UVs are carried per vertex; image_dst remains shared storage for
     * the SDF rounded-rectangle pipeline's screen-space box parameters. */
    uint32_t image_handle;   /* offset 120 */
    uint32_t sampler_handle; /* offset 124 */
    float image_dst[4];      /* offset 128 — SDF box / reserved for images */
    /* image_src selects the sampled sub-rectangle in NORMALISED texture
     * coordinates (u, v, du, dv). The per-vertex [0,1] UV is remapped into
     * it: uv = image_src.xy + v_uv * image_src.zw.
     * For whole-image draws this is {0,0,1,1}; a glyph atlas passes the
     * glyph's sub-rect so one texture serves every glyph. */
    float image_src[4]; /* offset 144 — u, v, du, dv (normalised) */
} flux_canvas_push;

/* Push constants for CANVAS_PIPE_OUTPUT (ADR-0069). Layout matches the
 * push block in shaders/canvas_output.frag and fits inside the shared
 * canvas layout's 160-byte range. */
typedef struct flux_output_push {
    float primaries[3][4]; /* column-major mat3, one column per vec4 */
    uint32_t image_handle;
    uint32_t sampler_handle;
    uint32_t transfer;      /* flux_transfer_func of the encoded side */
    uint32_t flags;         /* bit0 decode, bit1 no-dither */
    float gamma;            /* FLUX_TRANSFER_GAMMA exponent */
    float dither_levels;    /* 255 / 1023; ignored when no-dither */
    float sdr_white_nits;   /* Phase 3 tone mapping; 203 default */
    float _pad;
} flux_output_push;

#define FLUX_OUTPUT_F_DECODE 0x1u
#define FLUX_OUTPUT_F_NO_DITHER 0x2u

/* ------------------------------------------------------------------ */
/*  Display-list segment bodies (forward-declared above)              */
/* ------------------------------------------------------------------ */

struct flux_record_op {
    flux_canvas_push push; /* complete draw state (incl. baked inv_window_size) */
    flux_recti scissor;    /* effective scissor at emit time                    */
    uint32_t pipe_id;      /* canvas_pipe_id                                    */
    uint32_t blend;        /* c->pending_blend at emit time                     */
    uint32_t vert_offset;  /* into the segment's vertex buffer                  */
    uint32_t vert_count;
    /* Host R8 glyph atlas borrowed by the CPU backend (ADR-0019); NULL for
     * non-glyph ops. Points into flux_text's persistent atlas buffer.
     * host_atlas_gen is the producer generation at capture time
     * (FLUX_HOST_ATLAS_UNVERSIONED when the run carried no extension):
     * replay refuses the segment when the canvas has since seen a newer
     * generation for this buffer (texels rearranged, baked UVs stale). */
    const uint8_t *host_atlas;
    uint32_t host_atlas_w;
    uint32_t host_atlas_h;
    uint64_t host_atlas_gen;
};

struct flux_canvas_record_slot {
    flux_canvas *owner;
    uint64_t generation; /* bumped on release/evict/discard; invalidates handles */
    uint64_t last_used;  /* LRU tick */
    uint32_t state;      /* enum flux_record_slot_state */

    /* Replay anchor: the segment is only valid under the exact canvas state
     * it was recorded against — same framebuffer extent (push constants bake
     * inv_window_size), same absolute transform (vertices are baked in
     * physical pixels), same incoming scissor. */
    flux_mat3x2 anchor_transform;
    flux_recti anchor_scissor;
    uint32_t fb_w, fb_h;

    flux_record_op *ops;
    uint32_t op_count, op_cap;
    flux_canvas_vertex *verts;
    uint32_t vert_count, vert_cap;

    /* Resources referenced by recorded image/glyph draws, retained so
     * recorded bindless handles can never dangle or be recycled. */
    flux_image *images[FLUX_CANVAS_RECORD_IMG_CAP];
    uint32_t image_count;
    flux_sampler *samplers[FLUX_CANVAS_RECORD_SAMPLER_CAP];
    uint32_t sampler_count;

    size_t bytes; /* ops + verts, for budget accounting */
};

/* ------------------------------------------------------------------ */
/*  Image (internal)                                                  */
/*                                                                      */
/*  struct flux_image + flux_image_create_compute_writable live in     */
/*  image_internal.h so the effect module can depend on the image      */
/*  struct without dragging in canvas pipelines, push constants,       */
/*  geometry helpers, or path/paint internals.                         */
/* ------------------------------------------------------------------ */

#include "../core/image_internal.h"

/* ------------------------------------------------------------------ */
/*  Path                                                              */
/* ------------------------------------------------------------------ */

/* flux_path_op / flux_path_segment live above struct flux_canvas
 * (the tess cache embeds them). */
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
    CANVAS_PIPE_STENCIL_WRITE_EO, /* even-odd variant: INVERT both faces */
    CANVAS_PIPE_COVER_SOLID,
    CANVAS_PIPE_COVER_GRADIENT,
    CANVAS_PIPE_GLYPH, /* batched glyph run (ADR-0010) */
    CANVAS_PIPE_SDF,   /* analytic rounded-rect / circle (fill + ring) */
    /* Working-space intermediate -> destination output transform
     * (ADR-0069). Never emitted by draws; backend_vk records it in
     * end_pass (encode) and in the LOAD seed blit (decode). Always
     * built 1x / no-stencil / blend-off. */
    CANVAS_PIPE_OUTPUT,
    CANVAS_PIPE_COUNT,
} canvas_pipe_id;

/* flux_canvas_push.kind high bits: content decode hints (ADR-0069).
 * Set when the sampled 8-bit UNORM image holds sRGB-encoded content
 * that hardware views cannot decode for us. */
#define FLUX_CANVAS_PUSH_DECODE_SRGB 0x100u
/* color_params_address holds a live buffer reference (ADR-0070). */
#define FLUX_CANVAS_PUSH_HAS_COLOR_PARAMS 0x200u

flux_result get_canvas_pipeline(flux_device *device, VkFormat color_format,
                                VkSampleCountFlagBits samples, flux_paint_kind kind,
                                flux_blend_mode blend, bool with_stencil,
                                VkPipelineLayout *out_layout, VkPipeline *out_pipeline);
flux_result get_canvas_pipeline_id(flux_device *device, VkFormat color_format,
                                   VkSampleCountFlagBits samples, canvas_pipe_id id,
                                   flux_blend_mode blend, bool with_stencil,
                                   VkPipelineLayout *out_layout, VkPipeline *out_pipeline);

/* Stencil format used by stencil-capable Canvas passes/pipeline variants on
 * this device. Probed once: S8_UINT preferred, combined depth-stencil
 * fallbacks; VK_FORMAT_UNDEFINED when nothing supports the attachment. The
 * independent no-stencil variants always use VK_FORMAT_UNDEFINED. */
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
/*  Display-list record/replay (record.c)                             */
/* ------------------------------------------------------------------ */

/* Single submission choke point: capture into every active recording
 * (when any), then hand the batch to the backend. All front-end draw
 * paths route through here instead of calling backend->submit directly
 * so a recording never misses a draw. */
void canvas_emit(flux_canvas *c, canvas_pipe_id id, const flux_canvas_push *push,
                 const flux_canvas_vertex *verts, uint32_t vertex_count);

/* Retain `img` in every active recording so recorded bindless handles
 * stay valid until the segment is released/evicted. No-op without an
 * active recording, a device, or an image. */
void canvas_record_retain_image(flux_canvas *c, flux_image *img);
void canvas_record_retain_sampler(flux_canvas *c, flux_sampler *sampler);
bool canvas_track_foreign_image(flux_canvas *c, flux_image *img);

/* Record the newest producer generation seen for a host coverage buffer
 * (flux_glyph_run_host_atlas_desc); replay consults it to refuse segments
 * captured before an in-place atlas rearrange. */
void canvas_host_atlas_gen_track(flux_canvas *c, const uint8_t *ptr, uint64_t gen);

/* Release every slot's buffers + retained images and free the pool.
 * Called from flux_canvas_destroy. */
void canvas_record_pool_destroy(flux_canvas *c);

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
/*  Tessellation cache (defined in geometry_tess.c)                   */
/* ------------------------------------------------------------------ */

/* Look up `p` + the tessellation parameters in the canvas's cache.
 * On a hit the cached path-space triangles are transformed by `tx`,
 * submitted with `paint`, and true is returned — the caller is done.
 * On a miss (miss counter bumped) the caller must tessellate with an
 * IDENTITY transform so the output is cacheable path-space geometry,
 * then call tess_cache_store_and_transform before submit. */
bool tess_cache_lookup_submit(flux_canvas *c, const flux_path *p, const flux_paint *paint,
                              bool is_stroke, float pixel_scale, float stroke_width,
                              float miter_limit, flux_mat3x2 tx);

/* After an identity-transform tessellation on a cache miss: store the
 * path-space result (`verts`, `v_count`) in the cache when it is
 * cacheable (!stalled, counts within the inline caps), then transform
 * every vertex in place by `tx` so the caller's submit produces
 * exactly what a transform-during-emit run would have produced. */
void tess_cache_store_and_transform(flux_canvas *c, const flux_path *p, const flux_paint *paint,
                                    bool is_stroke, float pixel_scale, float stroke_width,
                                    float miter_limit, bool stalled, flux_canvas_vertex *verts,
                                    uint32_t v_count, flux_mat3x2 tx);

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
