/*
 * Internal canvas layout. Never installed.
 */
#ifndef OPTICS_GFX_CANVAS_INTERNAL_H
#define OPTICS_GFX_CANVAS_INTERNAL_H

#include "../core/internal.h"
#include <assert.h>
#include <math.h>
#include <optics/gfx/canvas.h>
#include <optics/gfx/compute.h>
#include <optics/gfx/vulkan.h>
#include <stdint.h>

void canvas_fail(optics_gfx_canvas *c, optics_gfx_result result, const char *message);
bool canvas_finite_rect(optics_gfx_rect r);
bool canvas_valid_geometry(const optics_gfx_geometry *g);
bool canvas_valid_brush(const optics_gfx_brush *b);
bool canvas_valid_brush_slot(const optics_gfx_brush *b);

/* Paint kind selects how the surface colour is computed.
 *   SOLID  - flat colour from `color`
 *   LINEAR - colour interpolated between stops along
 *            line (linear.from -> linear.to), positions in pixel space
 *   RADIAL - colour interpolated between stops by distance from
 *            radial.center, normalized by radial.radius (pixel space) */
typedef enum canvas_paint_kind : uint32_t {
    CANVAS_PAINT_SOLID = 0,
    CANVAS_PAINT_LINEAR_GRADIENT = 1,
    CANVAS_PAINT_RADIAL_GRADIENT = 2,
    CANVAS_PAINT_IMAGE = 3,
} canvas_paint_kind;

typedef struct canvas_paint {
    canvas_paint_kind kind;
    optics_gfx_color color; /* SOLID: fill colour; IMAGE: tint; gradient: ignored */

    /* Extension chain head; tagged with an OPTICS_GFX_TYPE_* value (optics_gfx_struct_type). NULL
     * for every paint produced by the constructors below and by zero-init. */

    /* Stroke parameters — apply to every stroke_path regardless of kind. */
    float stroke_width;
    float miter_limit;
    optics_gfx_line_cap cap;
    optics_gfx_line_join join;
    optics_gfx_fill_rule fill_rule;
    optics_gfx_blend_mode blend;

    /* Gradient or image pattern parameters (canvas space, pre-transform). */
    union {
        struct {
            optics_gfx_point from;
            optics_gfx_point to;
            optics_gfx_gradient_stops stops;
        } linear;
        struct {
            optics_gfx_point center;
            float radius;
            optics_gfx_gradient_stops stops;
        } radial;
        struct {
            optics_gfx_image *image;
            optics_gfx_sampler *sampler;
            optics_gfx_rect dst_rect;
            optics_gfx_rect src_rect;
            optics_gfx_bindless_handle sampler_handle;
            bool opaque_only;
        } image;
    } gradient;
} canvas_paint;

canvas_paint canvas_paint_default(void);

#define OPTICS_GFX_CANVAS_MAX_STATES 32
#define OPTICS_GFX_CANVAS_PATH_SCRATCH_CAP 2048
#define OPTICS_GFX_CANVAS_MAX_CONTOURS 64

/* ADR-0069: the canvas always renders into this working-space
 * intermediate (linear light, scRGB scale); the output transform
 * pass converts to the destination's color space. */
#define OPTICS_GFX_CANVAS_LINEAR_FORMAT VK_FORMAT_R16G16B16A16_SFLOAT

/* Integer clip rectangle in physical pixels. Backend-neutral (the Vulkan
 * backend converts it to VkRect2D at scissor time). */
typedef struct optics_gfx_recti {
    int32_t x, y;
    uint32_t w, h;
} optics_gfx_recti;

typedef struct optics_gfx_canvas_state {
    optics_gfx_mat3x2 transform;
    optics_gfx_recti scissor;
    bool is_layer;
    float layer_opacity;
} optics_gfx_canvas_state;

/* Internal, ABI-independent pass policy. The public descriptor is parsed exactly
 * once by canvas.c; backends consume this normalized value and never inspect
 * extension memory themselves. Declared here because `optics_gfx_canvas` carries the
 * active pass's copy. */
typedef struct canvas_pass_config {
    const optics_gfx_color *clear_color;
    /* The pass's write scope, in destination physical pixels. Empty means the
     * full destination extent. Borrowed from the public descriptor's array, which
     * must outlive the pass. */
    optics_gfx_damage damage;
    /* The caller declared the whole destination may be cleared. Distinct from an
     * empty damage set: a frame can legitimately want "everything" while still
     * naming regions elsewhere. */
    bool clear_extent;
} canvas_pass_config;

/* Rendering backend vtable (defined in backend.h). The canvas holds a
 * borrowed pointer to a stateless singleton; see backend_vk.c. */
typedef struct optics_gfx_canvas_backend optics_gfx_canvas_backend;

/* Forward declared so optics_gfx_canvas can hold a pointer-array to it
 * without depending on geometry layout. */
typedef struct optics_gfx_canvas_contour optics_gfx_canvas_contour;
typedef struct stroke_frame stroke_frame;

/* Forward declaration so we can hold pointers without dragging in
 * the (intentionally private) vertex layout from canvas.c. */
typedef struct optics_gfx_canvas_vertex optics_gfx_canvas_vertex;

/* Path verb stream. The full struct optics_gfx_path is in the Path section
 * below. */
typedef enum optics_gfx_path_op : uint32_t {
    OPTICS_GFX_PATH_MOVE = 0,
    OPTICS_GFX_PATH_LINE = 1,
    OPTICS_GFX_PATH_QUAD = 2,
    OPTICS_GFX_PATH_CUBIC = 3,
    OPTICS_GFX_PATH_CLOSE = 4,
} optics_gfx_path_op;

typedef struct optics_gfx_path_segment {
    uint32_t op;  /* optics_gfx_path_op */
    float pts[6]; /* up to a cubic's 3 control points */
} optics_gfx_path_segment;

/* ------------------------------------------------------------------ */
/*  Display-list segments (record / replay)                           */
/*                                                                    */
/*  A segment captures every backend submit (pipeline id, push        */
/*  constants, scissor, blend, vertex bytes) emitted between          */
/*  optics_gfx_canvas_cache_begin / optics_gfx_canvas_cache_end so an unchanged */
/*  subtree can be re-submitted later without re-running the emitter  */
/*  (text shaping, flattening, measure). Recording is passive: the  */
/*  draws are submitted live as usual while being captured.           */
/*                                                                    */
/*  Ownership: slots live in a fixed pool owned by the canvas, freed  */
/*  at optics_gfx_canvas_release. The public handle is a {slot, generation} */
/*  pair; release or LRU eviction bumps the generation so stale       */
/*  handles fail validation instead of replaying a reused slot. Total */
/*  recorded bytes are capped (LRU eviction), as is each segment.     */
/* ------------------------------------------------------------------ */

#define OPTICS_GFX_CANVAS_RECORD_DEPTH_CAP 16u            /* nested recordings  */
#define OPTICS_GFX_CANVAS_RECORD_SLOT_CAP 1024u           /* pool size          */
#define OPTICS_GFX_CANVAS_RECORD_TOTAL_BUDGET (16u << 20) /* bytes, all segs    */
#define OPTICS_GFX_CANVAS_RECORD_SEG_BUDGET (2u << 20)    /* bytes, one segment */
#define OPTICS_GFX_CANVAS_RECORD_IMG_CAP 32u              /* retained images    */
#define OPTICS_GFX_CANVAS_RECORD_SAMPLER_CAP 16u          /* retained samplers  */

/* Host-atlas generation registry size (one entry per live producer) and
 * the sentinel recorded when a glyph run carried no generation
 * extension — such ops replay unchecked, matching producers that never
 * rearrange their buffer in place. */
#define OPTICS_GFX_CANVAS_HOST_ATLAS_GEN_CAP 8u
#define OPTICS_GFX_HOST_ATLAS_UNVERSIONED UINT64_MAX

typedef struct optics_gfx_host_atlas_gen {
    const uint8_t *ptr;
    uint64_t gen;
} optics_gfx_host_atlas_gen;

enum optics_gfx_record_slot_state {
    OPTICS_GFX_RECORD_SLOT_FREE = 0,
    OPTICS_GFX_RECORD_SLOT_RECORDING,
    OPTICS_GFX_RECORD_SLOT_VALID,
    OPTICS_GFX_RECORD_SLOT_POISONED, /* recording aborted (budget/alloc); end_record discards */
};

/* Full definitions sit below optics_gfx_canvas_push (optics_gfx_record_op embeds it).
 * struct optics_gfx_canvas only holds pointers to these. */
typedef struct optics_gfx_record_op optics_gfx_record_op;
typedef struct optics_gfx_canvas_record_slot optics_gfx_canvas_record_slot;

struct optics_gfx_canvas {
    atomic_uint ref_count;
    optics_gfx_device *device;   /* retained */
    optics_gfx_surface *surface; /* retained */

    /* Rendering backend (borrowed singleton) plus its private per-canvas
     * state. All GPU resources (pipelines, working-space targets, formats)
     * live in backend_data; the front end never dereferences it. See
     * backend.h / backend_vk.c. */
    const optics_gfx_canvas_backend *backend;
    void *backend_data;

    /* Active recording state */
    optics_gfx_frame *frame;
    bool recording;
    bool pass_active;
    bool target_pass;         /* true: active pass renders into target, not the frame */
    optics_gfx_image *target; /* borrowed during begin_target..end_target */
    optics_gfx_target
        *bound_target; /* target leased exclusively for recording interval (ADR-0093) */
    optics_gfx_target *bound_cpu_target; /* CPU target receiving readback pixels on end */

    /* Sticky error for the active pass. Void draw calls cannot return a
     * contract violation directly, so checked pass termination exposes it
     * without relying on thread-local diagnostic polling. First error wins. */
    optics_gfx_result pass_error;
    optics_gfx_recti pass_scissor;

    /* Physical framebuffer size of the active pass, set by the backend at
     * begin_pass. build_push reads it for the NDC transform, so the front end
     * needs no surface handle while recording (the CPU backend has none). */
    uint32_t fb_width, fb_height;

    /* RenderPlan execution diagnostics (ADR-0089 / ADR-0092) */
    uint32_t pass_count;
    uint32_t layer_depth_peak;
    size_t transient_bytes_allocated;
    size_t transient_bytes_aliased_saved;

    /* RenderPlan node for the active Canvas pass, or UINT32_MAX when the pass
     * has no plan node (CPU canvas, no frame, or a failed compile gate).
     * Sampled-image reads are appended to this node while the pass records;
     * the node is validated again at end_pass before the output transform. */
    uint32_t plan_node;

    /* Content scale (device-pixel ratio). The base transform at index 0 is
     * this scale, so callers draw in logical units; 1.0 means logical ==
     * physical. glyph reads it via optics_gfx_canvas_get_scale. */
    float content_scale;

    /* Transform / clip stack. Index 0 is the content-scale base transform. */
    optics_gfx_canvas_state states[OPTICS_GFX_CANVAS_MAX_STATES];
    uint32_t state_top;

    /* The active pass's damage scope (ADR-0149). Copied from the normalized
     * config at begin_pass. `pass_damage_rects` is the canvas-owned backing
     * store: the caller's `optics_gfx_damage` array is only promised for the
     * `optics_gfx_canvas_begin` call, and every draw and `end_pass` consults this
     * scope long after that call returns. */
    canvas_pass_config pass_damage;
    optics_gfx_rect pass_damage_rects[OPTICS_GFX_DAMAGE_CAPACITY];

    /* Per-instance scratch buffers for fill/stroke. Moved off any
     * static / thread_local area so two canvases on two threads can
     * record concurrently without corrupting each other. */
    optics_gfx_point *scratch_pts;
    optics_gfx_canvas_vertex *scratch_verts;
    optics_gfx_canvas_contour *scratch_contours;
    stroke_frame *scratch_frames;
    uint32_t scratch_pts_cap;
    uint32_t scratch_verts_cap;
    uint32_t scratch_contours_cap;
    uint32_t scratch_frames_cap;
    /* Stroke outline pieces in physical pixels (geometry_stroke.c). Separate
     * from scratch_pts, which holds the flattened input while they are built. */
    optics_gfx_point *scratch_outline_pts;
    optics_gfx_canvas_contour *scratch_outline_contours;
    uint32_t scratch_outline_pts_cap;
    uint32_t scratch_outline_contours_cap;

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
     * optics_gfx_glyph_run_host_atlas_desc extension; OPTICS_GFX_HOST_ATLAS_UNVERSIONED
     * when the run carried none. Captured into record ops so replay can
     * refuse segments whose baked UVs predate an in-place rearrange. */
    uint64_t pending_host_atlas_gen;

    /* Newest content generation seen per host atlas buffer (see
     * optics_gfx_glyph_run_host_atlas_desc). draw_glyph_run refreshes an entry,
     * optics_gfx_canvas_cache_replay refuses a segment whose recorded generation is
     * older. Fixed-size, round-robin: one entry per live producer. */
    optics_gfx_host_atlas_gen host_atlas_gens[OPTICS_GFX_CANVAS_HOST_ATLAS_GEN_CAP];
    uint32_t host_atlas_gen_next; /* round-robin insert slot */

    /* Active blend mode for the next submit, set by submit_triangles*
     * from the paint (ADR: canvas blend modes). The GPU backend uses
     * it to key into the per-blend pipeline cache; the CPU backend
     * uses it to pick the compositing routine. STENCIL_WRITE ignores
     * it (color writes are off). Defaults to SRC_OVER. */
    optics_gfx_blend_mode pending_blend;

    /* Display-list segments (see "Display-list segments" above). The
     * slot pool is allocated lazily on the first optics_gfx_canvas_cache_begin
     * and freed at destroy; the stack tracks nested recordings. */
    optics_gfx_canvas_record_slot *record_slots;
    optics_gfx_canvas_record_slot *record_stack[OPTICS_GFX_CANVAS_RECORD_DEPTH_CAP];
    uint32_t record_depth;
    uint64_t record_tick; /* LRU clock */
    size_t record_bytes;  /* total bytes across VALID segments */
    /* Diagnostics: cumulative successful end_record / replay counts. */
    uint64_t records_created, records_replayed;
};

/* The smallest rectangle covering the pass's write scope, i.e. the
 * dynamic-rendering renderArea. Returns false when the scope is empty in a way
 * that is not "the whole destination" (an empty rect set that the caller also
 * refused as full extent, or a destination with no extent).
 *
 * The bounding box is a *superset*: it bounds where the load/store may touch,
 * while the scissor decides what each draw actually writes. Pixels inside the
 * box but outside every damage rect are preserved because nothing clears,
 * draws, or writes them back — see `backend_vk.c`'s per-rect clear, encode and
 * layer composite. */
static inline bool canvas_pass_render_bounds(const canvas_pass_config *config, uint32_t fb_width,
                                             uint32_t fb_height, optics_gfx_recti *out) {
    if (!config || !out || fb_width == 0 || fb_height == 0)
        return false;
    const optics_gfx_recti whole = {0, 0, fb_width, fb_height};
    if (!config->damage.rects || config->damage.count == 0 || config->clear_extent) {
        *out = whole;
        return true;
    }
    /* canvas.c has already degraded an over-capacity set to `clear_extent`, so a
     * non-covering scope here is always within the bound. Reading past it would be
     * a bug in that parser, not a caller error, and silently clamping would turn
     * that bug into stale pixels — assert instead. */
    const uint32_t count = config->damage.count;
    assert(count <= OPTICS_GFX_DAMAGE_CAPACITY && "damage set was not degraded by canvas.c");
    optics_gfx_recti bbox = {0, 0, 0, 0};
    for (uint32_t i = 0; i < count; i++) {
        const optics_gfx_rect r = config->damage.rects[i];
        if (!(r.w > 0.0f) || !(r.h > 0.0f))
            continue;
        /* Snap outward: a fractional edge must not shrink the region that has
         * to be preserved. */
        const int32_t x0 = (int32_t)floorf(r.x);
        const int32_t y0 = (int32_t)floorf(r.y);
        const int32_t x1 = (int32_t)ceilf(r.x + r.w);
        const int32_t y1 = (int32_t)ceilf(r.y + r.h);
        if (x1 <= 0 || y1 <= 0 || x0 >= (int32_t)fb_width || y0 >= (int32_t)fb_height)
            continue;
        const int32_t cx0 = x0 < 0 ? 0 : x0;
        const int32_t cy0 = y0 < 0 ? 0 : y0;
        const int32_t cx1 = x1 > (int32_t)fb_width ? (int32_t)fb_width : x1;
        const int32_t cy1 = y1 > (int32_t)fb_height ? (int32_t)fb_height : y1;
        if (cx1 <= cx0 || cy1 <= cy0)
            continue;
        if (bbox.w == 0 || bbox.h == 0) {
            bbox = (optics_gfx_recti){cx0, cy0, (uint32_t)(cx1 - cx0), (uint32_t)(cy1 - cy0)};
        } else {
            const int32_t ux0 = bbox.x < cx0 ? bbox.x : cx0;
            const int32_t uy0 = bbox.y < cy0 ? bbox.y : cy0;
            const int32_t ux1 =
                (int32_t)(bbox.x + (int32_t)bbox.w) > cx1 ? bbox.x + (int32_t)bbox.w : cx1;
            const int32_t uy1 =
                (int32_t)(bbox.y + (int32_t)bbox.h) > cy1 ? bbox.y + (int32_t)bbox.h : cy1;
            bbox = (optics_gfx_recti){ux0, uy0, (uint32_t)(ux1 - ux0), (uint32_t)(uy1 - uy0)};
        }
    }
    if (bbox.w == 0 || bbox.h == 0)
        return false;
    *out = bbox;
    return true;
}

/* True when the pass's scope is the whole destination, so single-renderArea
 * shortcuts (one clear, one seed blit, one encode) are exact. */
static inline bool canvas_pass_covers_extent(const canvas_pass_config *config) {
    return !config || !config->damage.rects || config->damage.count == 0 || config->clear_extent;
}

/* Insert integer rectangle `r` into `out[0..*count)` by subtracting every
 * existing rectangle so the output set is strictly pairwise-disjoint. Returns
 * false if the disjoint decomposition cannot fit in `capacity`. */
static inline bool canvas_append_disjoint_recti(optics_gfx_recti *out, uint32_t *count,
                                                uint32_t capacity, optics_gfx_recti r) {
    if (!out || capacity == 0 || r.w == 0 || r.h == 0)
        return false;

    enum { WORKLIST_CAP = OPTICS_GFX_DAMAGE_CAPACITY * 4u };
    optics_gfx_recti pending[WORKLIST_CAP];
    optics_gfx_recti next[WORKLIST_CAP];
    pending[0] = r;
    uint32_t pending_count = 1;

    const uint32_t existing_count = *count;
    for (uint32_t i = 0; i < existing_count && pending_count > 0; i++) {
        const optics_gfx_recti e = out[i];
        const int32_t ex0 = e.x;
        const int32_t ey0 = e.y;
        const int32_t ex1 = e.x + (int32_t)e.w;
        const int32_t ey1 = e.y + (int32_t)e.h;

        uint32_t next_count = 0;
        for (uint32_t p = 0; p < pending_count; p++) {
            const optics_gfx_recti c = pending[p];
            const int32_t cx0 = c.x;
            const int32_t cy0 = c.y;
            const int32_t cx1 = c.x + (int32_t)c.w;
            const int32_t cy1 = c.y + (int32_t)c.h;

            const int32_t ix0 = cx0 > ex0 ? cx0 : ex0;
            const int32_t iy0 = cy0 > ey0 ? cy0 : ey0;
            const int32_t ix1 = cx1 < ex1 ? cx1 : ex1;
            const int32_t iy1 = cy1 < ey1 ? cy1 : ey1;

            if (!(ix0 < ix1 && iy0 < iy1)) {
                if (next_count >= WORKLIST_CAP)
                    return false;
                next[next_count++] = c;
                continue;
            }
            if (cy0 < iy0) {
                if (next_count >= WORKLIST_CAP)
                    return false;
                next[next_count++] =
                    (optics_gfx_recti){cx0, cy0, (uint32_t)(cx1 - cx0), (uint32_t)(iy0 - cy0)};
            }
            if (iy1 < cy1) {
                if (next_count >= WORKLIST_CAP)
                    return false;
                next[next_count++] =
                    (optics_gfx_recti){cx0, iy1, (uint32_t)(cx1 - cx0), (uint32_t)(cy1 - iy1)};
            }
            if (cx0 < ix0) {
                if (next_count >= WORKLIST_CAP)
                    return false;
                next[next_count++] =
                    (optics_gfx_recti){cx0, iy0, (uint32_t)(ix0 - cx0), (uint32_t)(iy1 - iy0)};
            }
            if (ix1 < cx1) {
                if (next_count >= WORKLIST_CAP)
                    return false;
                next[next_count++] =
                    (optics_gfx_recti){ix1, iy0, (uint32_t)(cx1 - ix1), (uint32_t)(iy1 - iy0)};
            }
        }
        for (uint32_t p = 0; p < next_count; p++)
            pending[p] = next[p];
        pending_count = next_count;
    }

    if (pending_count == 0)
        return true;
    if (*count + pending_count > capacity)
        return false;
    for (uint32_t p = 0; p < pending_count; p++)
        out[(*count)++] = pending[p];
    return true;
}

/* Enumerate the pass's write scope as concrete, clipped, non-empty, strictly
 * pairwise-disjoint integer rectangles. Returns the number written (never more
 * than `capacity`).
 *
 * Every consumer that must touch *each* region separately — the LOAD seed, the
 * output encode, a layer composite — iterates this rather than the bounding box,
 * because writing the bounding box is what preserves garbage between two damage
 * bands and defeats the localisation. Pairwise-disjointness guarantees no pixel
 * is decoded or encoded twice when caller-supplied damage rectangles overlap or
 * share a rounded integer boundary. */
static inline uint32_t canvas_pass_areas(const canvas_pass_config *config, uint32_t fb_width,
                                         uint32_t fb_height, optics_gfx_recti *out,
                                         uint32_t capacity) {
    if (!config || !out || capacity == 0 || fb_width == 0 || fb_height == 0)
        return 0;
    if (canvas_pass_covers_extent(config)) {
        out[0] = (optics_gfx_recti){0, 0, fb_width, fb_height};
        return 1;
    }
    /* canvas.c has already degraded an over-capacity set to `clear_extent`, so a
     * non-covering scope here is always within the bound. Reading past it would be
     * a bug in that parser, not a caller error, and silently clamping would turn
     * that bug into stale pixels — assert instead. */
    const uint32_t count = config->damage.count;
    assert(count <= OPTICS_GFX_DAMAGE_CAPACITY && "damage set was not degraded by canvas.c");
    uint32_t n = 0;
    for (uint32_t i = 0; i < count; i++) {
        const optics_gfx_rect r = config->damage.rects[i];
        if (!(r.w > 0.0f) || !(r.h > 0.0f))
            continue;
        int32_t x0 = (int32_t)floorf(r.x);
        int32_t y0 = (int32_t)floorf(r.y);
        int32_t x1 = (int32_t)ceilf(r.x + r.w);
        int32_t y1 = (int32_t)ceilf(r.y + r.h);
        if (x0 < 0)
            x0 = 0;
        if (y0 < 0)
            y0 = 0;
        if (x1 > (int32_t)fb_width)
            x1 = (int32_t)fb_width;
        if (y1 > (int32_t)fb_height)
            y1 = (int32_t)fb_height;
        if (x1 <= x0 || y1 <= y0)
            continue;
        const optics_gfx_recti ri = {x0, y0, (uint32_t)(x1 - x0), (uint32_t)(y1 - y0)};
        if (!canvas_append_disjoint_recti(out, &n, capacity, ri)) {
            out[0] = (optics_gfx_recti){0, 0, fb_width, fb_height};
            return 1;
        }
    }
    return n;
}

/* Resolve the scissor for one draw: the union of the damage rects that
 * intersect `clip`, further intersected with `clip`. Returns false when the
 * draw touches no damage at all, which means it can be dropped.
 *
 * Union rather than "pick one" because a single draw may straddle two damage
 * rects. The union then also covers the gap between them, which is safe — a
 * scissored repaint is idempotent and over-covering never leaves stale pixels —
 * whereas walking rects per draw would multiply draws and break the vertex
 * batching that makes a frame cheap. One rect (the common case: a draw belongs
 * to one band) is exact. */
static inline bool canvas_damage_select(const canvas_pass_config *config, optics_gfx_recti clip,
                                        optics_gfx_recti *out) {
    if (!out || clip.w == 0 || clip.h == 0)
        return false;
    if (canvas_pass_covers_extent(config)) {
        *out = clip;
        return true;
    }
    /* canvas.c has already degraded an over-capacity set to `clear_extent`, so a
     * non-covering scope here is always within the bound. Reading past it would be
     * a bug in that parser, not a caller error, and silently clamping would turn
     * that bug into stale pixels — assert instead. */
    const uint32_t count = config->damage.count;
    assert(count <= OPTICS_GFX_DAMAGE_CAPACITY && "damage set was not degraded by canvas.c");
    int32_t x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    bool any = false;
    for (uint32_t i = 0; i < count; i++) {
        const optics_gfx_rect r = config->damage.rects[i];
        if (!(r.w > 0.0f) || !(r.h > 0.0f))
            continue;
        int32_t rx0 = (int32_t)floorf(r.x);
        int32_t ry0 = (int32_t)floorf(r.y);
        int32_t rx1 = (int32_t)ceilf(r.x + r.w);
        int32_t ry1 = (int32_t)ceilf(r.y + r.h);
        if (rx0 < clip.x)
            rx0 = clip.x;
        if (ry0 < clip.y)
            ry0 = clip.y;
        if (rx1 > clip.x + (int32_t)clip.w)
            rx1 = clip.x + (int32_t)clip.w;
        if (ry1 > clip.y + (int32_t)clip.h)
            ry1 = clip.y + (int32_t)clip.h;
        if (rx1 <= rx0 || ry1 <= ry0)
            continue;
        if (!any) {
            x0 = rx0;
            y0 = ry0;
            x1 = rx1;
            y1 = ry1;
            any = true;
        } else {
            if (rx0 < x0)
                x0 = rx0;
            if (ry0 < y0)
                y0 = ry0;
            if (rx1 > x1)
                x1 = rx1;
            if (ry1 > y1)
                y1 = ry1;
        }
    }
    if (!any)
        return false;
    *out = (optics_gfx_recti){x0, y0, (uint32_t)(x1 - x0), (uint32_t)(y1 - y0)};
    return true;
}

/* Vertex layout matches std430 buffer_reference in
 * src/canvas/shaders/canvas_solid.vert. The forward declaration above
 * uses this same name; the struct body is defined here. */
struct optics_gfx_canvas_vertex {
    float pos[2];
    uint32_t color;
    uint32_t _pad;
};

/* Push constants for the canvas pipelines. The solid pipeline only
 * touches the first 24 bytes (the rest is uninitialised but harmless).
 * The gradient pipeline reads everything. Layout must match the
 * std140-style block in src/canvas/shaders/canvas_gradient.frag. */
typedef struct optics_gfx_canvas_gradient_stop_pc {
    float t;
    uint32_t color;
} optics_gfx_canvas_gradient_stop_pc;

typedef struct optics_gfx_canvas_push {
    uint64_t verts_address;        /* offset  0 */
    float inv_window_size[2];      /* offset  8 */
    uint64_t color_params_address; /* offset 16 — optics_gfx_image_color_params BDA,
                                    * 0 = format-default content space (ADR-0069) */
    uint32_t kind;                 /* offset 24 (0 solid, 1 linear, 2 radial, 3 image) */
    uint32_t num_stops;            /* offset 28 */
    union {
        struct {
            float grad_from[2]; /* offset 32 */
            float grad_to[2];   /* offset 40 */
            float grad_radius;  /* offset 48 */
            float _pad1;        /* offset 52 */
            optics_gfx_canvas_gradient_stop_pc
                stops[OPTICS_GFX_GRADIENT_MAX_STOPS]; /* offset 56, size 64 */
            /* Image and vector draw extension (read by image and vector fragment shaders). */
            union {
                struct {
                    uint32_t image_handle;   /* offset 120 */
                    uint32_t sampler_handle; /* offset 124 */
                    float image_dst[4];      /* offset 128 */
                    float image_src[4];      /* offset 144 — u, v, du, dv (normalised) */
                };
                struct {
                    uint64_t tile_dispatch;  /* offset 120 — BDA to optics_gfx_gpu_tile_dispatch */
                    uint32_t image_handle;   /* offset 128 — image pattern bindless handle */
                    uint32_t sampler_handle; /* offset 132 — image pattern sampler handle */
                    float uv_row0[2];        /* offset 136 — du/dx, du/dy */
                    float uv_row1[2];        /* offset 144 — dv/dx, dv/dy */
                    float uv_off[2];         /* offset 152 — u0, v0 */
                } vector;
            };
        };
        struct {
            float inv[4];    /* offset 32..48: inverse linear a, b, c, d (row-major) */
            float _pad0[4];  /* offset 48..64 */
            float radii[4];  /* offset 64..80: corner radii (tl, tr, br, bl) */
            float _pad1[12]; /* offset 80..128 */
            float box[4];    /* offset 128..144: cx, cy, hx, hy */
            float stroke_hw; /* offset 144: ring half-width, shape units */
            float curvature; /* offset 148: [0, 1] circular..L4 squircle */
            float aa;        /* offset 152: shape units per screen pixel */
            float _pad2;     /* offset 156 */
        } sdf;
    };
} optics_gfx_canvas_push;

static_assert(sizeof(optics_gfx_canvas_push) == 160,
              "optics_gfx_canvas_push must remain 160 bytes");
static_assert(offsetof(optics_gfx_canvas_push, sdf.inv) == 32, "sdf.inv offset");
static_assert(offsetof(optics_gfx_canvas_push, sdf.radii) == 64, "sdf.radii offset");
static_assert(offsetof(optics_gfx_canvas_push, sdf.box) == 128, "sdf.box offset");
static_assert(offsetof(optics_gfx_canvas_push, sdf.stroke_hw) == 144, "sdf.stroke_hw offset");

/* Push constants for CANVAS_PIPE_OUTPUT (ADR-0069). Layout matches the
 * push block in shaders/canvas_output.frag and fits inside the shared
 * canvas layout's 160-byte range. */
typedef struct optics_gfx_output_push {
    float primaries[3][4]; /* column-major mat3, one column per vec4 */
    uint32_t image_handle;
    uint32_t sampler_handle;
    uint32_t transfer;    /* optics_gfx_transfer_func of the encoded side */
    uint32_t flags;       /* bit0 decode, bit1 no-dither */
    float gamma;          /* OPTICS_GFX_TRANSFER_GAMMA exponent */
    float dither_levels;  /* 255 / 1023; ignored when no-dither */
    float sdr_white_nits; /* Phase 3 tone mapping; 203 default */
    /* Was `_pad`. The bucketed target-attachment pool can serve a pass
     * from an intermediate larger than the pass extent; the blit then
     * scales its sampling UV by extent/image to read the right sub-rect.
     * Exact-size passes pass {1,1}. */
    float uv_scale[2]; /* pass extent / intermediate extent */
} optics_gfx_output_push;

#define OPTICS_GFX_OUTPUT_F_DECODE 0x1u
#define OPTICS_GFX_OUTPUT_F_NO_DITHER 0x2u

/* ------------------------------------------------------------------ */
/*  Display-list segment bodies (forward-declared above)              */
/* ------------------------------------------------------------------ */

struct optics_gfx_record_op {
    optics_gfx_canvas_push push; /* complete draw state (incl. baked inv_window_size) */
    optics_gfx_recti scissor;    /* effective scissor at emit time                    */
    uint32_t pipe_id;            /* canvas_pipe_id                                    */
    uint32_t blend;              /* c->pending_blend at emit time                     */
    uint32_t vert_offset;        /* into the segment's vertex buffer                  */
    uint32_t vert_count;
    /* Host R8 glyph atlas borrowed by the CPU backend (ADR-0019); NULL for
     * non-glyph ops. Points into glyph's persistent atlas buffer.
     * host_atlas_gen is the producer generation at capture time
     * (OPTICS_GFX_HOST_ATLAS_UNVERSIONED when the run carried no extension):
     * replay refuses the segment when the canvas has since seen a newer
     * generation for this buffer (texels rearranged, baked UVs stale). */
    const uint8_t *host_atlas;
    uint32_t host_atlas_w;
    uint32_t host_atlas_h;
    uint64_t host_atlas_gen;
};

struct optics_gfx_canvas_record_slot {
    optics_gfx_canvas *owner;
    uint64_t generation; /* bumped on release/evict/discard; invalidates handles */
    uint64_t last_used;  /* LRU tick */
    uint32_t state;      /* enum optics_gfx_record_slot_state */

    /* Replay anchor: the segment is only valid under the exact canvas state
     * it was recorded against — same framebuffer extent (push constants bake
     * inv_window_size), same absolute transform (vertices are baked in
     * physical pixels), same incoming scissor. */
    optics_gfx_mat3x2 anchor_transform;
    optics_gfx_recti anchor_scissor;
    uint32_t fb_w, fb_h;

    optics_gfx_record_op *ops;
    uint32_t op_count, op_cap;
    optics_gfx_canvas_vertex *verts;
    uint32_t vert_count, vert_cap;

    /* Resources referenced by recorded image/glyph draws, retained so
     * recorded bindless handles can never dangle or be recycled. */
    optics_gfx_image *images[OPTICS_GFX_CANVAS_RECORD_IMG_CAP];
    uint32_t image_count;
    optics_gfx_sampler *samplers[OPTICS_GFX_CANVAS_RECORD_SAMPLER_CAP];
    uint32_t sampler_count;

    size_t bytes; /* ops + verts, for budget accounting */
};

/* ------------------------------------------------------------------ */
/*  Image (internal)                                                  */
/*                                                                      */
/*  struct optics_gfx_image + optics_gfx_image_create (OPTICS_GFX_IMAGE_USAGE_COMPUTE_WRITE) live in
 */
/*  image_internal.h so the effect module can depend on the image      */
/*  struct without dragging in canvas pipelines, push constants,       */
/*  geometry helpers, or path/paint internals.                         */
/* ------------------------------------------------------------------ */

#include "../core/image_internal.h"

/* ------------------------------------------------------------------ */
/*  Path                                                              */
/* ------------------------------------------------------------------ */

/* optics_gfx_path_op / optics_gfx_path_segment are declared near the top. */
struct optics_gfx_path {
    optics_gfx_path_segment *segments;
    uint32_t capacity;
    uint32_t count;
    uint32_t dropped;
    float cursor_x;
    float cursor_y;
    optics_gfx_arena *arena;
};

/* ------------------------------------------------------------------ */
/*  Renderer (pipeline cache + draw submission)                       */
/* ------------------------------------------------------------------ */

/* Internal pipeline selector. Single-sample, no stencil attachment. */
typedef enum canvas_pipe_id : uint32_t {
    CANVAS_PIPE_SOLID = 0,
    CANVAS_PIPE_GRADIENT,
    CANVAS_PIPE_IMAGE,
    CANVAS_PIPE_GLYPH,  /* batched glyph run (ADR-0010) */
    CANVAS_PIPE_SDF,    /* analytic rounded-rect / circle (fill + ring) */
    CANVAS_PIPE_VECTOR, /* ADR-0116: analytic fragment vector rasterizer */
    /* Working-space intermediate -> destination output transform
     * (ADR-0069). Never emitted by draws; backend_vk records it in
     * end_pass (encode) and in the LOAD seed blit (decode). Always
     * built 1x / blend-off. */
    CANVAS_PIPE_OUTPUT,
    CANVAS_PIPE_COUNT,
} canvas_pipe_id;

/* optics_gfx_canvas_push.kind high bits: content decode hints (ADR-0069).
 * Set when the sampled 8-bit UNORM image holds sRGB-encoded content
 * that hardware views cannot decode for us. */
#define OPTICS_GFX_CANVAS_PUSH_DECODE_SRGB 0x100u
/* color_params_address holds a live buffer reference (ADR-0070). */
#define OPTICS_GFX_CANVAS_PUSH_HAS_COLOR_PARAMS 0x200u

optics_gfx_result get_canvas_pipeline_id(optics_gfx_device *device, VkFormat color_format,
                                         canvas_pipe_id id, optics_gfx_blend_mode blend,
                                         VkPipelineLayout *out_layout, VkPipeline *out_pipeline);

typedef enum canvas_tile_pipe_id : uint32_t {
    CANVAS_TILE_PIPE_BIN_COUNT = 0,
    CANVAS_TILE_PIPE_SCAN_OFFSETS = 1,
    CANVAS_TILE_PIPE_SCAN_BACKDROP = 2,
    CANVAS_TILE_PIPE_BIN_SCATTER = 3,
    CANVAS_TILE_PIPE_COUNT = 4,
} canvas_tile_pipe_id;

optics_gfx_result
canvas_get_tile_compute_pipelines(optics_gfx_device *device,
                                  optics_gfx_compute_pipeline *out[CANVAS_TILE_PIPE_COUNT]);

/* Software-backend constructor behind optics_gfx_canvas_create (backend_cpu.c). */
OPTICS_GFX_NODISCARD optics_gfx_result optics_gfx_canvas_cpu_create(uint32_t width, uint32_t height,
                                                                    float scale,
                                                                    optics_gfx_canvas **out);

void *canvas_state_get_or_init(optics_gfx_device *d);

/* Allocation shims + scratch lifecycle shared by the GPU and CPU canvas
 * constructors (canvas.c). A NULL device routes to the C allocator. */
void *optics_gfx_canvas_alloc(optics_gfx_device *d, size_t n);
void optics_gfx_canvas_free(optics_gfx_device *d, void *p);
bool optics_gfx_canvas_alloc_scratch(optics_gfx_canvas *c);
void optics_gfx_canvas_free_scratch(optics_gfx_canvas *c);

void push_vertex(optics_gfx_canvas_vertex *v, optics_gfx_point p, optics_gfx_mat3x2 tx,
                 optics_gfx_color c);
void build_push(optics_gfx_canvas *c, const canvas_paint *paint, optics_gfx_canvas_push *out);
bool ensure_pipeline_bound(optics_gfx_canvas *c, canvas_paint_kind kind);
bool ensure_pipeline_bound_id(optics_gfx_canvas *c, canvas_pipe_id id);
void submit_triangles(optics_gfx_canvas *c, const canvas_paint *paint,
                      const optics_gfx_canvas_vertex *verts, uint32_t vertex_count);
void submit_triangles_id(optics_gfx_canvas *c, const canvas_paint *paint, canvas_pipe_id id,
                         const optics_gfx_canvas_vertex *verts, uint32_t vertex_count);
void draw_image_with_sampler_handle(optics_gfx_canvas *c, optics_gfx_image *img,
                                    uint32_t image_handle, optics_gfx_sampler *sampler,
                                    optics_gfx_bindless_handle sh, optics_gfx_rect dst,
                                    optics_gfx_rect src, optics_gfx_color tint,
                                    optics_gfx_blend_mode blend, uint32_t kind);

/* ------------------------------------------------------------------ */
/*  Display-list record/replay (record.c)                             */
/* ------------------------------------------------------------------ */

/* Single submission choke point: capture into every active recording
 * (when any), then hand the batch to the backend. All front-end draw
 * paths route through here instead of calling backend->submit directly
 * so a recording never misses a draw. */
void canvas_emit(optics_gfx_canvas *c, canvas_pipe_id id, const optics_gfx_canvas_push *push,
                 const optics_gfx_canvas_vertex *verts, uint32_t vertex_count);

/* Retain `img` in every active recording so recorded bindless handles
 * stay valid until the segment is released/evicted. No-op without an
 * active recording, a device, or an image. */
void canvas_record_retain_image(optics_gfx_canvas *c, optics_gfx_image *img);
void canvas_record_retain_sampler(optics_gfx_canvas *c, optics_gfx_sampler *sampler);
/* Retain every sampled image through the current frame fence; imported
 * dma-bufs additionally receive ownership-transfer bookkeeping. */
bool canvas_track_foreign_image(optics_gfx_canvas *c, optics_gfx_image *img);
bool canvas_track_sampler(optics_gfx_canvas *c, optics_gfx_sampler *sampler);
bool canvas_track_display_list(optics_gfx_canvas *c, const optics_gfx_display_list *list);

/* Record the newest producer generation seen for a host coverage buffer
 * (optics_gfx_glyph_run_host_atlas_desc); replay consults it to refuse segments
 * captured before an in-place atlas rearrange. */
void canvas_host_atlas_gen_track(optics_gfx_canvas *c, const uint8_t *ptr, uint64_t gen);

/* Release every slot's buffers + retained images and free the pool.
 * Called from optics_gfx_canvas_release. */
void canvas_record_pool_destroy(optics_gfx_canvas *c);

/* ------------------------------------------------------------------ */
/*  Flattening                                                        */
/* ------------------------------------------------------------------ */

typedef struct optics_gfx_canvas_contour {
    uint32_t start; /* index in the points array */
    uint32_t count;
    bool closed;
    float first_x, first_y;
} optics_gfx_canvas_contour;

typedef struct flatten_multi {
    uint32_t point_count;
    uint32_t contour_count;
} flatten_multi;

/* Coverage rasterizer (ADR-0119 S1): exact per-pixel area of the region the
 * fill rule selects, from flattened contours. Returns OPTICS_GFX_OK, or an explicit
 * error when the split budget is exhausted (never silently wrong coverage). */
OPTICS_GFX_NODISCARD optics_gfx_result canvas_coverage_rasterize(
    const optics_gfx_point *pts, const optics_gfx_canvas_contour *cons, uint32_t ncons,
    optics_gfx_fill_rule rule, float ox, float oy, uint32_t w, uint32_t h, float *out_cov);

typedef bool (*optics_gfx_flatten_grow_fn)(void *ctx, uint32_t need_pts, uint32_t need_cons,
                                           optics_gfx_point **pts, uint32_t *pts_cap,
                                           optics_gfx_canvas_contour **cons, uint32_t *cons_cap);

flatten_multi flatten_path_to_contours(const optics_gfx_path *p, float pixel_scale,
                                       optics_gfx_point *out_pts, uint32_t pts_cap,
                                       optics_gfx_canvas_contour *out_cons, uint32_t cons_cap);
flatten_multi flatten_path_to_contours_dyn(const optics_gfx_path *p, float pixel_scale,
                                           optics_gfx_point *out_pts, uint32_t pts_cap,
                                           optics_gfx_canvas_contour *out_cons, uint32_t cons_cap,
                                           optics_gfx_flatten_grow_fn grow, void *grow_ctx);
flatten_multi canvas_flatten_path(optics_gfx_canvas *c, const optics_gfx_path *p,
                                  float pixel_scale);

bool canvas_ensure_pts(optics_gfx_canvas *c, uint32_t needed);
bool canvas_ensure_contours(optics_gfx_canvas *c, uint32_t needed);
bool canvas_ensure_verts(optics_gfx_canvas *c, uint32_t needed);
bool canvas_ensure_frames(optics_gfx_canvas *c, uint32_t needed);
bool canvas_ensure_outline(optics_gfx_canvas *c, uint32_t needed_pts, uint32_t needed_contours);

/* Operator-norm upper bound of `m`'s 2×2 linear part. Callers pass
 * the result as `pixel_scale` to the flatteners so the per-pixel
 * tolerance survives non-identity transforms. */
float optics_gfx_canvas_mat3x2_pixel_scale(optics_gfx_mat3x2 m);

/* ------------------------------------------------------------------ */
/*  Stroke                                                            */
/* ------------------------------------------------------------------ */

typedef struct stroke_frame {
    optics_gfx_point left;  /* offset by +half along the bisector (or n_in for endpoints) */
    optics_gfx_point right; /* offset by -half */
    optics_gfx_point n_in;  /* left normal of incoming segment */
    optics_gfx_point n_out; /* left normal of outgoing segment */
    bool miter_ok;          /* true → left/right are a single bisector point */
    bool turning_left;      /* outside of the turn is on the left side */
} stroke_frame;

stroke_frame compute_frame(optics_gfx_point prev, optics_gfx_point pt, optics_gfx_point next,
                           float half, float miter_limit, bool has_prev, bool has_next);

/* ------------------------------------------------------------------ */
/*  Point helpers                                                     */
/* ------------------------------------------------------------------ */

static inline optics_gfx_point pt_add(optics_gfx_point a, optics_gfx_point b) {
    return (optics_gfx_point){a.x + b.x, a.y + b.y};
}
static inline optics_gfx_point pt_sub(optics_gfx_point a, optics_gfx_point b) {
    return (optics_gfx_point){a.x - b.x, a.y - b.y};
}
static inline optics_gfx_point pt_scale(optics_gfx_point a, float k) {
    return (optics_gfx_point){a.x * k, a.y * k};
}

/* ------------------------------------------------------------------ */
/*  Internal rasterization primitives (RFC-0094)                      */
/* ------------------------------------------------------------------ */

void canvas_fill_rect_internal(optics_gfx_canvas *c, optics_gfx_rect r, const canvas_paint *paint);
void canvas_fill_path_internal(optics_gfx_canvas *c, const optics_gfx_path *p,
                               const canvas_paint *paint);
void canvas_stroke_path_internal(optics_gfx_canvas *c, const optics_gfx_path *p,
                                 const canvas_paint *paint);

#endif /* OPTICS_GFX_CANVAS_INTERNAL_H */
