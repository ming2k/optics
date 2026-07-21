/*
 * lens internal header — cross-module private types and `lensi_*`
 * symbols. Not installed, not exported. The `lensi_*` prefix marks
 * any symbol that is internal to the library; the public surface is
 * the `lens_*` prefix in <lens/lens.h>. See docs/dev/project-layout.md.
 */

#ifndef LENSI_INTERNAL_H
#define LENSI_INTERNAL_H

#include <flux-text/text.h>
#include <lens/icon.h>
#include <lens/lens.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/*  Tunables                                                          */
/* ------------------------------------------------------------------ */

#define LENSI_DEFAULT_ARENA_BYTES (1u << 20) /* 1 MiB per-frame arena */
#define LENSI_DEFAULT_STORE_CAP 256u         /* initial hash slots    */
#define LENSI_LEAVE_GRACE_FRAMES 8u          /* reap delay (ADR-0027) */
#define LENSI_ID_STACK_MAX 64u
#define LENSI_CONTAINER_STACK_MAX 64u
#define LENSI_PASTE_MAX 1024u /* clipboard staging (ADR-0036) */
#define LENSI_OVERLAY_MAX 8u  /* max simultaneously-open overlays (ADR-0037) */

/* ------------------------------------------------------------------ */
/*  Draw command (ADR-0030) — coordinates relative to the node box    */
/* ------------------------------------------------------------------ */

typedef enum lens_draw_kind {
    LENS_DRAW_RECT = 0,
    LENS_DRAW_BORDER = 1,
    LENS_DRAW_TEXT = 2,
    LENS_DRAW_IMAGE = 3,
    LENS_DRAW_CLIP_PUSH = 4,
    LENS_DRAW_CLIP_POP = 5,
    LENS_DRAW_ICON = 6,
    LENS_DRAW_CONNECTED_TAB = 7,
    LENS_DRAW_TAB_INDICATOR = 8,
} lens_draw_kind;

enum {
    LENSI_TAB_CONNECT_LEFT = 1u << 0,
    LENSI_TAB_CONNECT_RIGHT = 1u << 1,
};

typedef struct lens_draw_cmd {
    lens_draw_kind kind;
    /* Relative to the node's final_rect. With an explicit positive width,
     * a negative x anchors the rectangle to the trailing edge. */
    flux_rect rel;
    flux_color color;
    float radius;
    float width;      /* border width */
    const char *text; /* LENS_DRAW_TEXT: arena-copied utf8 */
    float text_size;
    float text_weight; /* 0 = use theme default */
    int32_t icon_id;   /* LENS_DRAW_ICON: enum lens_icon_id */
    uint32_t flags;    /* kind-specific flags */
    flux_image *image; /* LENS_DRAW_IMAGE: host-owned texture, borrowed
                          for the frame; must outlive lens_render */
} lens_draw_cmd;

enum {
    LENSI_ICON_RENDER_STROKE = 0,
    LENSI_ICON_RENDER_FILL = 1,
};

/* Generated beside lens_icon_table from the vendored SVG source style. */
extern const uint8_t lens_icon_render_modes[LENS_ICON_COUNT];

/* ------------------------------------------------------------------ */
/*  Retained node slot (ADR-0027)                                     */
/* ------------------------------------------------------------------ */

struct lens_node {
    lens_id id;
    lens *ui; /* owning context (for lens_node_state) */

    /* lifecycle (persistent) */
    uint64_t last_seen;      /* frame stamp */
    uint32_t leaving_frames; /* grace countdown when LEAVING */
    lens_node_phase phase;
    bool has_prev; /* prev_rect is valid */

    /* tree (rebuilt each frame) */
    lens_node *parent;
    lens_node *first_child;
    lens_node *last_child;
    lens_node *next_sibling;
    uint32_t child_count;
    uint32_t child_seq; /* sibling-disambiguation counter */

    /* layout inputs (per frame) */
    bool is_container;
    bool is_scroll;
    bool is_overlay;     /* a floating layer (ADR-0037)       */
    bool is_panel_layer; /* persistent panel (no flip/dismiss) */
    bool is_centered;    /* center on display instead of anchor (modal, ADR-0039) */
    bool dismissable; /* overlay may be closed by Esc/click-outside; false = modal-pinned (ADR-0039)
                       */
    flux_rect overlay_anchor; /* set by lens_overlay_begin/lens_layer_begin */
    flux_rect overlay_bounds; /* optional placement/render boundary inherited from an owner */
    bool has_overlay_bounds;
    lens_axis axis;
    float gap, pad;
    lens_align cross;
    float flex_grow;
    float fixed_w, fixed_h; /* 0 = intrinsic */
    float min_w, max_w;     /* 0 = unconstrained */
    float min_h, max_h;     /* 0 = unconstrained */
    float scroll_x, scroll_y;
    float scroll_gutter; /* trailing viewport space reserved for scrollbar */

    /* layout outputs */
    flux_point measured;  /* measure pass */
    flux_rect final_rect; /* arrange pass (this frame) */
    flux_rect prev_rect;  /* last frame's final_rect (hit-test) */

    /* draw list (per frame, arena-allocated) */
    lens_draw_cmd *cmds;
    uint32_t cmd_count, cmd_cap;
    uint32_t cmd_hash;      /* rolling hash of this frame's cmds */
    uint32_t last_cmd_hash; /* previous frame's cmd_hash */

    /* accessibility semantics (per frame; ADR-0035) */
    lens_semantics semantics;

    /* animation (persistent) */
    float hover_t;
    float active_t;
    float last_hover_t;
    float last_active_t;

    /* damage tracking (per frame) */
    bool subtree_changed;

    /* persistent per-node user state (lens_node_state) */
    void *state;
    size_t state_bytes;
};

/* ------------------------------------------------------------------ */
/*  Scroll state (shared between scroll.c and solve.c)                */
/* ------------------------------------------------------------------ */

typedef struct lens_scroll_state {
    float offset_x, offset_y; /* persistent scroll offsets */
    /* geometry from last frame's clamp pass, used for thumb hit-testing */
    float thumb_y, thumb_h;  /* thumb position & height in node-local space */
    float track_len;         /* draggable track length */
    float scroll_range;      /* max scroll offset */
    bool dragging;           /* thumb is being dragged */
    float drag_start_offset; /* offset_y when drag began */
    float drag_start_y;      /* cursor.y when drag began */
    bool hovering;           /* cursor over the track this frame (hover styling) */
} lens_scroll_state;

/* ------------------------------------------------------------------ */
/*  Open-addressing store: lens_id -> lens_node* (ADR-0027)               */
/* ------------------------------------------------------------------ */

typedef struct lens_store_slot {
    lens_id id; /* 0 = empty */
    lens_node *node;
} lens_store_slot;

typedef struct lens_store {
    lens_store_slot *slots;
    uint32_t cap; /* power of two */
    uint32_t count;
} lens_store;

/* ------------------------------------------------------------------ */
/*  Context (ADR-0024, ADR-0032)                                      */
/* ------------------------------------------------------------------ */

struct lens {
    flux_device *device; /* may be NULL (headless) */
    lens_theme theme;
    float scale;      /* device-pixel scale (HiDPI); 1.0 by default */
    flux_arena arena; /* per-frame; reset each lens_begin */
    flux_text *text;  /* shared text engine (flux-text); never NULL
                       * after create — degrades to mono internally */

    lens_store store;
    lens_node *root;
    uint64_t frame;

    lens_input input;  /* copy for the frame */
    bool overflow;     /* arena overflowed this frame */
    bool anim_pending; /* an eased value is still in transit this
                        * frame; the host should schedule another
                        * frame so the animation can settle even
                        * without further input (see lens_anim_pending) */
    bool reduced_motion; /* accessibility: when set, every eased value
                          * resolves to its target in one frame (see
                          * lens_set_reduced_motion) */

    /* id stack (ADR-0026) */
    lens_id id_stack[LENSI_ID_STACK_MAX];
    uint32_t id_top; /* count; top = id_stack[id_top-1] */

    /* container stack (layout build) */
    lens_node *cont_stack[LENSI_CONTAINER_STACK_MAX];
    uint32_t cont_top;

    /* pending modifiers for the next widget */
    float next_flex;
    float next_w, next_h;
    bool have_next_flex, have_next_size;
    bool next_disabled;
    bool next_error;
    const char *next_placeholder;

    bool click_hit_focusable; /* mouse pressed inside a focusable widget */

    /* interaction state (ADR-0029) */
    lens_id hot_id;    /* hovered this frame */
    lens_id active_id; /* captured (e.g. dragging) */
    lens_id focused_id;
    lens_id scroll_hot_id; /* deepest scroll container under cursor */
    lens_cursor_hint cursor_hint; /* semantic cursor for the top hovered control */
    lens_response last_response;
    lens_node *last_node; /* most recently linked node (for lens_a11y) */

    /* tab focus order, collected during build (arena) */
    lens_id *tab_order;
    uint32_t tab_count, tab_cap;

    /* clipboard + IME (ADR-0036) */
    lens_clipboard clipboard;
    flux_rect caret_rect; /* set by the focused text widget */
    char paste_buf[LENSI_PASTE_MAX];
    uint32_t paste_len; /* 0 = nothing pending */

    /* overlay layer (ADR-0037) */
    struct lens_overlay_slot {
        lens_id id;
        uint64_t opened_frame; /* dismiss grace: same-frame open ignored */
        bool dismissable;      /* false = Escape/click-outside leave it (modal, ADR-0039) */
    } open_overlays[LENSI_OVERLAY_MAX];
    uint32_t open_overlay_count;
    lens_node **overlay_layers; /* per-frame, arena-backed */
    uint32_t overlay_layer_count;
    uint32_t overlay_layer_cap;
    /* Floating layers as of the END of the previous frame, kept across
     * the arena reset as plain ids so eclipse hit-testing covers base
     * widgets built BEFORE the layer re-registers this frame (the common
     * case: popups are declared after the content they cover). Without
     * this the eclipse only applied to widgets declared after the layer
     * in build order, and clicks fell through every popup to tables and
     * scroll areas beneath. */
    lens_id prev_overlay_layer_ids[LENSI_OVERLAY_MAX];
    uint32_t prev_overlay_layer_count;

    /* modal focus trap (ADR-0039). When modal_active, Tab cycling is
     * clamped to [modal_tab_lo, modal_tab_hi) — the tab_order slice
     * recorded during the modal body build. */
    bool modal_active;
    uint32_t modal_tab_lo;
    uint32_t modal_tab_hi;

    /* tooltip (frame-scoped) */
    struct {
        bool active;
        flux_rect anchor;
        char text[128];
    } tooltip;
};

/* ================================================================== */
/*  Internal API                                                      */
/* ================================================================== */

/* allocator (persistent store) — honours the device's flux_allocator,
 * or libc malloc when headless. */
void *lensi_alloc(lens *ui, size_t bytes);
void lensi_free(lens *ui, void *ptr);

/* identity (id.c) */
uint64_t lensi_hash(const void *data, size_t len, uint64_t seed);
lens_id lensi_id_top(const lens *ui);
lens_id lensi_gen_widget_id(lens *ui, const char *label);
lens_id lensi_gen_container_id(lens *ui, const char *kind);
size_t lensi_label_visible_len(const char *label);

/* store (store.c) */
flux_result lensi_store_init(lens *ui, uint32_t cap);
void lensi_store_destroy(lens *ui);
lens_node *lensi_store_find(const lens *ui, lens_id id);
lens_node *lensi_store_touch(lens *ui, lens_id id); /* find-or-create, frame-reset */
void lensi_store_reap(lens *ui);                    /* phase + GC at frame end */

/* node (node.c) */
void lensi_node_reset_frame(lens_node *n);

/* accessibility (a11y/semantics.c) — set a node's semantics for this
 * frame; `name` is truncated at "##" and `name`/`value` are arena-copied
 * so they survive the post-end walk. */
void lensi_node_semantics(lens *ui, lens_node *n, lens_role role, const char *name,
                          const char *value, uint32_t flags);

/* tree (tree.c) */
lens_node *lensi_open_container(lens *ui);     /* current open container */
void lensi_link_child(lens *ui, lens_node *n); /* link into open container */
void lensi_open_container_push(lens *ui, lens_node *n);
void lensi_open_container_pop(lens *ui);

/* descriptor plumbing (tree.c) — used by the *_ex widget wrappers. */
void lensi_apply_box(lens *ui, lens_box box); /* stage flex/size/disabled/error */
void lensi_set_placeholder(lens *ui, const char *text);
void lensi_tooltip(lens *ui, const char *text); /* anchored to last widget */

/* layout (solve.c) */
void lensi_layout_solve(lens *ui);
void lensi_layout_subtree(lens_node *n, flux_rect rect); /* used by overlay */
void lensi_scroll_clamp(lens *ui);
void lensi_scroll_clamp_subtree(lens_node *n); /* overlay sub-roots, post-placement */

/* input / interaction (input.c, focus.c) */
lens_response lensi_interact(lens *ui, lens_node *n, bool focusable, bool disabled);
void lensi_focus_tab(lens *ui);

/* clipboard + IME (clipboard.c, ADR-0036) — internal helpers for text
 * widgets (the consumer that will land with lens_text_input). */
void lensi_set_caret_rect(lens *ui, flux_rect r);
/* Borrow + clear any pending paste. Returns NULL when none. The buffer
 * is valid for the rest of the frame. */
const char *lensi_take_paste(lens *ui, uint32_t *out_len);

/* overlay layer (overlay.c, ADR-0037) — shared with persistent panels
 * (lens_layer_begin). Both register positional sub-roots in the same
 * per-frame layer array; only overlays track open state and dismissal. */
bool lensi_overlay_is_open_id(const lens *ui, lens_id id);
void lensi_overlay_open_id_pub(lens *ui, lens_id id, bool dismissable); /* ADR-0040 menu internal */
void lensi_overlay_constrain_current(lens *ui, flux_rect bounds);
void lensi_overlay_layout(lens *ui);                                    /* post-arrange placement */
void lensi_overlay_render(lens *ui, flux_canvas *canvas);
void lensi_overlay_dismiss(lens *ui); /* click-outside + Esc (overlays only) */
/* Cursor sits inside any rendered floating layer (overlay or panel),
 * using last-frame geometry. Used by lensi_interact to eclipse base
 * widgets under a popup or chrome panel. */
bool lensi_point_in_floating_layer(const lens *ui, flux_point p);
/* True when a floating layer covers the cursor for a widget that does
 * NOT belong to that layer. Widgets with their own hit-testing
 * (table rows, scrollbars, wheel routing) must check this in addition
 * to lensi_interact so popups eclipse them too. */
bool lensi_widget_eclipsed(const lens *ui, const lens_node *n);
/* True when point `p` falls outside the viewport of any scroll ancestor
 * of `n`. Scroll containers clip their children's RENDERING to the
 * viewport; hit-testing must apply the same clip, otherwise children
 * scrolled out of view stay hoverable/clickable through whatever is
 * painted over them (a queue item folded below the viewport edge
 * reacting under the player bar). Every interactive hit-test must
 * consult this in addition to the widget's own rect. */
bool lensi_point_clipped_by_scroll(const lens_node *n, flux_point p);
/* Walk an overlay/panel subtree for accessibility (called by the a11y walk). */
lens_node **lensi_overlay_layers(const lens *ui, uint32_t *out_count);

/* render (drawlist.c, replay.c) */
void lensi_drawlist_push(lens *ui, lens_node *n, lens_draw_cmd cmd);
flux_result lensi_render_tree(lens *ui, flux_canvas *canvas);
void lensi_render_node(lens *ui, flux_canvas *canvas, lens_node *n, flux_rect clip);
void lensi_mark_dirty(lens *ui); /* per-frame subtree change detection */

/* text — lens's thin seam (seam.c) over the shared flux-text engine.
 * These take lens (routing to ui->text) and apply lens label
 * conventions; the engine itself lives in <flux-text/text.h>. */

/* A visual x-span [x0, x1] in logical px, for selection rectangles.
 * Aliased to flux_text_xrange so seam.c forwards selection rects without
 * a copy. */
typedef flux_text_xrange lens_text_xrange;

/* Measure the *visible* portion of a label (everything before "##"). Use
 * this from widgets so the measured advance matches what is painted. */
lens_text_metrics lensi_text_measure_label(lens *ui, const char *label, float size_px,
                                           float weight);

/* Caret geometry (BiDi-correct). caret_x: logical x before byte `byte`;
 * caret_byte: byte nearest local x. */
float lensi_text_caret_x(lens *ui, const char *utf8, size_t byte, float size_px, float weight);
size_t lensi_text_caret_byte(lens *ui, const char *utf8, float local_x, float size_px,
                             float weight);

/* Visual rectangles covering the logical byte range [lo, hi). Returns the
 * count written (<= max). */
int lensi_text_sel_rects(lens *ui, const char *utf8, size_t lo, size_t hi, float size_px,
                         float weight, lens_text_xrange *out, int max);

/* Next caret byte one step in the visual direction (`forward` = visually
 * rightward). Returns the same byte when there is no stop on that side. */
size_t lensi_text_caret_visual(lens *ui, const char *utf8, size_t byte, bool forward, float size_px,
                               float weight);

/* ------------------------------------------------------------------ */
/*  Small helpers                                                     */
/* ------------------------------------------------------------------ */

static inline bool lensi_point_in(flux_point p, flux_rect r) {
    return p.x >= r.x && p.x < r.x + r.w && p.y >= r.y && p.y < r.y + r.h;
}

/* Exponential ease toward `target`. Pass the `ui` so a value still in transit
 * marks the frame as animating (`ui->anim_pending`): an input-driven host can
 * then schedule the next frame and let the animation settle instead of
 * freezing mid-fade when input stops. `ui` may be NULL (e.g. tests). Under
 * reduced motion the value resolves to `target` immediately. */
static inline float lensi_approach(lens *ui, float cur, float target, float dt, float rate) {
    if (ui && ui->reduced_motion)
        return target;
    float k = dt * rate;
    float next = (k >= 1.0f) ? target : cur + (target - cur) * k;
    if (ui && fabsf(next - target) > 0.0015f)
        ui->anim_pending = true;
    return next;
}

/* premultiplied-colour linear lerp via flux core */
static inline flux_color lensi_lerp_color(flux_color a, flux_color b, float t) {
    flux_vec4 la = flux_color_to_linear(a);
    flux_vec4 lb = flux_color_to_linear(b);
    flux_vec4 r = {
        la.x + (lb.x - la.x) * t,
        la.y + (lb.y - la.y) * t,
        la.z + (lb.z - la.z) * t,
        la.w + (lb.w - la.w) * t,
    };
    return flux_color_from_linear(r);
}

static inline flux_color lensi_color_alpha(flux_color c, uint8_t a) {
    return (c & 0x00FFFFFFu) | ((uint32_t)a << 24);
}

#endif /* LENSI_INTERNAL_H */
