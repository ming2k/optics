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
#define LENSI_PASTE_MAX (1024u * 1024u) /* clipboard staging (ADR-0036) */
#define LENSI_TRANSIENT_MAX 8u   /* max simultaneously-open transients (ADR-0060) */
#define LENSI_BAND_PREV_MAX 16u  /* per-band prev-frame ids carried for hit-testing */
#define LENSI_STYLE_STACK_MAX 16u /* style scope stack depth (ADR-0061) */
#define LENSI_MODAL_STACK_MAX 4u  /* nested modal focus traps (ADR-0039) */

/* Node placement (ADR-0060): FLOW participates in its parent's flexbox;
 * ABS escapes the flow and the ancestor clips and is emitted in its z
 * band. Only container sub-roots may be ABS (lens_place_begin). */
typedef enum lens_place {
    LENS_PLACE_FLOW = 0,
    LENS_PLACE_ABS = 1,
} lens_place;

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
    flux_color outline_color; /* opt-in foreground contour */
    float radius;
    float width;      /* border width */
    float outline_width;
    const char *text; /* LENS_DRAW_TEXT: arena-copied utf8 */
    float text_size;
    float text_weight; /* 0 = use theme default */
    int32_t text_family; /* lens_text_family captured at build; 0 = default */
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

/* icon registry (icon_runtime.c) — resolve an icon id, built-in or
 * runtime-registered (lens_icon_register_svg), to its command stream and
 * render mode. lensi_icon_desc returns NULL for unknown ids. */
bool lensi_icon_valid(int32_t id);
const lens_icon_desc *lensi_icon_desc(int32_t id);
uint8_t lensi_icon_mode(int32_t id);

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
    uint32_t child_hash;      /* rolling hash of this frame's child id sequence */
    uint32_t last_child_hash; /* previous frame's child_hash */

    /* layout inputs (per frame) */
    bool is_container;
    bool is_scroll;
    /* placement metadata (ADR-0060). FLOW nodes are ordinary flex items and
     * live in LENS_BAND_BASE. An ABS node keeps its parent chain and sibling
     * sequence position but consumes no flow space: it is measured, then
     * resolved by `mode` against `place_rect`/`place_bounds` after the flow
     * children are arranged, escapes every ancestor clip (render + hit), and
     * is emitted in `band` order. `transient` gates the node on the open-set
     * (Esc / click-outside dismissal); `interactive` opts a BACKDROP node
     * into hit-testing (all other bands always occlude/hit normally). */
    lens_place place;
    lens_band band;
    lens_place_mode mode;
    flux_rect place_rect;   /* EXACT: position+min extent; ANCHORED: owner anchor */
    flux_rect place_bounds; /* placement/render boundary; default = display */
    bool has_place_bounds;
    bool transient;
    bool interactive;
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
    /* Geometry as of the last completed lens_render. `prev_rect` is updated
     * during layout for next-frame hit testing, so it cannot also serve as
     * the paint baseline: comparing final_rect to prev_rect after arrange
     * would always report equality and let scroll/resize replay stale
     * vertices. */
    bool has_render_rect;
    flux_rect render_rect;

    /* display-list record of this subtree's emission (persistent;
     * canvas-owned segment, replayed instead of re-emitting when the
     * subtree is unchanged — see replay.c). record_clip is the lens
     * clip argument at record time; record_text_gen is the flux-text
     * atlas clear count at record time (a clear re-packs glyph texels,
     * which freezes recorded UVs — records must not survive it). */
    flux_canvas_record record;
    flux_rect record_clip;
    uint64_t record_text_gen;

    /* persistent per-node user state (lens_node_state) */
    void *state;
    size_t state_bytes;

    /* skin scratch (ADR-0061 item 9): four retained floats for caller-owned
     * skins (a spring's position/velocity), exposed via lens_skin_scratch.
     * Persistent like `state`: zeroed by the store_touch memset at creation,
     * deliberately NOT cleared by lensi_node_reset_frame, and freed with the
     * node by the ADR-0038 GC. Mechanism, not animation — the library stores
     * but never integrates. */
    float skin_scratch[4];
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
    int text_family;  /* lens_text_family for subsequently built widgets;
                       * 0 (DEFAULT) keeps the engine's sans default */

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
    lens_style next_style; /* staged per-call style atoms (lens_box.style) */

    /* style scope stack (ADR-0061): strictly nested, folded bottom-up so
     * the nearest enclosing scope wins per field; reset every lens_begin. */
    lens_style style_stack[LENSI_STYLE_STACK_MAX];
    uint32_t style_top;

    bool click_hit_focusable; /* mouse pressed inside a focusable widget */

    /* interaction state (ADR-0029). There is deliberately no `hot_id`:
     * hover is reported per-widget through lens_response.hovered, and a
     * context-global hovered id had no reader (dead state). */
    lens_id active_id; /* captured (e.g. dragging) */
    lens_id focused_id;
    bool focus_visible; /* keyboard modality (ADR-0058): true when the last
                         * focus move came from Tab traversal; cleared by a
                         * pointer press or programmatic lens_set_focus.
                         * Drives LENS_STATE_FOCUS_VISIBLE. */
    lens_id scroll_hot_id; /* deepest scroll container under cursor */
    lens_cursor_hint cursor_hint; /* semantic cursor for the top hovered control */
    lens_response last_response;
    lens_node *last_node; /* most recently linked node (for lens_a11y) */

    /* Pending assistive-technology activation (ADR-0062): set by
     * lens_a11y_activate, consumed single-shot by lensi_interact when the
     * matching node is built (focusable + not disabled), cleared at
     * lens_end if unconsumed. Bypasses pointer occlusion by design. */
    lens_id a11y_activate_id;

    /* Per-frame key-consumption marks (ADR-0029): a widget that eats a key
     * (activation, menu arrow navigation) marks its index so a later
     * central consumer — the transient dismissal pass reading Escape — does
     * not see it. Indexed parallel to ui->input.keys; reset in lens_begin. */
    uint8_t key_consumed[LENS_INPUT_MAX_KEYS];
    /* Menu arrow-nav request (ADR-0040): a focused menu row records the
     * desired direction here (the key is consumed at the same time); the
     * menu's end applies the move once every sibling row of the popup has
     * been built — mid-build the sibling links are only partial. */
    int8_t menu_nav;

    /* tab focus order, collected during build (arena) */
    lens_id *tab_order;
    uint32_t tab_count, tab_cap;

    /* clipboard + IME (ADR-0036) */
    lens_clipboard clipboard;
    flux_rect caret_rect; /* set by the focused text widget */
    lens_text_context text_context; /* set alongside caret_rect (surrounding
                                     * text + content hints for the host IME) */
    char paste_buf[LENSI_PASTE_MAX];
    uint32_t paste_len;     /* 0 = nothing pending */
    lens_id paste_target;   /* focused widget at lens_request_paste time;
                               0 = unbound (host pushed lens_paste directly) */
    uint64_t paste_frame;   /* ui->frame when the paste payload arrived */

    /* transient open-set (ADR-0060): retained per id; only transient place
     * nodes enter this table. Drives begin gating, is_open queries, and
     * Esc/click-outside dismissal. */
    struct lens_transient_slot {
        lens_id id;
        uint64_t opened_frame; /* dismiss grace: same-frame open ignored */
        bool dismissable;      /* false = Escape/click-outside leave it (modal, ADR-0039) */
    } open_transients[LENSI_TRANSIENT_MAX];
    uint32_t open_transient_count;

    /* Z-band buckets (ADR-0060): this frame's ABS nodes, collected by one
     * post-arrange tree walk (lensi_place_bucket); tree pre-order within
     * each band = registration order. Arena-backed; rebuilt each frame. */
    lens_node **bands[LENS_BAND_COUNT];
    uint32_t band_counts[LENS_BAND_COUNT];
    uint32_t band_caps[LENS_BAND_COUNT];
    /* Band membership as of the END of the previous frame, kept across the
     * arena reset as plain ids so band-ordered hit-testing covers base
     * widgets built BEFORE a placed node re-registers this frame (the
     * common case: popups are declared after the content they cover).
     * Without this, occlusion only applied to widgets declared after the
     * placed node in build order, and clicks fell through every popup to
     * tables and scroll areas beneath. */
    lens_id prev_band_ids[LENS_BAND_COUNT][LENSI_BAND_PREV_MAX];
    uint32_t prev_band_counts[LENS_BAND_COUNT];

    /* modal focus trap (ADR-0039). When modal_active, Tab cycling is
     * clamped to [modal_tab_lo, modal_tab_hi) — the tab_order slice
     * recorded during the modal body build. Nested modals stack: opening an
     * inner modal suspends the outer range on the trap stack; the inner
     * modal's end restores it, so only the innermost trap is active. */
    bool modal_active;
    uint32_t modal_tab_lo;
    uint32_t modal_tab_hi;
    uint32_t modal_trap_lo[LENSI_MODAL_STACK_MAX];
    uint32_t modal_trap_hi[LENSI_MODAL_STACK_MAX];
    uint32_t modal_trap_depth;

    /* tooltip (frame-scoped) */
    struct {
        bool active;
        flux_rect anchor;
        char text[128];
    } tooltip;
    bool prev_tooltip_active; /* tooltip.active at the end of last frame;
                               * a disappearing tooltip is damage too */

    /* skin overrides (ADR-0059): per-kind replacement emission functions;
     * NULL entries use the built-in default. */
    lens_skin_fn skins[LENS_WIDGET_KIND_COUNT];

    /* display-list records (render/replay.c). record_canvas owns the
     * per-node segments (borrowed; refreshed every lensi_render_tree —
     * a canvas switch drops every handle without releasing, as the old
     * canvas may already be destroyed). record_text_gen snapshots the
     * flux-text atlas clear count for the frame. */
    flux_canvas *record_canvas;
    uint64_t record_text_gen;
};

/* ------------------------------------------------------------------ */
/*  Style resolution (ADR-0058)                                       */
/*                                                                    */
/*  lens_style_resolved is public (lens.h): skins receive it in the   */
/*  widget record (ADR-0059). The resolver (style/style.c) applies    */
/*  its documented order — theme fallback, hover/pressed derivation,  */
/*  disabled dim — so a consumer reads slots directly.                */
/* ------------------------------------------------------------------ */

/* Derivation strengths used by the resolver, exposed so tests state the
 * contract in the same numbers the implementation uses. Hover mixes a
 * little of the foreground in (a "lift": fg contrasts with any surface by
 * definition, so the direction is right in light and dark themes alike);
 * pressed doubles it; disabled blends most of the way toward the theme's
 * disabled colour. */
#define LENSI_STYLE_HOVER_LIFT 0.08f
#define LENSI_STYLE_PRESSED_DEPTH 0.16f
#define LENSI_STYLE_DISABLED_DIM 0.60f

/* Pre-measure geometry access. Geometry slots are state-independent, so
 * the measure phase reads them through the same fallback rule the
 * resolver applies — instance wins, else theme — without waiting for the
 * interaction state bits. */
static inline float lensi_style_font_size(const lens_style *inst, const lens_theme *theme) {
    return (inst && (inst->fields & LENS_STYLE_FONT_SIZE)) ? inst->font_size : theme->font_size;
}
static inline float lensi_style_padding(const lens_style *inst, const lens_theme *theme) {
    return (inst && (inst->fields & LENS_STYLE_PADDING)) ? inst->padding : theme->padding;
}

/* ================================================================== */
/*  Internal API                                                      */
/* ================================================================== */

/* allocator (persistent store) — honours the device's flux_allocator,
 * or libc malloc when headless. */
void *lensi_alloc(lens *ui, size_t bytes);
void lensi_free(lens *ui, void *ptr);

/* overflow (drawlist.c) — the single writer for ui->overflow. Debug builds
 * emit a one-shot stderr warning: overflow drops draw calls, and silent
 * visual corruption must never be the only signal. */
void lensi_set_overflow(lens *ui);

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
void lensi_scroll_clamp(lens *ui);

/* input / interaction (input.c, focus.c) */
lens_response lensi_interact(lens *ui, lens_node *n, bool focusable, bool disabled);
void lensi_focus_tab(lens *ui);

/* style resolution + cascade (style/style.c, ADR-0058/0061) — see the file
 * header for the resolution order contract. lensi_style_resolve consumes
 * the *effective* style (post-cascade); merge/effective are the per-field
 * cascade itself: per-call (lens_box.style, staged as ui->next_style) over
 * the folded scope stack. Pure: no globals, no side effects beyond draining
 * next_style. */
lens_style_resolved lensi_style_resolve(const lens_style *eff, const lens_theme *theme,
                                        uint32_t state);
lens_style lensi_style_merge(const lens_style *base, const lens_style *over);
lens_style lensi_style_scope_merged(const lens *ui);
lens_style lensi_style_effective(lens *ui); /* drains ui->next_style */

/* skins (skin/, ADR-0059) — the emission phase of migrated widgets.
 * lensi_skin_emit runs the context override (lens_set_skin), else the
 * built-in default. The lensi_skin_* set is the built-in default table
 * behind lens_default_skin. */
void lensi_skin_emit(lens *ui, lens_node *n, const lens_widget_record *rec);
void lensi_skin_button(lens *ui, lens_node *n, const lens_widget_record *rec);
void lensi_skin_selectable(lens *ui, lens_node *n, const lens_widget_record *rec);
void lensi_skin_checkbox(lens *ui, lens_node *n, const lens_widget_record *rec);
void lensi_skin_switch(lens *ui, lens_node *n, const lens_widget_record *rec);
void lensi_skin_radio(lens *ui, lens_node *n, const lens_widget_record *rec);
void lensi_skin_slider(lens *ui, lens_node *n, const lens_widget_record *rec);
void lensi_skin_icon_button(lens *ui, lens_node *n, const lens_widget_record *rec);
void lensi_skin_tabs(lens *ui, lens_node *n, const lens_widget_record *rec);
void lensi_skin_label(lens *ui, lens_node *n, const lens_widget_record *rec);
void lensi_skin_separator(lens *ui, lens_node *n, const lens_widget_record *rec);
void lensi_skin_icon(lens *ui, lens_node *n, const lens_widget_record *rec);
void lensi_skin_image(lens *ui, lens_node *n, const lens_widget_record *rec);
void lensi_skin_progress(lens *ui, lens_node *n, const lens_widget_record *rec);
void lensi_skin_textfield(lens *ui, lens_node *n, const lens_widget_record *rec);
void lensi_skin_textarea(lens *ui, lens_node *n, const lens_widget_record *rec);
void lensi_skin_collapsing(lens *ui, lens_node *n, const lens_widget_record *rec);
void lensi_skin_tree(lens *ui, lens_node *n, const lens_widget_record *rec);
void lensi_skin_table(lens *ui, lens_node *n, const lens_widget_record *rec);
void lensi_skin_split(lens *ui, lens_node *n, const lens_widget_record *rec);
void lensi_skin_menu_item(lens *ui, lens_node *n, const lens_widget_record *rec);
void lensi_skin_dropdown(lens *ui, lens_node *n, const lens_widget_record *rec);
void lensi_skin_link(lens *ui, lens_node *n, const lens_widget_record *rec);

/* scrollbar chrome (skin/scrollbar.c, ADR-0059) — the drawlist-finalize
 * walk that emits scrollbars for solved scroll containers (placed
 * subtrees included: they live in the one tree now, ADR-0060). Runs in
 * lens_end after layout and placement, before damage tracking; also
 * persists the thumb geometry next frame's thumb hit-testing reads. */
void lensi_scrollbars_emit(lens *ui);

/* clipboard + IME (clipboard.c, ADR-0036) — internal helpers for text
 * widgets (the consumer that will land with lens_text_input). */
void lensi_set_caret_rect(lens *ui, flux_rect r);
/* The focused text widget pushes its surrounding-text context every frame
 * (borrowed buffer; see lens_text_context in lens.h). */
void lensi_set_text_context(lens *ui, const char *utf8, uint32_t len, uint32_t cursor,
                            bool multiline);
/* Borrow + clear any pending paste. Returns NULL when none. The buffer
 * is valid for the rest of the frame. */
const char *lensi_take_paste(lens *ui, uint32_t *out_len);

/* placement (place/place.c, ADR-0060) — the open-set, band bucketing, and
 * dismissal behind the public lens_place_* API. Transient and persistent
 * placed nodes share the same placement + band machinery; only transients
 * track open state. */
bool lensi_place_is_open_id(const lens *ui, lens_id id);
void lensi_place_open_id_pub(lens *ui, lens_id id, bool dismissable); /* ADR-0040 menu internal */
/* Carry this frame's band buckets across the arena reset as per-band prev
 * id lists (lens_begin), so next frame's occlusion checks hit-test against
 * the nodes that are actually on screen. */
void lensi_place_snapshot_prev(lens *ui);
/* One post-arrange tree walk that buckets ABS nodes into ui->bands[] — the
 * single choke point that defines the global emission order (ADR-0060). */
void lensi_place_bucket(lens *ui);
void lensi_place_dismiss(lens *ui); /* click-outside + Esc (transients only) */
/* True when a node emitted ABOVE the widget — a strictly-higher band, or
 * the same band with a strictly greater registration index — covers the
 * cursor: occlusion *is* the reversed global emission order (ADR-0060; the
 * old eclipse mechanism is deleted). Also true for widgets inside a
 * hit-transparent BACKDROP node. Widgets with their own hit-testing (table
 * rows, scrollbars, wheel routing) must check this in addition to
 * lensi_interact so placed nodes above them swallow the interaction too. */
bool lensi_widget_occluded(const lens *ui, const lens_node *n);
/* True when point `p` falls outside the viewport of any scroll ancestor
 * of `n`. Scroll containers clip their children's RENDERING to the
 * viewport; hit-testing must apply the same clip, otherwise children
 * scrolled out of view stay hoverable/clickable through whatever is
 * painted over them (a queue item folded below the viewport edge
 * reacting under the player bar). ABS ancestors escape ancestor clips
 * (ADR-0060), so the walk stops at the nearest ABS node. Every
 * interactive hit-test must consult this in addition to the widget's
 * own rect. */
bool lensi_point_clipped_by_scroll(const lens_node *n, flux_point p);

/* render (drawlist.c, replay.c) */
void lensi_drawlist_push(lens *ui, lens_node *n, lens_draw_cmd cmd);
flux_result lensi_render_tree(lens *ui, flux_canvas *canvas);
void lensi_render_node(lens *ui, flux_canvas *canvas, lens_node *n, flux_rect clip);
void lensi_mark_dirty(lens *ui); /* per-frame subtree change detection */
/* Bottom-up change detection rooted at `n` (replay.c). ABS descendants are
 * marked too (single tree, ADR-0060) but do NOT roll up into the parent's
 * subtree_changed: the parent's display-list record contains flow children
 * only, so a placed subtree's damage must not invalidate it — the repaint
 * query consults the band buckets directly (context.c). */
bool lensi_mark_subtree_changed(lens_node *n);
/* Drop a node's display-list record handle WITHOUT releasing the
 * segment (replay.c). Used from store teardown, where the owning canvas
 * may already be gone; a live canvas reclaims the slot via its LRU
 * budget. */
void lensi_node_drop_record(lens *ui, lens_node *n);

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
