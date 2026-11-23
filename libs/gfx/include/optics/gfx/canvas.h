/*
 * gfx/canvas.h — 2D immediate-mode canvas.
 *
 * Design contract:
 *   - One canvas per surface.
 *   - optics_gfx_path, optics_gfx_brush are value types owned by a optics_gfx_arena.
 *   - optics_gfx_image is refcounted.
 *   - Per-frame:
 *       optics_gfx_surface_begin_frame  ->
 *       optics_gfx_canvas_begin (clear or load) ->
 *       record draws ->
 *       optics_gfx_canvas_end ->
 *       optics_gfx_frame_submit / optics_gfx_frame_present
 */

#ifndef OPTICS_GFX_CANVAS_H
#define OPTICS_GFX_CANVAS_H

#include <optics/gfx/core.h>
#include <optics/gfx/math.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct optics_gfx_canvas optics_gfx_canvas;

/* optics_gfx_sampler lives in <optics/gfx/vulkan.h>; forward-declare it so sampled image
 * drawing does not pull Vulkan declarations into every Canvas consumer.
 * optics_gfx_image is a core resource and is already declared by <optics/gfx/core.h>. */
typedef struct optics_gfx_sampler optics_gfx_sampler;

/* ------------------------------------------------------------------ */
/*  Paint                                                             */
/* ------------------------------------------------------------------ */

typedef enum optics_gfx_line_cap : uint32_t {
    OPTICS_GFX_CAP_BUTT = 0,
    OPTICS_GFX_CAP_ROUND = 1,
    OPTICS_GFX_CAP_SQUARE = 2
} optics_gfx_line_cap;
typedef enum optics_gfx_line_join : uint32_t {
    OPTICS_GFX_JOIN_MITER = 0,
    OPTICS_GFX_JOIN_ROUND = 1,
    OPTICS_GFX_JOIN_BEVEL = 2
} optics_gfx_line_join;
typedef enum optics_gfx_fill_rule : uint32_t {
    OPTICS_GFX_FILL_NON_ZERO = 0,
    OPTICS_GFX_FILL_EVEN_ODD = 1
} optics_gfx_fill_rule;
typedef enum optics_gfx_blend_mode : uint32_t {
    OPTICS_GFX_BLEND_SRC_OVER = 0,
    OPTICS_GFX_BLEND_SRC = 1,
    OPTICS_GFX_BLEND_PLUS = 2,
    OPTICS_GFX_BLEND_MULTIPLY = 3
} optics_gfx_blend_mode;

#define OPTICS_GFX_GRADIENT_MAX_STOPS 8

typedef struct optics_gfx_gradient_stop {
    float t;                /* [0, 1] along the gradient axis */
    optics_gfx_color color; /* premultiplied */
} optics_gfx_gradient_stop;

typedef struct optics_gfx_gradient_stops {
    uint32_t count;
    optics_gfx_gradient_stop stops[OPTICS_GFX_GRADIENT_MAX_STOPS];
} optics_gfx_gradient_stops;

/* ------------------------------------------------------------------ */
/*  Path (arena-owned)                                                */
/* ------------------------------------------------------------------ */

typedef struct optics_gfx_path optics_gfx_path; /* opaque-in-this-header; see implementation */

OPTICS_GFX_API void optics_gfx_path_reset(optics_gfx_path *p);
OPTICS_GFX_NODISCARD OPTICS_GFX_API optics_gfx_result
optics_gfx_path_create(optics_gfx_path **out, optics_gfx_arena *arena);
OPTICS_GFX_API void optics_gfx_path_move_to(optics_gfx_path *p, float x, float y);
OPTICS_GFX_API void optics_gfx_path_line_to(optics_gfx_path *p, float x, float y);
OPTICS_GFX_API void optics_gfx_path_quad_to(optics_gfx_path *p, float cx, float cy, float x,
                                            float y);
OPTICS_GFX_API void optics_gfx_path_cubic_to(optics_gfx_path *p, float c1x, float c1y, float c2x,
                                             float c2y, float x, float y);
OPTICS_GFX_API void optics_gfx_path_close(optics_gfx_path *p);
OPTICS_GFX_API void optics_gfx_path_add_rect(optics_gfx_path *p, optics_gfx_rect r);
OPTICS_GFX_API void optics_gfx_path_add_round_rect(optics_gfx_path *p, optics_gfx_rect r,
                                                   float radius);
OPTICS_GFX_API void optics_gfx_path_add_squircle(optics_gfx_path *p, optics_gfx_rect r,
                                                 float radius, float curvature);
OPTICS_GFX_API void optics_gfx_path_add_circle(optics_gfx_path *p, float cx, float cy,
                                               float radius);

/* Number of segments rejected due to arena exhaustion. Non-zero means
 * subsequent mutators silently dropped data. Path mutators are
 * void-returning (Cairo/Skia-style); this accessor lets a caller
 * detect overflow without consulting optics_gfx_get_last_error. */
OPTICS_GFX_API uint32_t optics_gfx_path_dropped_count(const optics_gfx_path *p);

/* ------------------------------------------------------------------ */
/*  Canvas lifecycle + drawing                                        */
/* ------------------------------------------------------------------ */

/* Backend selection for optics_gfx_canvas_create (Skia SkSurface-style: the factory
 * binds the backend, then the same drawing API is used regardless). AUTO picks
 * GPU when a surface is provided, otherwise the software (CPU) backend. */
typedef enum optics_gfx_canvas_backend_kind : uint32_t {
    OPTICS_GFX_CANVAS_BACKEND_AUTO = 0,
    OPTICS_GFX_CANVAS_BACKEND_GPU = 1,
    OPTICS_GFX_CANVAS_BACKEND_CPU = 2,
} optics_gfx_canvas_backend_kind;

typedef struct optics_gfx_canvas_desc {
    optics_gfx_struct_type type; /* OPTICS_GFX_TYPE_CANVAS_DESC */
    const void *next;
    optics_gfx_surface *surface;            /* GPU: required (retained). CPU: ignored. */
    float scale;                            /* content scale (device-pixel ratio); 0 => 1.0.
                                               The canvas draws in logical units scaled onto
                                               the physical surface by this factor; glyph
                                               reads it to rasterise glyphs crisply at HiDPI.
                                               Change later with optics_gfx_canvas_set_scale. */
    optics_gfx_canvas_backend_kind backend; /* default AUTO */
    uint32_t width, height;                 /* CPU framebuffer size (physical px), both
                                               required; ignored for the GPU backend. */
} optics_gfx_canvas_desc;

#define OPTICS_GFX_CANVAS_DESC_INIT {.type = OPTICS_GFX_TYPE_CANVAS_DESC}

/* Destination selection is independent of recording context and pass policy.
 * DEFAULT selects the frame attachment on GPU, or the Canvas framebuffer on
 * CPU. IMAGE selects a sampleable GPU render image. TARGET selects an owned
 * CPU target or an image-backed target. GPU destinations always require frame. */
typedef enum optics_gfx_canvas_attachment_kind : uint32_t {
    OPTICS_GFX_CANVAS_ATTACHMENT_DEFAULT = 0,
    OPTICS_GFX_CANVAS_ATTACHMENT_IMAGE = 1,
    OPTICS_GFX_CANVAS_ATTACHMENT_TARGET = 2,
} optics_gfx_canvas_attachment_kind;

typedef struct optics_gfx_canvas_attachment {
    optics_gfx_canvas_attachment_kind kind;
    union {
        optics_gfx_image *image;
        optics_gfx_target *target;
    };
} optics_gfx_canvas_attachment;

/* Write scope of one Canvas pass: the set of rectangles, in physical pixels of
 * the destination, that the pass may modify. Empty (`count == 0`, or a null
 * pointer) means "the whole destination", which is also what an over-capacity
 * set degrades to.
 *
 * The set is a *scope*, not a bound: geometry outside it is culled by the
 * rasteriser and pixels outside it are preserved. Reporting one rectangle before
 * the consumer has seen the set destroys the localisation irrecoverably — the
 * union of a top strip and a bottom dock is the whole output — so this type
 * carries the whole list and the backend scissors each rectangle independently
 * rather than collapsing. See `optics_ui_damage_rects` for the producer side.
 *
 * Rectangles are expected disjoint but overlap is harmless: a scissored repaint
 * is idempotent. */
/* `optics_gfx_damage` and OPTICS_GFX_DAMAGE_CAPACITY are core types (gfx/core.h): a
 * damage set is a frame input, not a canvas-private convention. */

typedef struct optics_gfx_canvas_pass_desc {
    optics_gfx_struct_type type; /* OPTICS_GFX_TYPE_CANVAS_PASS_DESC */
    const void *next;
    optics_gfx_frame *frame; /* GPU recording context; nullptr on CPU. Borrowed until end. */
    optics_gfx_canvas_attachment attachment; /* resources retained until end */
    const optics_gfx_color *clear_color;     /* non-NULL: clear; NULL: load */
    /* Rectangles this pass may write, in destination physical pixels. The
     * destination must already hold a complete frame outside them — a skipped
     * region is not repainted. Empty selects the full destination extent.
     *
     * The array is read and copied during `optics_gfx_canvas_begin`; it does not need
     * to outlive that call. Retaining it for the whole pass would be a
     * use-after-free for bindings that build the list in a temporary container.
     *
     * A pass is the right granularity for this because a frame can render into
     * several destinations (a target capture and the window), and each has its
     * own scope. The frame aggregates what its passes actually wrote into
     * `optics_gfx_frame`'s epoch history, which is what a swapchain present uses to
     * bring every image in the rotation up to date (ADR-0149). */
    optics_gfx_damage damage;
    /* Declare that this pass clears its whole destination. A full-extent pass is
     * the only way to erase a region whose owner is gone, so the flag exists to
     * make "I really do want everything repainted" explicit rather than inferred
     * from an empty damage set. */
    bool clear_extent;
} optics_gfx_canvas_pass_desc;

#define OPTICS_GFX_CANVAS_PASS_DESC_INIT {.type = OPTICS_GFX_TYPE_CANVAS_PASS_DESC}

/* Create a canvas on the selected backend. GPU canvases need desc->surface;
 * CPU canvases need desc->width/height (see gfx/canvas_cpu.h for a convenience
 * wrapper and pixel readback). Release with optics_gfx_canvas_release. */
OPTICS_GFX_NODISCARD OPTICS_GFX_API optics_gfx_result
optics_gfx_canvas_create(const optics_gfx_canvas_desc *desc, optics_gfx_canvas **out);
OPTICS_GFX_NODISCARD OPTICS_GFX_API optics_gfx_canvas *
optics_gfx_canvas_retain(optics_gfx_canvas *c);
OPTICS_GFX_API void optics_gfx_canvas_release(optics_gfx_canvas *c);

/* Content scale (device-pixel ratio). optics_gfx_canvas_set_scale makes each
 * optics_gfx_canvas_begin start with this as the base transform, so all drawing is
 * in logical units mapped onto the physical surface. optics_gfx_canvas_get_scale
 * returns the *effective* scale of the active transform (the base content
 * scale composed with any optics_gfx_canvas_scale on the stack); glyph reads it
 * to rasterise glyphs at the device resolution. Set the content scale once
 * when the surface scale changes. */
OPTICS_GFX_API void optics_gfx_canvas_set_scale(optics_gfx_canvas *c, float scale);
OPTICS_GFX_API float optics_gfx_canvas_get_scale(const optics_gfx_canvas *c);

/* The only pass bracket. A null descriptor selects defaults (CPU only).
 * GPU execution requires an open frame belonging to the Canvas surface.
 * The destination determines extent. CPU TARGET loads its own pixels, never
 * another destination's contents. Unsupported attachments fail before recording.
 * All draw-time failures are sticky; end closes the pass and returns the first
 * failure. Neither end nor release submits GPU work. Always check end. */
OPTICS_GFX_NODISCARD OPTICS_GFX_API optics_gfx_result
optics_gfx_canvas_begin(optics_gfx_canvas *c, const optics_gfx_canvas_pass_desc *desc);
OPTICS_GFX_NODISCARD OPTICS_GFX_API optics_gfx_result optics_gfx_canvas_end(optics_gfx_canvas *c);

/* CPU default framebuffer readback, valid only outside a pass. Borrowed until
 * the next Canvas operation. Returns nullptr for GPU canvases. */
OPTICS_GFX_API const uint8_t *optics_gfx_canvas_read_pixels(optics_gfx_canvas *c, uint32_t *width,
                                                            uint32_t *height, uint32_t *stride);

/* State stack. All draws and state mutators between optics_gfx_canvas_begin
 * and optics_gfx_canvas_end record into the bound frame; calls outside that
 * window are silent no-ops (consult optics_gfx_get_last_error). */
OPTICS_GFX_API void optics_gfx_canvas_save(optics_gfx_canvas *c);
OPTICS_GFX_API void optics_gfx_canvas_restore(optics_gfx_canvas *c);
/* Save a compositing layer (ADR-0088 / Opacity Group).
 * Subtree draws are recorded into a tightly scissored transient offscreen buffer
 * and composited back into the parent target with `opacity` on optics_gfx_canvas_restore. */
typedef struct optics_gfx_canvas_save_layer_opts {
    const optics_gfx_rect *bounds;
    float opacity;
} optics_gfx_canvas_save_layer_opts;

OPTICS_GFX_API void optics_gfx_canvas_save_layer(optics_gfx_canvas *c,
                                                 const optics_gfx_canvas_save_layer_opts *opts);
/* Intersect the current clip with `r`. The rectangle is expressed in the
 * current logical coordinate system and transformed to the physical-pixel
 * scissor, including content scale and affine transforms. */
OPTICS_GFX_API void optics_gfx_canvas_clip_rect(optics_gfx_canvas *c, optics_gfx_rect r);

typedef struct optics_gfx_canvas_translate_opts {
    float x;
    float y;
} optics_gfx_canvas_translate_opts;

OPTICS_GFX_API void optics_gfx_canvas_translate(optics_gfx_canvas *c,
                                                const optics_gfx_canvas_translate_opts *opts);

typedef struct optics_gfx_canvas_scale_opts {
    float sx;
    float sy;
} optics_gfx_canvas_scale_opts;

OPTICS_GFX_API void optics_gfx_canvas_scale(optics_gfx_canvas *c,
                                            const optics_gfx_canvas_scale_opts *opts);
OPTICS_GFX_API void optics_gfx_canvas_rotate(optics_gfx_canvas *c, float radians);
OPTICS_GFX_API void optics_gfx_canvas_transform(optics_gfx_canvas *c, optics_gfx_mat3x2 m);

typedef struct optics_gfx_glyph_run_desc optics_gfx_glyph_run_desc;

/* ================================================================== */
/*  Algebraic Geometry & Brush Model (ADR-0088 / ADR-0091 Clean-Break)*/
/* ================================================================== */

typedef enum optics_gfx_geom_kind : uint8_t {
    OPTICS_GFX_GEOM_RECT = 0,
    OPTICS_GFX_GEOM_RRECT = 1,
    OPTICS_GFX_GEOM_SQUIRCLE = 2,
    OPTICS_GFX_GEOM_CIRCLE = 3,
    OPTICS_GFX_GEOM_LINE = 4,
    OPTICS_GFX_GEOM_PATH = 5,
} optics_gfx_geom_kind;

typedef struct optics_gfx_geom_rect_data {
    optics_gfx_rect rect;
} optics_gfx_geom_rect_data;

typedef struct optics_gfx_geom_rrect_data {
    optics_gfx_rect rect;
    float radius;
} optics_gfx_geom_rrect_data;

typedef struct optics_gfx_geom_squircle_data {
    optics_gfx_rect rect;
    float radius;
    float curvature; /* 0.0f = standard rrect, 1.0f = G2 continuous superellipse */
} optics_gfx_geom_squircle_data;

typedef struct optics_gfx_geom_circle_data {
    float cx, cy, radius;
} optics_gfx_geom_circle_data;

typedef struct optics_gfx_geom_line_data {
    float x0, y0, x1, y1;
} optics_gfx_geom_line_data;

typedef struct optics_gfx_geom_path_data {
    const optics_gfx_path *path;
    optics_gfx_fill_rule fill_rule;
} optics_gfx_geom_path_data;

/* Stroke topology is independent of the brush. Zero width means fill;
 * zero miter_limit selects 4. Other fields use their zero-valued defaults. */
typedef struct optics_gfx_stroke_style {
    float width;
    float miter_limit;
    optics_gfx_line_cap cap;
    optics_gfx_line_join join;
} optics_gfx_stroke_style;

typedef struct optics_gfx_geometry {
    optics_gfx_geom_kind kind;
    uint8_t _pad[3];
    optics_gfx_stroke_style stroke;
    union {
        optics_gfx_geom_rect_data rect;
        optics_gfx_geom_rrect_data rrect;
        optics_gfx_geom_squircle_data squircle;
        optics_gfx_geom_circle_data circle;
        optics_gfx_geom_line_data line;
        optics_gfx_geom_path_data path;
    };
} optics_gfx_geometry;

typedef enum optics_gfx_brush_kind : uint8_t {
    OPTICS_GFX_BRUSH_SOLID = 0,
    OPTICS_GFX_BRUSH_LINEAR_GRADIENT = 1,
    OPTICS_GFX_BRUSH_RADIAL_GRADIENT = 2,
    OPTICS_GFX_BRUSH_IMAGE_PATTERN = 3,
} optics_gfx_brush_kind;

typedef struct optics_gfx_brush_solid_data {
    optics_gfx_color color;
} optics_gfx_brush_solid_data;

typedef struct optics_gfx_brush_gradient_data {
    optics_gfx_gradient_stops stops;
    optics_gfx_point start;
    optics_gfx_point end;
    float radius;
} optics_gfx_brush_gradient_data;

typedef struct optics_gfx_brush_image_data {
    optics_gfx_image *image;
    optics_gfx_sampler *sampler; /* optional (nullptr = default linear) */
    optics_gfx_rect src_rect;    /* normalized UV {u, v, du, dv}, 0,0,0,0 = full */
    bool opaque_only;
    optics_gfx_color tint;
    optics_gfx_rect clip_rect;
    float clip_radius;
} optics_gfx_brush_image_data;

typedef struct optics_gfx_brush {
    optics_gfx_brush_kind kind;
    optics_gfx_blend_mode blend;
    float opacity; /* [0.0f, 1.0f] */
    union {
        optics_gfx_brush_solid_data solid;
        optics_gfx_brush_gradient_data gradient;
        optics_gfx_brush_image_data image;
    };
} optics_gfx_brush;

typedef struct optics_gfx_canvas_draw_geometry_opts {
    const optics_gfx_geometry *geom;
    const optics_gfx_brush *brush;
} optics_gfx_canvas_draw_geometry_opts;

OPTICS_GFX_API void
optics_gfx_canvas_draw_geometry(optics_gfx_canvas *c,
                                const optics_gfx_canvas_draw_geometry_opts *opts);

/* ================================================================== */
/*  Immutable DisplayList & Encoder (ADR-0088 Clean-Break)            */
/* ================================================================== */

/* Shared immutable owner. Payload storage is private; copying a pointer does
 * not acquire ownership. Each retained reference must be released once. */
typedef struct optics_gfx_display_list optics_gfx_display_list;

/* Dynamic image slots one display list can declare. The mask is a uint64_t. */
#define OPTICS_GFX_DISPLAY_LIST_MAX_INPUTS 64u

typedef struct optics_gfx_encoder optics_gfx_encoder;

typedef struct optics_gfx_encoder_desc {
    size_t max_bytes;          /* Owned command storage budget; zero selects 64 MiB. */
    uint32_t max_commands;     /* Expanded execution budget; zero selects 1,048,576. */
    size_t max_retained_bytes; /* Pinned image-content budget; zero = unbounded. */
} optics_gfx_encoder_desc;

/* Unique recorder. Creation owns all scratch storage. Finish is terminal;
 * destroy the recorder after finish or to abort unfinished recording. */
OPTICS_GFX_NODISCARD OPTICS_GFX_API optics_gfx_result
optics_gfx_encoder_create(const optics_gfx_encoder_desc *desc, optics_gfx_encoder **out);
/* Producers propagate capture failures into the recorder's sticky result.
 * OPTICS_GFX_OK is a no-op; the first failure cannot be replaced or cleared. */
OPTICS_GFX_API void optics_gfx_encoder_fail(optics_gfx_encoder *enc, optics_gfx_result error);
OPTICS_GFX_API optics_gfx_result optics_gfx_encoder_status(const optics_gfx_encoder *enc);
typedef struct optics_gfx_encoder_draw_geometry_opts {
    const optics_gfx_geometry *geom;
    const optics_gfx_brush *brush;
    uint32_t slot;
    bool use_slot;
} optics_gfx_encoder_draw_geometry_opts;

OPTICS_GFX_API void
optics_gfx_encoder_draw_geometry(optics_gfx_encoder *enc,
                                 const optics_gfx_encoder_draw_geometry_opts *opts);
OPTICS_GFX_API void optics_gfx_encoder_draw_glyph_run(optics_gfx_encoder *enc,
                                                      const optics_gfx_glyph_run_desc *desc);
OPTICS_GFX_API void optics_gfx_encoder_save(optics_gfx_encoder *enc);
OPTICS_GFX_API void optics_gfx_encoder_restore(optics_gfx_encoder *enc);
OPTICS_GFX_API void optics_gfx_encoder_clip_rect(optics_gfx_encoder *enc, optics_gfx_rect r);

typedef struct optics_gfx_encoder_save_layer_opts {
    const optics_gfx_rect *bounds;
    float opacity;
} optics_gfx_encoder_save_layer_opts;

OPTICS_GFX_API void optics_gfx_encoder_save_layer(optics_gfx_encoder *enc,
                                                  const optics_gfx_encoder_save_layer_opts *opts);

typedef struct optics_gfx_encoder_translate_opts {
    float x;
    float y;
} optics_gfx_encoder_translate_opts;

OPTICS_GFX_API void optics_gfx_encoder_translate(optics_gfx_encoder *enc,
                                                 const optics_gfx_encoder_translate_opts *opts);

typedef struct optics_gfx_encoder_scale_opts {
    float sx;
    float sy;
} optics_gfx_encoder_scale_opts;

OPTICS_GFX_API void optics_gfx_encoder_scale(optics_gfx_encoder *enc,
                                             const optics_gfx_encoder_scale_opts *opts);
OPTICS_GFX_API void optics_gfx_encoder_rotate(optics_gfx_encoder *enc, float radians);
OPTICS_GFX_API void optics_gfx_encoder_transform(optics_gfx_encoder *enc, optics_gfx_mat3x2 m);

/* Destroy an encoder and release its scratch resources. Published display lists survive. */
OPTICS_GFX_API void optics_gfx_encoder_destroy(optics_gfx_encoder *enc);

/* Freeze encoder commands into an immutable display list. */
OPTICS_GFX_NODISCARD OPTICS_GFX_API optics_gfx_result
optics_gfx_encoder_finish(optics_gfx_encoder *enc, optics_gfx_display_list **out_list);

OPTICS_GFX_API optics_gfx_display_list *
optics_gfx_display_list_retain(optics_gfx_display_list *list);
OPTICS_GFX_API void optics_gfx_display_list_release(optics_gfx_display_list *list);
OPTICS_GFX_API size_t optics_gfx_display_list_size(const optics_gfx_display_list *list);
OPTICS_GFX_API uint32_t optics_gfx_display_list_command_count(const optics_gfx_display_list *list);
/* Pinned image-content bytes retained by this list, including child lists.
 * A shared child is counted once per list that pins it. */
OPTICS_GFX_API size_t optics_gfx_display_list_retained_bytes(const optics_gfx_display_list *list);
/* Bit set of dynamic image input slots this list (including children) uses. */
OPTICS_GFX_API uint64_t optics_gfx_display_list_input_mask(const optics_gfx_display_list *list);

/* One execution binding for a dynamic image slot. The slot must be present in
 * the list's input mask; the image is borrowed for the submit call and
 * retained by the canvas for the frame. */
typedef struct optics_gfx_display_list_binding {
    uint32_t slot;
    optics_gfx_image *image;
    optics_gfx_sampler *sampler; /* optional (nullptr = default linear) */
} optics_gfx_display_list_binding;

/* Submit an immutable display list to a canvas for batch execution. A list
 * that declares dynamic inputs fails with OPTICS_GFX_ERROR_INVALID_STATE here; use
 * the bound form below. */
typedef struct optics_gfx_canvas_submit_display_list_opts {
    const optics_gfx_display_list *list;
    const optics_gfx_display_list_binding *bindings;
    uint32_t binding_count;
} optics_gfx_canvas_submit_display_list_opts;

/* Submit a display list, optionally binding dynamic image slots. */
OPTICS_GFX_NODISCARD OPTICS_GFX_API optics_gfx_result optics_gfx_canvas_submit_display_list(
    optics_gfx_canvas *c, const optics_gfx_canvas_submit_display_list_opts *opts);

/* ================================================================== */
/*  DisplayList serialization (ADR-0157)                              */
/* ================================================================== */

/* A serialized list is a versioned byte stream with a resource table. It
 * never contains C pointers or native struct bytes. Resources are identified
 * by caller-assigned nonzero IDs; child lists are flattened into the command
 * stream with their state isolation preserved by SAVE/RESTORE. */

typedef enum optics_gfx_display_list_resource_kind : uint32_t {
    OPTICS_GFX_DISPLAY_LIST_RESOURCE_IMAGE = 1,
    OPTICS_GFX_DISPLAY_LIST_RESOURCE_SAMPLER = 2,
} optics_gfx_display_list_resource_kind;

/* Return a stable nonzero ID for `resource`, or 0 to abort serialization.
 * The same (kind, resource) pair must always return the same ID, and one ID
 * cannot name two different resources. */
typedef uint32_t (*optics_gfx_display_list_resource_id_fn)(
    void *user, optics_gfx_display_list_resource_kind kind, const void *resource);

/* Resolve an ID from the serialized table. Return nullptr to fail the load.
 * Images are snapshotted by the loader; samplers are retained by the list. */
typedef const void *(*optics_gfx_display_list_resource_resolve_fn)(
    void *user, optics_gfx_display_list_resource_kind kind, uint32_t id);

typedef struct optics_gfx_display_list_serialize_opts {
    optics_gfx_struct_type type; /* OPTICS_GFX_TYPE_DISPLAY_LIST_SERIALIZE_OPTS */
    const void *next;
    optics_gfx_display_list_resource_id_fn resource_id;
    void *user;
    uint8_t **out_data;
    size_t *out_size;
} optics_gfx_display_list_serialize_opts;

typedef struct optics_gfx_display_list_deserialize_opts {
    optics_gfx_struct_type type; /* OPTICS_GFX_TYPE_DISPLAY_LIST_DESERIALIZE_OPTS */
    const void *next;
    const uint8_t *data;
    size_t size;
    optics_gfx_display_list_resource_resolve_fn resolve;
    void *user;
    optics_gfx_display_list **out_list;
} optics_gfx_display_list_deserialize_opts;

#define OPTICS_GFX_DISPLAY_LIST_SERIALIZE_OPTS_INIT                                                \
    {.type = OPTICS_GFX_TYPE_DISPLAY_LIST_SERIALIZE_OPTS}
#define OPTICS_GFX_DISPLAY_LIST_DESERIALIZE_OPTS_INIT                                              \
    {.type = OPTICS_GFX_TYPE_DISPLAY_LIST_DESERIALIZE_OPTS}

/* Serialize to a newly allocated byte stream. Release with
 * optics_gfx_display_list_serialized_free. A list with resources requires
 * opts->resource_id. */
OPTICS_GFX_NODISCARD OPTICS_GFX_API optics_gfx_result optics_gfx_display_list_serialize(
    const optics_gfx_display_list *list, const optics_gfx_display_list_serialize_opts *opts);

/* Load a serialized list. Every table entry must resolve; malformed input,
 * duplicate IDs, unknown opcodes, bad lengths, and missing resources fail
 * with no published list and no retained reference. */
OPTICS_GFX_NODISCARD OPTICS_GFX_API optics_gfx_result
optics_gfx_display_list_deserialize(const optics_gfx_display_list_deserialize_opts *opts);

OPTICS_GFX_API void optics_gfx_display_list_serialized_free(uint8_t *data);

/* Record an owned child list under the recorder's current placement and clip.
 * Child state is isolated; the child is retained without copying. */
OPTICS_GFX_NODISCARD OPTICS_GFX_API optics_gfx_result
optics_gfx_encoder_draw_display_list(optics_gfx_encoder *enc, const optics_gfx_display_list *list);

/* ================================================================== */
/*  RenderPlan Execution Diagnostics (ADR-0089 / ADR-0092)            */
/* ================================================================== */

typedef struct optics_gfx_render_plan_stats {
    uint32_t pass_count;
    uint32_t layer_depth_peak;
    size_t transient_bytes_allocated;
    size_t transient_bytes_aliased_saved;
} optics_gfx_render_plan_stats;

OPTICS_GFX_API optics_gfx_render_plan_stats
optics_gfx_canvas_get_plan_stats(const optics_gfx_canvas *c);

#include <optics/gfx/canvas_helpers.h>

/* ------------------------------------------------------------------ */
/*  Glyph runs (ADR-0010)                                             */
/* ------------------------------------------------------------------ */

/* One glyph blit: a screen-space rectangle sampled from a texel
 * sub-rect of the run's atlas, tinted by a premultiplied colour.
 * Maps 1:1 onto a shaped glyph (e.g. one HarfBuzz glyph position). */
typedef struct optics_gfx_glyph_quad {
    float sx, sy;           /* screen-space top-left (canvas transform applies) */
    float sw, sh;           /* destination size in pixels */
    uint16_t ax, ay;        /* atlas top-left, texels */
    uint16_t aw, ah;        /* atlas extent,   texels */
    optics_gfx_color color; /* premultiplied per-glyph tint */
} optics_gfx_glyph_quad;

typedef struct optics_gfx_glyph_run_desc {
    optics_gfx_struct_type type; /* OPTICS_GFX_TYPE_GLYPH_RUN_DESC */
    const void *next;
    optics_gfx_image *atlas; /* caller-owned; R8 coverage (.r × tint).
                              * NULL on a device-less CPU canvas — set
                              * host_coverage instead. */
    /* Host-resident R8 coverage atlas, the CPU-backend alternative to
     * `atlas` (ADR-0019): when non-NULL the CPU rasteriser samples
     * coverage straight from this buffer with no GPU image, so a
     * device-less canvas (optics_gfx_canvas_create (CPU backend)) can render glyph runs.
     * Ignored when a device/atlas is present. `host_atlas_w/h` are the
     * buffer's texel extent (0 when unused).
     *
     * Lifetime + invalidation: the buffer is borrowed, never copied. It
     * must outlive every display-list segment that recorded the run —
     * release such segments (or release the canvas) before the producer
     * frees or reallocates the buffer. A producer that rearranges texels
     * in place (e.g. a full-atlas reclaim) should attach
     * optics_gfx_glyph_run_host_atlas_desc so replay of pre-rearrangement
     * segments is refused instead of sampling moved texels. */
    const uint8_t *host_coverage;
    uint32_t host_atlas_w;
    uint32_t host_atlas_h;
    optics_gfx_sampler *sampler; /* optional; NULL = canvas default (linear).
                                  * Pass a NEAREST sampler for crisp blits. */
    const optics_gfx_glyph_quad *quads;
    uint32_t quad_count;
} optics_gfx_glyph_run_desc;

#define OPTICS_GFX_GLYPH_RUN_DESC_INIT {.type = OPTICS_GFX_TYPE_GLYPH_RUN_DESC}

/* Optional optics_gfx_glyph_run_desc.next payload: the producer's content
 * generation for a host coverage buffer. The canvas records the pair
 * (host_coverage pointer, generation) with each captured glyph batch and
 * remembers the newest generation it has seen per buffer; optics_gfx_canvas_cache_replay
 * then refuses a segment whose recorded generation is stale (the buffer's
 * texels were rearranged since, so the baked UVs would sample the wrong
 * cells), letting the caller re-emit and re-record. Producers that never
 * rearrange in place may omit the extension; their segments replay
 * unchecked. glyph attaches its atlas generation automatically. */
typedef struct optics_gfx_glyph_run_host_atlas_desc {
    optics_gfx_struct_type type; /* OPTICS_GFX_TYPE_GLYPH_RUN_HOST_ATLAS_DESC */
    const void *next;
    uint64_t generation;
} optics_gfx_glyph_run_host_atlas_desc;

#define OPTICS_GFX_GLYPH_RUN_HOST_ATLAS_DESC_INIT                                                  \
    {.type = OPTICS_GFX_TYPE_GLYPH_RUN_HOST_ATLAS_DESC}

/* Draw a pre-shaped glyph run as a single batched draw call. The
 * library does no shaping, kerning, or atlas management (ADR-0010):
 * quads arrive positioned, the atlas is caller-owned and updated via
 * optics_gfx_image_update_region. The atlas's R channel is alpha coverage
 * multiplied by each quad's premultiplied tint, blended SRC_OVER —
 * the same R8-coverage × tint contract, without per-glyph draw calls.
 *
 * On a device-less CPU canvas (optics_gfx_canvas_create (CPU backend)) there is no
 * GPU image to bind, so `atlas` is ignored and `host_coverage` is used
 * instead: a host-resident R8 coverage buffer of host_atlas_w×host_atlas_h
 * texels (ADR-0019). The CPU rasteriser samples coverage directly from it. */
OPTICS_GFX_API void optics_gfx_canvas_draw_glyph_run(optics_gfx_canvas *c,
                                                     const optics_gfx_glyph_run_desc *desc);

/* Cumulative count of draw calls dropped due to transient ring exhaustion
 * since canvas creation. Non-zero means the per-frame transient ring is too
 * small for the workload; check optics_gfx_frame_alloc_transient or increase the
 * ring size. */
OPTICS_GFX_API uint64_t optics_gfx_canvas_dropped_draws(const optics_gfx_canvas *c);

/* Vulkan batching diagnostics, cumulative since canvas creation.
 * `submit_calls` counts front-end batches handed to the backend;
 * `recorded_draws` counts vkCmdDraw commands after consecutive compatible
 * submits are merged. Both are zero for the CPU backend. */
OPTICS_GFX_API uint64_t optics_gfx_canvas_submit_calls(const optics_gfx_canvas *c);
OPTICS_GFX_API uint64_t optics_gfx_canvas_recorded_draws(const optics_gfx_canvas *c);

/* ------------------------------------------------------------------ */
/*  Display-list segments (record / replay)                           */
/* ------------------------------------------------------------------ */

/* Opaque handle to a recorded draw segment. The segment is owned by the
 * canvas (fixed slot pool); the handle is a {slot, generation} pair so a
 * stale handle — released, LRU-evicted, or from another canvas — fails
 * validation instead of replaying the wrong draws. Zero-initialise for
 * the null record. */
typedef struct optics_gfx_canvas_cache_entry {
    void *slot;
    uint64_t generation;
} optics_gfx_canvas_cache_entry;

#define OPTICS_GFX_CANVAS_CACHE_ENTRY_INIT {.slot = nullptr, .generation = 0}

/* Start capturing every draw submitted between here and the matching
 * optics_gfx_canvas_cache_end into a new segment. Recording is passive: draws
 * are still submitted live, so a recorded frame renders exactly as an
 * unrecorded one. Recordings nest (e.g. a re-recording parent subtree
 * around a recording child); every active recording captures every draw.
 * Returns false (and records nothing) when called outside
 * begin_frame/end_frame or when the nesting-depth cap is hit — the caller
 * then simply draws without recording and must not call end_record. */
OPTICS_GFX_API bool optics_gfx_canvas_cache_begin(optics_gfx_canvas *c);

/* Close the innermost recording and return its handle (null on budget
 * overflow or allocation failure — the live draws still happened, only
 * the recording is lost). The segment stays valid until
 * optics_gfx_canvas_cache_release, LRU eviction under the canvas-wide byte
 * budget, or canvas destruction. */
OPTICS_GFX_API optics_gfx_canvas_cache_entry optics_gfx_canvas_cache_end(optics_gfx_canvas *c);

/* Re-submit a recorded segment. Replays only when the canvas state still
 * matches the recording exactly — same framebuffer extent, same absolute
 * transform, same incoming scissor — because vertex positions and push
 * constants are baked in physical pixels at record time. On any mismatch
 * (moved/scaled content, changed clip, resized target) nothing is drawn
 * and false is returned; the caller should re-emit and re-record. A
 * replay inside an active recording is itself captured, so a
 * re-recording ancestor stays complete when an unchanged child replays.
 * Images and custom samplers referenced by the segment are retained by
 * the canvas for the segment's lifetime. A recorded host-coverage glyph
 * batch additionally refuses to replay once its producer reports a newer
 * atlas generation (optics_gfx_glyph_run_host_atlas_desc): the baked UVs would
 * sample rearranged texels. */
OPTICS_GFX_API bool optics_gfx_canvas_cache_replay(optics_gfx_canvas *c,
                                                   optics_gfx_canvas_cache_entry rec);

/* Release a segment early (e.g. its subtree changed or died). Safe on a
 * null or stale handle. Unreleased segments are reclaimed by the canvas's
 * LRU byte budget and at canvas destruction. */
OPTICS_GFX_API void optics_gfx_canvas_cache_release(optics_gfx_canvas *c,
                                                    optics_gfx_canvas_cache_entry rec);

/* Diagnostics: cumulative successful end_record / replay counts since
 * canvas creation. Tests diff these across a frame to assert that a
 * static frame replays and a changed frame re-records. */
OPTICS_GFX_API uint64_t optics_gfx_canvas_cache_entries_created(const optics_gfx_canvas *c);
OPTICS_GFX_API uint64_t optics_gfx_canvas_cache_entries_replayed(const optics_gfx_canvas *c);

#ifdef __cplusplus
}
#endif

#endif /* OPTICS_GFX_CANVAS_H */
