/*
 * ui internal header — cross-module private types and `optics_ui__*`
 * symbols. Not installed, not exported. The double-underscore prefix marks
 * any symbol that is internal to the library; the public surface is the
 * `optics_ui_*` prefix in <optics/ui.h>. See docs/dev/project-layout.md.
 */

#ifndef OPTICS_UI_INTERNAL_H
#define OPTICS_UI_INTERNAL_H

#include <math.h>
#include <optics/gfx/observe.h>
#include <optics/gfx/profile.h>
#include <optics/text.h>
#include <optics/ui.h>
#include <optics/ui/icon.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/*  Tunables                                                          */
/* ------------------------------------------------------------------ */

#define OPTICS_UI_DEFAULT_ARENA_BYTES (1u << 20) /* 1 MiB per-frame arena */
#define OPTICS_UI_DEFAULT_STORE_CAP 256u         /* initial hash slots    */
#define OPTICS_UI_LEAVE_GRACE_FRAMES 8u          /* reap delay (ADR-0027) */
#define OPTICS_UI_ID_STACK_MAX 64u
#define OPTICS_UI_CONTAINER_STACK_MAX 64u
#define OPTICS_UI_PASTE_MAX (1024u * 1024u) /* clipboard staging (ADR-0036) */
#define OPTICS_UI_TRANSIENT_MAX 8u          /* max simultaneously-open transients (ADR-0060) */
#define OPTICS_UI_STYLE_STACK_MAX 16u       /* style scope stack depth (ADR-0061) */
#define OPTICS_UI_CLIP_STACK_MAX 8u         /* nested optics_ui_clip_begin depth */
#define OPTICS_UI_MODAL_STACK_MAX 4u        /* nested modal focus traps (ADR-0039) */
/* Caret blink period: one full on/off cycle, so a 530 ms half-cycle — the
 * cadence X11 blink, Win32, AppKit and GTK converged on (ADR-0151). A host
 * may override it per context with `optics_ui_set_caret_blink`. */
#define OPTICS_UI_CARET_BLINK_PERIOD_S 1.06f

/* Node placement (ADR-0060): FLOW participates in its parent's flexbox;
 * ABS escapes the flow and the ancestor clips and is emitted in its z
 * band. Only container sub-roots may be ABS (optics_ui_place_begin). */
typedef enum optics_ui_place : uint32_t {
    OPTICS_UI_PLACE_FLOW = 0,
    OPTICS_UI_PLACE_ABS = 1,
} optics_ui_place;

/* ------------------------------------------------------------------ */
/*  Draw command (ADR-0030) — coordinates relative to the node box    */
/* ------------------------------------------------------------------ */

typedef enum optics_ui_draw_kind : uint32_t {
    OPTICS_UI_DRAW_RECT = 0,
    OPTICS_UI_DRAW_BORDER = 1,
    OPTICS_UI_DRAW_TEXT = 2,
    OPTICS_UI_DRAW_IMAGE = 3,
    OPTICS_UI_DRAW_CLIP_PUSH = 4,
    OPTICS_UI_DRAW_CLIP_POP = 5,
    OPTICS_UI_DRAW_ICON = 6,
} optics_ui_draw_kind;

typedef struct optics_ui_draw_cmd {
    optics_ui_draw_kind kind;
    /* Relative to the node's final_rect. With an explicit positive width,
     * a negative x anchors the rectangle to the trailing edge, and a negative
     * width/height centres the content instead of sizing it. */
    optics_gfx_rect rel;
    /* For `OPTICS_UI_DRAW_TEXT` only: the shaped advance of the run, recorded by the
     * widget that measured it. Replay positions the glyphs from it and the damage
     * walk reports those same pixels from it, so neither re-shapes — shaping is a
     * frame-scoped operation and running it inside a damage query corrupts the
     * glyph cache for the next frame. Zero means "not measured", and both
     * consumers fall back to `rel`. */
    float text_advance;
    optics_gfx_color color;
    optics_gfx_color outline_color; /* opt-in foreground contour */
    float radius;
    float width; /* border width */
    float outline_width;
    const char *text; /* OPTICS_UI_DRAW_TEXT: arena-copied utf8 */
    float text_size;
    float text_weight;         /* 0 = use theme default */
    int32_t text_family;       /* optics_ui_text_family captured at build; 0 = default */
    optics_ui_icon_id icon_id; /* OPTICS_UI_DRAW_ICON: built-in or context-registered id */
    optics_gfx_image *image;   /* OPTICS_UI_DRAW_IMAGE: host-owned texture, borrowed
                          for the frame; must outlive snapshot publication */
} optics_ui_draw_cmd;

/* Upper bound on the paint runs one icon may resolve to. */
#define OPTICS_UI_ICON_MAX_RUNS 64u

/* Per-context registered vector icon slot (icon_runtime.c). */
typedef struct optics_ui__icon_slot {
    uint32_t generation;
    bool occupied;
    optics_ui_icon_cmd *cmds;
    optics_ui_icon_run *runs;
    optics_ui_icon_desc desc;
} optics_ui__icon_slot;

typedef struct optics_ui__icon_registry {
    optics_ui__icon_slot *slots;
    uint32_t cap;
} optics_ui__icon_registry;

void optics_ui__icon_registry_init(optics_ui *ui);
void optics_ui__icon_registry_destroy(optics_ui *ui);
bool optics_ui__icon_valid(const optics_ui *ui, optics_ui_icon_id id);
const optics_ui_icon_desc *optics_ui__icon_desc(const optics_ui *ui, optics_ui_icon_id id);

/* ------------------------------------------------------------------ */
/*  Retained node slot (ADR-0027)                                     */
/* ------------------------------------------------------------------ */

struct optics_ui_node {
    optics_ui_id id;
    optics_ui *ui; /* owning context (for optics_ui_node_state) */

    /* lifecycle (persistent) */
    uint64_t incarnation;
    size_t presented_index;
    uint64_t last_seen;      /* frame stamp */
    uint32_t leaving_frames; /* grace countdown when LEAVING */
    optics_ui_node_phase phase;
    bool has_prev; /* prev_rect is valid */

    /* tree (rebuilt each frame) */
    optics_ui_node *parent;
    optics_ui_node *first_child;
    optics_ui_node *last_child;
    optics_ui_node *next_sibling;
    uint32_t child_count;
    uint32_t child_seq;       /* sibling-disambiguation counter */
    uint32_t child_hash;      /* rolling hash of this frame's child id sequence */
    uint32_t last_child_hash; /* previous frame's child_hash */

    /* layout inputs (per frame) */
    bool is_container;
    bool is_scroll;
    bool is_grid;
    bool is_stack;
    bool wrap;
    float row_gap;
    /* placement metadata (ADR-0060). FLOW nodes are ordinary flex items and
     * live in OPTICS_UI_BAND_BASE. An ABS node keeps its parent chain and sibling
     * sequence position but consumes no flow space: it is measured, then
     * resolved by `mode` against `place_rect`/`place_bounds` after the flow
     * children are arranged, escapes every ancestor clip (render + hit), and
     * is emitted in `band` order. `transient` gates the node on the open-set
     * (Esc / click-outside dismissal); `interactive` opts a BACKDROP node
     * into hit-testing (all other bands always occlude/hit normally). */
    optics_ui_place place;
    optics_ui_band band;
    optics_ui_place_mode mode;
    optics_gfx_rect place_rect;   /* EXACT: position+min extent; ANCHORED: owner anchor */
    optics_gfx_rect place_bounds; /* placement/render boundary; default = display */
    bool has_place_bounds;
    bool transient;
    bool interactive;
    optics_ui_axis axis;
    float gap, pad;
    optics_ui_align cross;
    optics_ui_align align;
    bool space_between, fit;
    bool box_disabled;
    uint32_t grid_columns;
    float grid_row_gap;
    float grid_row_height;
    uint32_t col_span;
    uint32_t row_span;
    int32_t grid_col;
    int32_t grid_row;
    uint32_t resolved_col;
    uint32_t resolved_row;
    uint32_t resolved_col_span;
    uint32_t resolved_row_span;
    float flex_grow;
    float fixed_w, fixed_h; /* 0 = intrinsic */
    float min_w, max_w;     /* 0 = unconstrained */
    float min_h, max_h;     /* 0 = unconstrained */
    float scroll_x, scroll_y;
    float scroll_gutter; /* trailing viewport space reserved for scrollbar */
    /* Build-time stamp of the context opacity (optics_ui_set_opacity): emission
     * bakes it into every command pushed onto this node this frame. */
    float opacity;
    /* Build-time stamp of the context frame clip (optics_ui_clip_begin). Unlike a
     * parent's flow viewport, this stamp follows the node into a placed band,
     * so absolute overlays cannot escape a scoped clip. */
    bool has_frame_clip;
    optics_gfx_rect frame_clip;

    /* layout outputs */
    optics_gfx_point measured;  /* measure pass */
    optics_gfx_rect final_rect; /* arrange pass (this frame) */
    optics_gfx_rect prev_rect;  /* last frame's final_rect (hit-test) */

    /* draw list (per frame, arena-allocated) */
    optics_ui_draw_cmd *cmds;
    uint32_t cmd_count, cmd_cap;
    uint32_t cmd_hash;      /* rolling hash of this frame's cmds */
    uint32_t last_cmd_hash; /* previous frame's cmd_hash */

    /* accessibility semantics (per frame; ADR-0035) */
    optics_ui_semantics semantics;

    /* animation (persistent) */
    float hover_t;
    float active_t;
    float last_hover_t;
    float last_active_t;
    float last_opacity;

    /* damage tracking (per frame) */
    bool subtree_changed;
    /* True when *this* node's own paint changed (geometry, opacity, hover/active
     * ease, draw-list hash, or its own linked-child sequence). Narrower than
     * `subtree_changed`, which additionally rolls up every FLOW descendant.
     *
     * `optics_ui_damage_rects` unions `subtree_changed` over ABS/band roots but only
     * `self_changed` over FLOW interior nodes, so a single edited cell reports
     * its own cell instead of the enclosing container's full-display rect. */
    bool self_changed;
    /* The two deltas that are NOT the node's own paint, tracked separately so the
     * damage walk can report the region each one affects instead of the node's box
     * (ADR-0149):
     *
     *   - `clip_changed` — the clip this node imposes on its children narrowed,
     *     widened or moved. The affected pixels belong to descendants, so they are
     *     reported from the clip's *previous* extent.
     *   - `restacked` — a placed node changed band or position within its band, so
     *     which pixels it occludes changed without its own paint changing.
     *
     * Both contribute to `self_changed`; they exist so the walk does not have to
     * re-derive which of several causes applied. */
    bool clip_changed;
    bool restacked;
    /* Last successfully compiled geometry for visual cache invalidation.
     * prev_rect belongs exclusively to the activated interaction snapshot. */
    bool has_render_rect;
    optics_gfx_rect render_rect;
    /* The pixels `render_rect`'s frame actually covered, captured from the
     * snapshot (ADR-0144). Survives the arena, so a LEAVING node's vacated
     * region is still known after its command list has been recycled.
     * `prev_paint_rect` snapshots `render_paint_rect` during `optics_ui_end`
     * so `optics_ui_damage_rects` still sees the previous frame's paint even
     * if a caller activates the new snapshot before querying damage. */
    optics_gfx_rect render_paint_rect;
    optics_gfx_rect prev_paint_rect;
    /* The clip this node imposed on its children last frame, and the band it was
     * composited into (ADR-0149). Both change what descendants look like without
     * any descendant's own state changing:
     *
     *   - a scrolling container's viewport shrinks when a scrollbar appears, or a
     *     `place_bounds` clamp moves, and the pixels that newly fall outside must
     *     be erased — but the container's *own* box, opacity and command list are
     *     untouched;
     *   - moving a placed node between bands, or reordering it within one,
     *     changes which pixels it occludes.
     *
     * Neither is visible to the geometry/hash comparison, so they are compared
     * explicitly. `has_prev_clip` distinguishes "never built" from "clips nothing". */
    bool has_prev_clip;
    optics_gfx_rect prev_clip;
    /* Snapshot of `prev_clip` captured during `optics_ui_end` before any
     * activation overwrites `prev_clip`, keeping `optics_ui_damage_rects`
     * order-independent with respect to `optics_ui_snapshot_activate`. */
    optics_gfx_rect damage_prev_clip;
    optics_ui_band prev_band;
    uint32_t prev_band_order;

    /* Cached DisplayList for static subtrees (ADR-0094) */
    optics_gfx_display_list *cached_dl;
    optics_gfx_rect cached_clip;
    uint64_t visual_revision, cached_revision, presented_revision;
    uint64_t self_revision, presented_self_revision;
    float cached_scale;

    /* persistent per-node user state (optics_ui_node_state) */
    void *state;
    size_t state_bytes;

    /* skin scratch (ADR-0061 item 9): four retained floats for caller-owned
     * skins (a spring's position/velocity), exposed via optics_ui_skin_scratch.
     * Persistent like `state`: zeroed by the store_touch memset at creation,
     * deliberately NOT cleared by optics_ui__node_reset_frame, and freed with the
     * node by the ADR-0038 GC. Mechanism, not animation — the library stores
     * but never integrates. */
    float skin_scratch[OPTICS_UI_SKIN_SCRATCH_FLOATS];
};

/* ------------------------------------------------------------------ */
/*  Scroll state (shared between scroll.c and solve.c)                */
/* ------------------------------------------------------------------ */

typedef struct optics_ui_scroll_geometry {
    optics_gfx_rect thumb, track;
    float offset_y, track_len, scroll_range;
} optics_ui_scroll_geometry;

typedef struct optics_ui_scroll_state {
    float offset_x, offset_y; /* persistent scroll offsets */
    /* Pending layout geometry; input reads activated snapshot geometry. */
    float thumb_y, thumb_h;  /* thumb position & height in node-local space */
    float track_len;         /* draggable track length */
    float scroll_range;      /* max scroll offset */
    bool dragging;           /* thumb is being dragged */
    float drag_start_offset; /* offset_y when drag began */
    float drag_start_y;      /* cursor.y when drag began */
    float drag_ratio;        /* presented content/track ratio when drag began */
    bool hovering;           /* cursor over the track this frame (hover styling) */
} optics_ui_scroll_state;

/* ------------------------------------------------------------------ */
/*  Open-addressing store: optics_ui_id -> optics_ui_node* (ADR-0027)               */
/* ------------------------------------------------------------------ */

/* Live-list terminator (see ui_store). Slot 0 is a valid index, so the
 * sentinel cannot be 0. Shared with the damage walk, which scans the live list
 * for nodes that have left the child lists but still own on-screen pixels. */
#define OPTICS_UI_STORE_LINK_NONE UINT32_MAX

typedef struct optics_ui_store_slot {
    optics_ui_id id; /* 0 = empty */
    optics_ui_node *node;
    /* Live-list link (see store.c): index of the next occupied slot in
     * insertion order, OPTICS_UI_STORE_LINK_NONE at the tail. Stored per
     * slot so the table stays one flat array. */
    uint32_t next_live;
    uint32_t prev_live;
} optics_ui_store_slot;

typedef struct optics_ui_store {
    optics_ui_store_slot *slots;
    uint32_t cap; /* power of two */
    uint32_t count;
    /* Live-list endpoints (see store.c): reap iterates these instead of
     * scanning all `cap` slots, keeping the per-frame GC cost O(live)
     * even when the table capacity sits far above the live population
     * (post-spike dwell during the shrink hysteresis window). */
    uint32_t live_head, live_tail;
    /* Frames the load has stayed under the shrink threshold (1/8 of cap).
     * Sustained dwell triggers a halving rehash; see optics_ui__store_reap. */
    uint32_t idle_frames;
    /* Structural change counter (insert, erase, rehash), wrapping. Debug
     * validation runs only when it moved, so a steady frame costs nothing. */
    uint32_t epoch, validated_epoch;
} optics_ui_store;

/* Shrink hysteresis (see optics_ui__store_reap): the load must stay under 1/8
 * of capacity for this many consecutive frames before the table halves,
 * so a frame-to-frame population wobble never rehashes. */
#define OPTICS_UI_STORE_SHRINK_FRAMES 300
/* Floor for shrinkage: below this the table stays put (the initial
 * capacity is small; rehash churn would outweigh the scan savings). */
#define OPTICS_UI_STORE_MIN_CAP 64

/* ------------------------------------------------------------------ */
/*  Context (ADR-0024, ADR-0032)                                      */
/* ------------------------------------------------------------------ */

/* Ghost replay bounds (ADR-0078): concurrent snapshots and the hard
 * unrefreshed lifetime. Structural ceilings — a UI does not legitimately
 * have more simultaneous exits, and a forgotten ghost must not pin memory
 * forever. */
#define OPTICS_UI_GHOST_MAX 16
#define OPTICS_UI_GHOST_MAX_FRAMES 64

/* ADR-0085 borrowed-string registry capacity (debug builds). Sized for
 * the densest practical frame (~4k widgets); the ring wraps, weakening
 * the net without misfiring it. */
#define OPTICS_UI_BORROW_SET_MAX 4096

/* Icon coverage-mask pool (ADR-0112). Inline storage, LRU-evicted; see
 * render/icon_mask.h for why masks exist and how they are keyed. */
#define OPTICS_UI_ICON_MASK_POOL_CAP 96

typedef struct optics_ui__icon_mask optics_ui__icon_mask;

typedef struct optics_ui__icon_mask_pool {
    optics_ui__icon_mask *entries; /* cap slots, allocated at optics_ui_create */
    uint32_t cap;
    uint32_t count;
    uint64_t tick;
    optics_gfx_sampler *sampler; /* NEAREST, created on first GPU bake */
} optics_ui__icon_mask_pool;

struct optics_ui {
    optics_gfx_device *device; /* may be NULL (headless) */
    optics_ui_theme theme;
    float scale;            /* device-pixel scale (HiDPI); 1.0 by default */
    float text_scale;       /* user text-scale factor (accessibility);
                             * multiplies every font-size token when the resolved
                             * style is built (optics_ui__style_resolve + optics_ui__font_px), so
                             * glyphs, caret metrics, and every widget height
                             * derived from a font token scale together. Geometry
                             * that is a pure px constant (padding, stroke widths,
                             * icon glyph boxes) stays put. 1.0 = default. Not
                             * frame-scoped: survives optics_ui_begin (same contract as
                             * scale/reduced_motion). See optics_ui_set_text_scale. */
    optics_gfx_arena arena; /* per-frame; reset each optics_ui_begin */
    optics_text_ctx *text;  /* shared text engine (text); never NULL
                             * after create — degrades to mono internally */
    int text_family;        /* optics_ui_text_family for subsequently built widgets;
                             * 0 (DEFAULT) keeps the engine's sans default */
    float opacity;          /* optics_ui_set_opacity context switch; frame-scoped
                             * (reset to 1.0 by optics_ui_begin), stamped per node at
                             * build time. */
    /* optics_ui_clip_begin/end scope, frame-scoped. `clip` is the effective
     * intersection; the stack restores the enclosing scope. `clip_overflow`
     * counts pushes beyond OPTICS_UI_CLIP_STACK_MAX so their matching ends stay
     * balanced instead of popping an unrelated outer scope. */
    optics_gfx_rect clip;
    optics_gfx_rect clip_stack[OPTICS_UI_CLIP_STACK_MAX];
    uint32_t clip_depth;
    uint32_t clip_overflow;

    optics_ui_store store;
    optics_ui__icon_registry icons;       /* per-context vector icon registry */
    optics_ui__icon_mask_pool icon_masks; /* baked coverage masks (ADR-0112); pool
                                           * storage is allocated at optics_ui_create so
                                           * the struct stays fixed-size */
    optics_ui_node *root;
    uint64_t frame;
    bool building;
    uint64_t generation;
    uint64_t last_presented_generation;
    uint64_t identity, next_incarnation;
    optics_ui_scene_snapshot *presented_snapshot;

    optics_ui_input input; /* copy for the frame */
    bool overflow;         /* arena overflowed this frame */
    bool duplicate_ids;    /* a parent received the same child id twice */
    bool anim_pending;     /* an eased value is still in transit this
                            * frame; the host should schedule another
                            * frame so the animation can settle even
                            * without further input (see optics_ui_anim_pending) */
    bool reduced_motion;   /* accessibility: when set, every eased value
                            * resolves to its target in one frame (see
                            * optics_ui_set_reduced_motion) */

    /* id stack (ADR-0026) */
    optics_ui_id id_stack[OPTICS_UI_ID_STACK_MAX];
    uint32_t id_top; /* count; top = id_stack[id_top-1] */

    /* container stack (layout build) */
    optics_ui_node *cont_stack[OPTICS_UI_CONTAINER_STACK_MAX];
    uint32_t cont_top;

    /* pending modifiers for the next widget */
    float next_flex;
    float next_w, next_h;
    uint32_t next_col_span;
    uint32_t next_row_span;
    int32_t next_grid_col;
    int32_t next_grid_row;
    bool have_next_flex, have_next_size;
    bool have_next_col_span, have_next_row_span, have_next_grid_pos;
    bool next_disabled;
    bool next_error;
    const char *next_placeholder;
    optics_ui_style next_style; /* staged per-call style atoms (optics_ui_box.style) */

    /* style scope stack (ADR-0061): strictly nested, folded bottom-up so
     * the nearest enclosing scope wins per field; reset every optics_ui_begin. */
    optics_ui_style style_stack[OPTICS_UI_STYLE_STACK_MAX];
    uint32_t style_top;

    bool click_hit_focusable; /* mouse pressed inside a focusable widget */

    /* Keep the internal layout identical for library and direct-source tests.
     * Registration code is optional; its diagnostic storage has a fixed slot. */
    const char *borrow_set[OPTICS_UI_BORROW_SET_MAX];
    uint32_t borrow_count;

    /* interaction state (ADR-0029). There is deliberately no `hot_id`:
     * hover is reported per-widget through optics_ui_response.hovered, and a
     * context-global hovered id had no reader (dead state). */
    optics_ui_id active_id; /* captured (e.g. dragging) */
    optics_ui_id focused_id;
    bool focus_visible;          /* keyboard modality (ADR-0058): true when the last
                                  * focus move came from Tab traversal; cleared by a
                                  * pointer press or programmatic optics_ui_set_focus.
                                  * Drives OPTICS_UI_STATE_FOCUS_VISIBLE. */
    optics_ui_id focus_at_begin; /* focus snapshot at optics_ui_begin, for detecting
                                  * mid/post-build focus transitions that need a
                                  * follow-up frame to settle visuals */
    bool focus_visible_at_begin;
    optics_ui_id scroll_hot_id;        /* deepest scroll container under cursor */
    optics_ui_cursor_hint cursor_hint; /* semantic cursor for the top hovered control */
    optics_ui_response last_response;
    optics_ui_node *last_node; /* most recently linked node (for optics_ui_a11y) */

    /* Pending assistive-technology activation (ADR-0062): set by
     * optics_ui_a11y_activate, consumed single-shot by optics_ui__interact when the
     * matching node is built (focusable + not disabled), cleared at
     * optics_ui_end if unconsumed. Bypasses pointer occlusion by design. */
    optics_ui_id a11y_activate_id;

    /* Per-frame key-consumption marks (ADR-0029): a widget that eats a key
     * (activation, menu arrow navigation) marks its index so a later
     * central consumer — the transient dismissal pass reading Escape — does
     * not see it. Indexed parallel to ui->input.keys; reset in optics_ui_begin. */
    uint8_t key_consumed[OPTICS_UI_INPUT_MAX_KEYS];
    /* Menu arrow-nav request (ADR-0040): a focused menu row records the
     * desired direction here (the key is consumed at the same time); the
     * menu's end applies the move once every sibling row of the popup has
     * been built — mid-build the sibling links are only partial. */
    int8_t menu_nav;

    /* tab focus order, collected during build (arena) */
    optics_ui_id *tab_order;
    uint32_t tab_count, tab_cap;

    /* clipboard + IME (ADR-0036) */
    optics_ui_clipboard clipboard;
    optics_ui_dnd_host dnd_host;
    optics_gfx_rect caret_rect;          /* set by the focused text widget */
    optics_ui_text_context text_context; /* set alongside caret_rect (surrounding
                                          * text + content hints for the host IME) */

    /* Caret blink clock (ADR-0151). The blink is a *phase* of the caret,
     * advanced by the frame delta, not a property of "a caret exists".
     * `caret_visible` is the current phase; `caret_presented_visible` is the
     * phase the last *activated* snapshot put on screen, so the change test
     * compares against pixels the user saw rather than against a frame that
     * may have been skipped.
     *
     * `caret_ui_owned` distinguishes the two kinds of caret this context
     * holds, and only one of them is ui's to blink:
     *
     *   - **widget-owned** (`optics_ui__set_caret_rect`, from a text widget): ui
     *     paints this caret in the skin, so ui owns its clock and reports
     *     its flips as damage.
     *   - **host-owned** (`optics_ui_set_caret_rect`, the public API an app uses for
     *     its own editor surface): the app paints the caret with its own clock.
     *     ui holds the rect only to answer `optics_ui_caret_rect` for the IME
     *     platform layer, and must report *no* damage and *no* clock for it —
     *     otherwise two independent clocks would drive one caret, and ui
     *     would manufacture flips the app's painter never made. */
    bool caret_visible;
    bool caret_blink_enabled;
    bool caret_ui_owned;
    float caret_blink_period_s;
    float caret_phase_s;
    bool caret_presented_visible;
    char paste_buf[OPTICS_UI_PASTE_MAX];
    uint32_t paste_len;        /* 0 = nothing pending */
    optics_ui_id paste_target; /* focused widget at optics_ui_request_paste time;
                             0 = unbound (host pushed optics_ui_paste directly) */
    uint64_t paste_frame;      /* ui->frame when the paste payload arrived */

    /* Drag-and-drop state (ADR-0086) */
    struct {
        bool active;
        optics_ui_id source_id;
        optics_gfx_point press_pos;
        char drag_text[OPTICS_UI_PASTE_MAX];
        uint32_t drag_text_len;
        uint32_t drag_actions;
        optics_gfx_rect preview_rect;
    } dnd_source;

    struct {
        char drop_buf[OPTICS_UI_PASTE_MAX];
        uint32_t drop_len;
        optics_gfx_point drop_pos;
        uint64_t drop_frame;
        bool dropped;
    } dnd_drop;

    /* transient open-set (ADR-0060): retained per id; only transient place
     * nodes enter this table. Drives begin gating, is_open queries, and
     * Esc/click-outside dismissal. */
    struct optics_ui_transient_slot {
        optics_ui_id id;
        uint64_t opened_frame; /* dismiss grace: same-frame open ignored */
        bool dismissable;      /* false = Escape/click-outside leave it (modal, ADR-0039) */
    } open_transients[OPTICS_UI_TRANSIENT_MAX];
    uint32_t open_transient_count;

    /* Z-band buckets (ADR-0060): this frame's ABS nodes, collected by one
     * post-arrange tree walk (optics_ui__place_bucket); tree pre-order within
     * each band = registration order. Arena-backed; rebuilt each frame. */
    optics_ui_node **bands[OPTICS_UI_BAND_COUNT];
    uint32_t band_counts[OPTICS_UI_BAND_COUNT];
    uint32_t band_caps[OPTICS_UI_BAND_COUNT];
    /* modal focus trap (ADR-0039). When modal_active, Tab cycling is
     * clamped to [modal_tab_lo, modal_tab_hi) — the tab_order slice
     * recorded during the modal body build. Nested modals stack: opening an
     * inner modal suspends the outer range on the trap stack; the inner
     * modal's end restores it, so only the innermost trap is active. */
    bool modal_active;
    uint32_t modal_tab_lo;
    uint32_t modal_tab_hi;
    uint32_t modal_trap_lo[OPTICS_UI_MODAL_STACK_MAX];
    uint32_t modal_trap_hi[OPTICS_UI_MODAL_STACK_MAX];
    uint32_t modal_trap_depth;

    /* tooltip (frame-scoped) */
    struct {
        bool active;
        optics_gfx_rect anchor;
        /* The box the tooltip actually paints, measured once at
         * `optics_ui__tooltip` time (ADR-0149). Replay paints it below the anchor with
         * `2*pad` padding, so the anchor is not its damage — and the damage walk
         * is a `const ui *` pass that must not re-shape text (shaping mutates
         * the glyph cache the next submit depends on), so the measurement has to
         * happen here, while the context is mutable and the frame is building.
         * Survives the `active = false` reset so a disappearing tooltip still
         * knows which pixels it vacated. */
        optics_gfx_rect painted;
        const char *text; /* frame arena; read only while `active` */
        float opacity;    /* context opacity at optics_ui__tooltip time; the tooltip
                           * paints outside the draw list, so replay applies it */
    } tooltip;
    bool prev_tooltip_active; /* tooltip.active at the end of last frame;
                               * a disappearing tooltip is damage too */

    /* skin overrides (ADR-0059): per-kind replacement emission functions;
     * NULL entries use the built-in default. */
    optics_ui_skin_fn skins[OPTICS_UI_WIDGET_KIND_COUNT];
    /* Userdata-skin overrides (optics_ui_set_skin_userdata). A kind has either
     * a plain skin or a userdata skin — the userdata form wins if both
     * were registered, matching "the most specific registration". */
    optics_ui_skin_userdata_fn skins_userdata[OPTICS_UI_WIDGET_KIND_COUNT];
    void *skins_user[OPTICS_UI_WIDGET_KIND_COUNT];
    /* Host-reserved kinds (>= OPTICS_UI_WIDGET_KIND_USER_BASE, ADR-0073) keyed
     * by offset from the base; the side table keeps the count-sized
     * library arrays untouched. */
#define OPTICS_UI_USER_SKIN_MAX 64
    optics_ui_skin_userdata_fn user_skins_userdata[OPTICS_UI_USER_SKIN_MAX];
    void *user_skins_user[OPTICS_UI_USER_SKIN_MAX];
    optics_ui_skin_fn user_skins[OPTICS_UI_USER_SKIN_MAX];

    /* Ghost snapshots (ADR-0078): deep copies of leaving subtrees' draw
     * commands + geometry, captured at their last live optics_ui_end, kept
     * alive by per-frame optics_ui_set_ghost calls, painted at the host's
     * alpha through the shared command emitter. Bounded: OPTICS_UI_GHOST_MAX
     * entries, OPTICS_UI_GHOST_MAX_FRAMES without a refresh. */
    struct optics_ui_ghost *ghosts[OPTICS_UI_GHOST_MAX];
    uint32_t ghost_count;
    /* Per-frame capture arming (optics_ui_set_ghost on a still-live subtree):
     * cleared at optics_ui_begin, consumed by optics_ui__ghost_capture at optics_ui_end.
     * A pin on an already-captured ghost refreshes it instead (ghost.c). */
    optics_ui_id ghost_pins[OPTICS_UI_GHOST_MAX];
    float ghost_pin_alphas[OPTICS_UI_GHOST_MAX];
    uint32_t ghost_pin_count;
};

/* ------------------------------------------------------------------ */
/*  Ghost replay (ADR-0078)                                           */
/*                                                                    */
/*  A leaving subtree's pixels come from a deep snapshot taken at its  */
/*  last live optics_ui_end (cmds + geometry, copied out of the per-frame   */
/*  arena into optics_ui__alloc memory so it survives the reset). The host  */
/*  re-pins it each frame with optics_ui_set_ghost(id, alpha); unrefreshed  */
/*  ghosts count down and expire. Ghosts paint after live content in   */
/*  their band, never hit-test/focus/a11y, and never create canvas     */
/*  display-list records.                                              */
/* ------------------------------------------------------------------ */

typedef struct optics_ui_ghost_cmd {
    optics_ui_draw_cmd cmd; /* text pointer, if any, points into optics_ui__alloc mem */
} optics_ui_ghost_cmd;

typedef struct optics_ui_ghost_node {
    optics_gfx_rect final_rect;
    bool is_scroll;
    float pad;
    float scroll_gutter;
    float opacity;
    optics_gfx_rect place_bounds;
    bool has_place_bounds;
    bool has_frame_clip;
    optics_gfx_rect frame_clip;
    optics_ui_ghost_cmd *cmds;
    uint32_t cmd_count;
    struct optics_ui_ghost_node *first_child;
    struct optics_ui_ghost_node *next_sibling;
} optics_ui_ghost_node;

typedef struct optics_ui_ghost {
    optics_ui_id root_id;
    optics_ui_band band;
    uint32_t frames_left;
    float alpha;
    bool refreshed_this_frame;
    bool wants_alive;           /* the host asked for it this frame (optics_ui_set_ghost) */
    optics_ui_ghost_node *root; /* NULL = slot unused */
} optics_ui_ghost;

/* optics_ui_end (after mark_dirty): capture leaving subtrees. Called with the
 * store walk complete; snapshots roots whose subtree had commands. */
void optics_ui__ghost_capture(optics_ui *ui);

/* True when the host pinned `id` this frame (capture arming). */
bool ghost_wants(const optics_ui *ui, optics_ui_id id);

/* optics_ui_begin: clear the per-frame refresh marks (ghosts survive the arena
 * reset; the countdown/expiry runs at optics_ui_end). */
void optics_ui__ghost_begin_frame(optics_ui *ui);

/* optics_ui_end tail: expire ghosts the host did not refresh this frame. */
void optics_ui__ghost_end_frame(optics_ui *ui);

/* optics_ui_set_ghost — public entry; also the capture-intent signal during a
 * live frame (a node still alive this frame that the host asks to ghost
 * is pinned for capture when it leaves). */
void optics_ui__ghost_pin(optics_ui *ui, optics_ui_id id, float alpha);

/* render: paint ghosts in band order after the live tree. */
optics_gfx_result optics_ui__ghost_compile(optics_ui *ui, optics_gfx_encoder *encoder);

/* destroy: free every snapshot. */
void optics_ui__ghost_destroy(optics_ui *ui);

/* True while any ghost exists — repaint-pending for hosts. */
bool optics_ui__ghost_active(const optics_ui *ui);

/* ------------------------------------------------------------------ */
/*  Style resolution (ADR-0058)                                       */
/*                                                                    */
/*  optics_ui_style_resolved is public (ui.h): skins receive it in the   */
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
#define OPTICS_UI_STYLE_HOVER_LIFT 0.08f
#define OPTICS_UI_STYLE_PRESSED_DEPTH 0.16f
#define OPTICS_UI_STYLE_DISABLED_DIM 0.60f

/* Pre-measure geometry access. Geometry slots are state-independent, so
 * the measure phase reads them through the same fallback rule the
 * resolver applies — instance wins, else theme — without waiting for the
 * interaction state bits.
 *
 * optics_ui__style_font_size is the ONE font-size funnel: it folds in the
 * accessibility text scale (optics_ui_set_text_scale) so every consumer —
 * widget measure paths and the style resolver alike — sees the same
 * scaled value. Raw semantic size tokens that never pass through a
 * optics_ui_style (title headings, tabs, tooltip) must use optics_ui__font_px. */
static inline float optics_ui__style_font_size(const optics_ui *ui, const optics_ui_style *inst,
                                               const optics_ui_theme *theme) {
    float token =
        (inst && (inst->fields & OPTICS_UI_STYLE_FONT_SIZE)) ? inst->font_size : theme->font_size;
    return token * (ui && ui->text_scale > 0.0f ? ui->text_scale : 1.0f);
}
static inline float optics_ui__style_padding(const optics_ui_style *inst,
                                             const optics_ui_theme *theme) {
    return (inst && (inst->fields & OPTICS_UI_STYLE_PADDING)) ? inst->padding : theme->padding;
}

/* Accessibility text scale applied to a raw font-size token (see
 * optics_ui_set_text_scale). optics_ui__style_resolve multiplies the resolved
 * font_size by this; sites that read a semantic size token straight off
 * the theme (title headings, tabs) must go through this helper so every
 * font in the frame scales by the same factor. Pure-px geometry tokens
 * (padding, stroke widths, icon glyph boxes) are deliberately NOT
 * scaled — only text and text-derived metrics grow. */
static inline float optics_ui__font_px(const optics_ui *ui, float token_px) {
    return token_px * (ui && ui->text_scale > 0.0f ? ui->text_scale : 1.0f);
}

/* ================================================================== */
/*  Internal API                                                      */
/* ================================================================== */

/* allocator (persistent store) — honours the device's optics_gfx_allocator,
 * or libc malloc when headless. */
void *optics_ui__alloc(optics_ui *ui, size_t bytes);
void *optics_ui__alloc_by_device(optics_gfx_device *d, size_t bytes);
void optics_ui__free_by_device(optics_gfx_device *d, void *ptr);
void optics_ui__free(optics_ui *ui, void *ptr);

/* overflow (drawlist.c) — the single writer for ui->overflow. Debug builds
 * emit a one-shot stderr warning: overflow drops draw calls, and silent
 * visual corruption must never be the only signal. */
void optics_ui__set_overflow(optics_ui *ui);
/* Copy `len` bytes of `s` into the per-frame arena, NUL-terminated, so a
 * caller's transient string survives until replay. On arena exhaustion the
 * frame is marked overflowed and "" is returned; a NULL `s` returns NULL. */
const char *optics_ui__arena_strndup(optics_ui *ui, const char *s, size_t len);

/* identity (id.c) */
uint64_t optics_ui__hash(const void *data, size_t len, uint64_t seed);
optics_ui_id optics_ui__id_top(const optics_ui *ui);
optics_ui_id optics_ui__gen_widget_id(optics_ui *ui, const char *label);
optics_ui_id optics_ui__gen_container_id(optics_ui *ui, const char *kind);
size_t optics_ui__label_visible_len(const char *label);

/* store (store.c) */
optics_gfx_result optics_ui__store_init(optics_ui *ui, uint32_t cap);
void optics_ui__store_destroy(optics_ui *ui);
optics_ui_node *optics_ui__store_find(const optics_ui *ui, optics_ui_id id);
optics_ui_node *optics_ui__store_touch(optics_ui *ui,
                                       optics_ui_id id); /* find-or-create, frame-reset */
void optics_ui__store_reap(optics_ui *ui);               /* phase + GC at frame end */
/* Structural self-check of the store: power-of-two capacity, load < 3/4,
 * every node reachable from its home slot, no duplicate ids, and a live
 * list that threads each occupied slot exactly once. Returns the first
 * broken invariant, or nullptr. O(cap + probe lengths); for tests and
 * debug builds. */
const char *optics_ui__store_validate(const optics_ui *ui);
/* optics_ui__store_validate, aborting with the reason on failure. */
void optics_ui__store_check(const optics_ui *ui);
/* Debug builds validate the store at the end of every reap; override with
 * -DOPTICS_UI_STORE_VALIDATE=0/1. */
#ifndef OPTICS_UI_STORE_VALIDATE
#ifdef NDEBUG
#define OPTICS_UI_STORE_VALIDATE 0
#else
#define OPTICS_UI_STORE_VALIDATE 1
#endif
#endif

/* node (node.c) */
void optics_ui__node_reset_frame(optics_ui_node *n);

/* accessibility (a11y/semantics.c) — set a node's semantics for this
 * frame; `name` is truncated at "##" and `name`/`value` are arena-copied
 * so they survive the post-end walk. */
void optics_ui__node_semantics(optics_ui *ui, optics_ui_node *n, optics_ui_role role,
                               const char *name, const char *value, uint32_t flags);

/* tree (tree.c) */
optics_ui_node *optics_ui__open_container(optics_ui *ui);     /* current open container */
void optics_ui__link_child(optics_ui *ui, optics_ui_node *n); /* link into open container */
void optics_ui__open_container_push(optics_ui *ui, optics_ui_node *n);
void optics_ui__open_container_pop(optics_ui *ui);

/* descriptor plumbing (tree.c) — used by the *_ex widget wrappers. */
void optics_ui__node_box(optics_ui *ui, optics_ui_node *n, const optics_ui_box *box);
optics_ui_box optics_ui__merge_box(optics_ui_box base, optics_ui_box override);
void optics_ui__apply_box(optics_ui *ui, optics_ui_box box); /* stage flex/size/disabled/error */
void optics_ui__set_placeholder(optics_ui *ui, const char *text);
void optics_ui__tooltip(optics_ui *ui, const char *text); /* anchored to last widget */

/* Spatial arithmetic & constraint resolution (solve.c, grid.c) */
static inline float optics_ui__constrain_extent(float value, float min_value, float max_value) {
    if (max_value > 0.0f)
        value = fminf(value, max_value);
    if (min_value > 0.0f)
        value = fmaxf(value, min_value);
    return value;
}

static inline float optics_ui__align_offset(optics_ui_align a, float free) {
    switch (a) {
    case OPTICS_UI_CENTER:
        return free * 0.5f;
    case OPTICS_UI_END:
        return free;
    case OPTICS_UI_START:
    case OPTICS_UI_STRETCH:
    default:
        return 0.0f;
    }
}

/* layout strategies: 1D flex (flex.c) and 2D grid (grid.c) */
void optics_ui__layout_solve(optics_ui *ui);
void optics_ui__scroll_clamp(optics_ui *ui);
void optics_ui__arrange_node(optics_ui_node *n, optics_gfx_rect rect);
optics_gfx_rect optics_ui__resolve_abs_rect(const optics_ui *ui, const optics_ui_node *n);
optics_gfx_point optics_ui__flex_measure(optics_ui_node *n);
void optics_ui__flex_arrange(optics_ui_node *n, optics_gfx_rect inner);
optics_gfx_point optics_ui__grid_measure(optics_ui_node *n);
void optics_ui__grid_arrange(optics_ui_node *n, optics_gfx_rect inner);
optics_gfx_point optics_ui__stack_measure(optics_ui_node *n);
void optics_ui__stack_arrange(optics_ui_node *n, optics_gfx_rect inner);

/* input / interaction (input.c, focus.c) */
optics_ui_response optics_ui__interact(optics_ui *ui, optics_ui_node *n, bool focusable,
                                       bool disabled);
void optics_ui__focus_tab(optics_ui *ui);

/* style resolution + cascade (style/style.c, ADR-0058/0061) — see the file
 * header for the resolution order contract. optics_ui__style_resolve consumes
 * the *effective* style (post-cascade); merge/effective are the per-field
 * cascade itself: per-call (optics_ui_box.style, staged as ui->next_style) over
 * the folded scope stack. Pure: no globals, no side effects beyond draining
 * next_style. */
optics_ui_style_resolved optics_ui__style_resolve(optics_ui *ui, const optics_ui_style *eff,
                                                  const optics_ui_theme *theme, uint32_t state);
optics_ui_style optics_ui__style_merge(const optics_ui_style *base, const optics_ui_style *over);
optics_ui_style optics_ui__style_scope_merged(const optics_ui *ui);
optics_ui_style optics_ui__style_effective(optics_ui *ui); /* drains ui->next_style */

/* skins (skin/, ADR-0059) — the emission phase of migrated widgets.
 * optics_ui__skin_emit runs the context override (optics_ui_set_skin), else the
 * built-in default. The optics_ui__skin_* set is the built-in default table
 * behind optics_ui_default_skin. */
void optics_ui__skin_emit(optics_ui *ui, optics_ui_node *n, const optics_ui_widget_record *rec);
#ifdef OPTICS_UI_DEBUG_BORROWS
/* ADR-0085: register a caller-borrowed string for this frame. */
void optics_ui__borrow_register(optics_ui *ui, const char *ptr);
/* ADR-0085: assert `ptr` was registered this frame (or is arena-owned).
 * Aborts with a diagnostic on violation; see core/borrow.c. */
void optics_ui__borrow_check(optics_ui *ui, const char *ptr, const char *what);
#endif

void optics_ui__skin_label(optics_ui *ui, optics_ui_node *n, const optics_ui_widget_record *rec);
void optics_ui__skin_icon(optics_ui *ui, optics_ui_node *n, const optics_ui_widget_record *rec);
void optics_ui__skin_image(optics_ui *ui, optics_ui_node *n, const optics_ui_widget_record *rec);
void optics_ui__skin_separator(optics_ui *ui, optics_ui_node *n,
                               const optics_ui_widget_record *rec);
void optics_ui__skin_button(optics_ui *ui, optics_ui_node *n, const optics_ui_widget_record *rec);
void optics_ui__skin_checkbox(optics_ui *ui, optics_ui_node *n, const optics_ui_widget_record *rec);
void optics_ui__skin_selectable(optics_ui *ui, optics_ui_node *n,
                                const optics_ui_widget_record *rec);
void optics_ui__skin_slider(optics_ui *ui, optics_ui_node *n, const optics_ui_widget_record *rec);
void optics_ui__skin_textedit(optics_ui *ui, optics_ui_node *n, const optics_ui_widget_record *rec);

/* scrollbar chrome (skin/scrollbar.c, ADR-0059) — the drawlist-finalize
 * walk that emits scrollbars for solved scroll containers (placed
 * subtrees included: they live in the one tree now, ADR-0060). Runs in
 * optics_ui_end after layout and placement, before damage tracking; also
 * persists the thumb geometry next frame's thumb hit-testing reads. */
void optics_ui__scrollbars_emit(optics_ui *ui);

/* clipboard + IME (clipboard.c, ADR-0036) — internal helpers for text
 * widgets (a single-line text input is the intended consumer). */
void optics_ui__set_caret_rect(optics_ui *ui, optics_gfx_rect r);
/* The focused text widget pushes its surrounding-text context every frame
 * (borrowed buffer; see optics_ui_text_context in ui.h). */
void optics_ui__set_text_context(optics_ui *ui, const char *utf8, uint32_t len, uint32_t cursor,
                                 bool multiline);
/* Borrow + clear any pending paste. Returns NULL when none. The buffer
 * is valid for the rest of the frame. */
const char *optics_ui__take_paste(optics_ui *ui, uint32_t *out_len);

/* placement (place/place.c, ADR-0060) — the open-set, band bucketing, and
 * dismissal behind the public optics_ui_place_* API. Transient and persistent
 * placed nodes share the same placement + band machinery; only transients
 * track open state. */
bool optics_ui__place_is_open_id(const optics_ui *ui, optics_ui_id id);
void optics_ui__place_open_id_pub(optics_ui *ui, optics_ui_id id,
                                  bool dismissable); /* ADR-0040 menu internal */
/* Carry this frame's band buckets across the arena reset as per-band prev
 * id lists (optics_ui_begin), so next frame's occlusion checks hit-test against
 * the nodes that are actually on screen. */

/* One post-arrange tree walk that buckets ABS nodes into ui->bands[] — the
 * single choke point that defines the global emission order (ADR-0060). */
void optics_ui__place_bucket(optics_ui *ui);
void optics_ui__place_dismiss(optics_ui *ui); /* click-outside + Esc (transients only) */
/* True when a node emitted ABOVE the widget — a strictly-higher band, or
 * the same band with a strictly greater registration index — covers the
 * cursor: occlusion *is* the reversed global emission order (ADR-0060; the
 * old eclipse mechanism is deleted). Also true for widgets inside a
 * hit-transparent BACKDROP node. Widgets with their own hit-testing (table
 * rows, scrollbars, wheel routing) must check this in addition to
 * optics_ui__interact so placed nodes above them swallow the interaction too. */
bool optics_ui__widget_occluded(const optics_ui *ui, const optics_ui_node *n);
bool optics_ui__snapshot_point_clipped(const optics_ui_node *n, optics_gfx_point p);
bool optics_ui__snapshot_scroll_geometry(const optics_ui_node *n, optics_ui_scroll_geometry *out);
/* True when point `p` falls outside the viewport of any scroll ancestor
 * of `n`. Scroll containers clip their children's RENDERING to the
 * viewport; hit-testing must apply the same clip, otherwise children
 * scrolled out of view stay hoverable/clickable through whatever is
 * painted over them (a queue item folded below the viewport edge
 * reacting under the player bar). ABS ancestors escape ancestor clips
 * (ADR-0060), so the walk stops at the nearest ABS node. Every
 * interactive hit-test must consult this in addition to the widget's
 * own rect. */
bool optics_ui__point_clipped_by_scroll(const optics_ui_node *n, optics_gfx_point p);

/* render (drawlist.c, replay.c) */
void optics_ui__drawlist_push(optics_ui *ui, optics_ui_node *n, optics_ui_draw_cmd cmd);
optics_gfx_result optics_ui__compile_commands(optics_ui *ui, optics_gfx_encoder *encoder,
                                              optics_gfx_rect box, optics_gfx_rect clip,
                                              const optics_ui_draw_cmd *cmds, uint32_t count,
                                              float alpha);
void optics_ui__mark_dirty(optics_ui *ui);
/* per-frame subtree change detection */ /* Bottom-up change detection rooted at `n` (replay.c).
                                          * ABS descendants are marked too (single tree,
                                          * ADR-0060) but do NOT roll up into the parent's
                                          * subtree_changed: the parent's display-list record
                                          * contains flow children only, so a placed subtree's
                                          * damage must not invalidate it — the repaint query
                                          * consults the band buckets directly (context.c). */
bool optics_ui__mark_subtree_changed(optics_ui *ui, optics_ui_node *n);
/* Release the immutable child-list cache owned by this node. */
void optics_ui__node_release_cache(optics_ui *ui, optics_ui_node *n);

/* text — ui's thin seam (seam.c) over the shared glyph engine.
 * These take ui (routing to ui->text) and apply ui label
 * conventions; the engine itself lives in <optics/text.h>. */

/* A visual x-span [x0, x1] in logical px, for selection rectangles.
 * Aliased to glyph_xrange so seam.c forwards selection rects without
 * a copy. */
typedef optics_text_xrange optics_ui_text_xrange;

/* Measure the *visible* portion of a label (everything before "##"). Use
 * this from widgets so the measured advance matches what is painted. */
optics_ui_text_metrics optics_ui__text_measure_label(optics_ui *ui, const char *label,
                                                     float size_px, float weight);

/* Caret geometry (BiDi-correct). caret_x: logical x before byte `byte`;
 * caret_byte: byte nearest local x. */
float optics_ui__text_caret_x(optics_ui *ui, const char *utf8, size_t byte, float size_px,
                              float weight);
size_t optics_ui__text_caret_byte(optics_ui *ui, const char *utf8, float local_x, float size_px,
                                  float weight);

/* Visual rectangles covering the logical byte range [lo, hi). Returns the
 * count written (<= max). */
int optics_ui__text_sel_rects(optics_ui *ui, const char *utf8, size_t lo, size_t hi, float size_px,
                              float weight, optics_ui_text_xrange *out, int max);

/* Next caret byte one step in the visual direction (`forward` = visually
 * rightward). Returns the same byte when there is no stop on that side. */
size_t optics_ui__text_caret_visual(optics_ui *ui, const char *utf8, size_t byte, bool forward,
                                    float size_px, float weight);

/* ------------------------------------------------------------------ */
/*  Small helpers                                                     */
/* ------------------------------------------------------------------ */

static inline bool optics_ui__point_in(optics_gfx_point p, optics_gfx_rect r) {
    return p.x >= r.x && p.x < r.x + r.w && p.y >= r.y && p.y < r.y + r.h;
}

/* Resolve one draw command's `rel` geometry against the node box it was emitted
 * in — the single definition of "where does this command paint", shared by
 * replay (which rasterises it) and damage tracking (which must report exactly
 * the pixels replay writes). Keeping one implementation is what stops the two
 * from disagreeing about the bounds of an edited label. */
static inline optics_gfx_rect optics_ui__offset_rel(optics_gfx_rect box, optics_gfx_rect rel) {
    float w = rel.w > 0 ? rel.w : box.w - 2.0f * rel.x;
    float h = rel.h > 0 ? rel.h : box.h - 2.0f * rel.y;
    if (w < 0)
        w = 0;
    if (h < 0)
        h = 0;
    float x = (rel.x < 0.0f && rel.w > 0.0f) ? box.x + box.w + rel.x - w : box.x + rel.x;
    return (optics_gfx_rect){x, box.y + rel.y, w, h};
}

/* Command-aware variant of `optics_ui__offset_rel`.
 *
 * For boxes/rects/borders/clips, `rel.w == 0` / `rel.h == 0` denotes a
 * symmetrically inset fill (`box.w - 2 * rel.x`, `box.h - 2 * rel.y`).
 * For `OPTICS_UI_DRAW_TEXT`, `rel.w == 0` / `rel.h == 0` instead places the
 * text origin at `(rel.x, rel.y)` and runs to the trailing edge of `box`
 * (`box.w - rel.x`, `box.h - rel.y`): subtracting `2 * rel.x` or `2 * rel.y`
 * collapsed right-aligned labels (`OPTICS_UI_END`), narrow icon+label rows,
 * and the bottom half of wrapped/multiline text to zero extent. Negative
 * `rel.w`/`rel.h` keep symmetric centering margins. */
static inline optics_gfx_rect optics_ui__cmd_rel_rect(optics_gfx_rect box,
                                                      const optics_ui_draw_cmd *c) {
    if (!c)
        return box;
    if (c->kind != OPTICS_UI_DRAW_TEXT)
        return optics_ui__offset_rel(box, c->rel);
    const optics_gfx_rect rel = c->rel;
    float w = rel.w > 0.0f
                  ? rel.w
                  : (rel.w < 0.0f ? box.w - 2.0f * rel.x : (rel.x < 0.0f ? box.w : box.w - rel.x));
    float h = rel.h > 0.0f ? rel.h : (rel.h < 0.0f ? box.h - 2.0f * rel.y : box.h - rel.y);
    if (w < 0.0f)
        w = 0.0f;
    if (h < 0.0f)
        h = 0.0f;
    float x = (rel.x < 0.0f && rel.w > 0.0f) ? box.x + box.w + rel.x - w : box.x + rel.x;
    return (optics_gfx_rect){x, box.y + rel.y, w, h};
}

/* The box a text command paints into, in absolute coordinates, from the advance
 * its widget recorded.
 *
 * A text run is the one command kind whose painted extent is not its `rel`
 * rectangle: negative `rel` extents centre the run inside the box, and `rel.w ==
 * 0` leaves it at the left edge but still stops at the advance. A rect taken from
 * the box corner therefore describes pixels the glyphs never touch and omits the
 * ones they do — a centred label in a 380px box paints at x ≈ 191 while its
 * corner-based rect claims x = 0.
 *
 * `advance` is the run's shaped width, recorded at emit time. Both replay and the
 * damage walk read it, so the two cannot disagree and neither re-shapes:
 * `glyph_measure` outside a frame mutates cache state the next submit depends on.
 * Falls back to the command rectangle when no advance was recorded. */
static inline optics_gfx_rect optics_ui__text_bounds(optics_gfx_rect rel_rect, float advance,
                                                     bool centre_w, bool centre_h) {
    (void)centre_h;
    if (!(advance > 0.0f) || rel_rect.w <= 0.0f || rel_rect.h <= 0.0f)
        return rel_rect;
    float x = rel_rect.x;
    if (centre_w)
        x = rel_rect.x + (rel_rect.w - advance) * 0.5f;
    /* Never spill past the box the run was measured against, and never retreat
     * past its origin: an over-wide advance must not report pixels outside the
     * widget it belongs to. */
    if (x < rel_rect.x)
        x = rel_rect.x;
    float w = advance;
    if (x + w > rel_rect.x + rel_rect.w)
        w = rel_rect.x + rel_rect.w - x;
    if (!(w > 0.0f))
        return rel_rect;
    return (optics_gfx_rect){x, rel_rect.y, w, rel_rect.h};
}

/* The clip a node imposes on its own children, resolved from layout state alone.
 *
 * Shared by replay (which installs it) and change detection (which compares it),
 * so the two cannot disagree about where a scrolling viewport ends. A node that
 * scrolls narrows to its padded viewport minus the scrollbar gutter; everything
 * else clips nothing of its own and reports its box, which is the "no narrower
 * clip" answer in a form that can be compared between frames. */
static inline optics_gfx_rect optics_ui__rect_intersect(optics_gfx_rect a, optics_gfx_rect b) {
    float x = a.x > b.x ? a.x : b.x;
    float y = a.y > b.y ? a.y : b.y;
    float right = (a.x + a.w) < (b.x + b.w) ? (a.x + a.w) : (b.x + b.w);
    float bottom = (a.y + a.h) < (b.y + b.h) ? (a.y + a.h) : (b.y + b.h);
    return (optics_gfx_rect){x, y, right > x ? right - x : 0.0f, bottom > y ? bottom - y : 0.0f};
}

static inline optics_gfx_rect optics_ui__node_child_clip(const optics_ui_node *n) {
    if (!n)
        return (optics_gfx_rect){0, 0, 0, 0};
    optics_gfx_rect clip = n->has_frame_clip ? n->frame_clip : n->final_rect;
    if (!n->is_scroll || !n->first_child)
        return clip;
    float viewport_w = n->final_rect.w - 2.0f * n->pad - n->scroll_gutter;
    if (viewport_w < 0.0f)
        viewport_w = 0.0f;
    optics_gfx_rect viewport = {n->final_rect.x + n->pad, n->final_rect.y + n->pad, viewport_w,
                                n->final_rect.h - 2.0f * n->pad};
    return optics_ui__rect_intersect(clip, viewport);
}

/* The pixels ONE command paints, in absolute coordinates, resolved against the
 * node box it was emitted in.
 *
 * This is the single definition of "where does this command paint", shared by
 * replay (which rasterises it) and the damage walk (which must report exactly
 * those pixels). The two cannot disagree about an edited label, an outlined
 * image, or a tab that reaches outside its box — and they cannot drift apart when
 * a new command kind is added, because replay's own `switch` and this function
 * are checked against each other by `tests/ui/test_repaint.c`.
 *
 * Three kinds paint outside the `rel` rectangle and would leave stale pixels if
 * damage reported `rel`:
 *
 *   - `OPTICS_UI_DRAW_BORDER`      straddles the path by `width`, so half of it lies
 *                             outside on every side.
 *   - `OPTICS_UI_DRAW_IMAGE`       draws an outline underlay expanded by
 *                             `outline_width` on all four sides.
 *   - `OPTICS_UI_DRAW_ICON`        blits a baked coverage mask that carries a halo for
 *                             the stroke and its AA fringe.
 *
 * Over-reporting is safe (a scissored repaint is idempotent); under-reporting
 * freezes pixels, which is the failure this exists to prevent. So each expansion
 * is derived from the same value replay uses and biased outward. */
static inline optics_gfx_rect optics_ui__cmd_paint_extent(optics_gfx_rect box,
                                                          const optics_ui_draw_cmd *c) {
    const optics_gfx_rect rel_rect = optics_ui__cmd_rel_rect(box, c);
    if (!c)
        return rel_rect;
    switch (c->kind) {
    case OPTICS_UI_DRAW_CLIP_PUSH:
    case OPTICS_UI_DRAW_CLIP_POP:
        /* Paint nothing themselves; including them would re-widen every clipped
         * subtree to its clip rectangle. */
        return (optics_gfx_rect){0, 0, 0, 0};
    case OPTICS_UI_DRAW_TEXT: {
        /* Positioned from the advance its widget recorded, exactly as replay
         * positions it. Neither consumer shapes here. */
        optics_gfx_rect tb =
            optics_ui__text_bounds(rel_rect, c->text_advance, c->rel.w < 0.0f, c->rel.h < 0.0f);
        if (tb.w > 0.0f && tb.h > 0.0f && c->outline_width > 0.0f &&
            (c->outline_color >> 24) != 0) {
            const float e = c->outline_width;
            tb = (optics_gfx_rect){tb.x - e, tb.y - e, tb.w + 2.0f * e, tb.h + 2.0f * e};
        }
        return tb;
    }
    case OPTICS_UI_DRAW_BORDER: {
        if (!(c->width > 0.0f) || (c->color >> 24) == 0)
            return (optics_gfx_rect){0, 0, 0, 0};
        /* Half the stroke sits outside the box, plus 1 logical px to absorb
         * replay's `snap_rect` shift and SDF/path AA fringe at any scale. */
        const float e = c->width * 0.5f + 1.0f;
        return (optics_gfx_rect){rel_rect.x - e, rel_rect.y - e, rel_rect.w + 2.0f * e,
                                 rel_rect.h + 2.0f * e};
    }
    case OPTICS_UI_DRAW_IMAGE: {
        const float e = c->outline_width > 0.0f ? c->outline_width : 0.0f;
        return (optics_gfx_rect){rel_rect.x - e, rel_rect.y - e, rel_rect.w + 2.0f * e,
                                 rel_rect.h + 2.0f * e};
    }
    case OPTICS_UI_DRAW_ICON: {
        /* replay blits the mask at `origin - halo` with extent `px + 2*halo`, where
         * the halo covers half the stroke, the full outline twice over, and the AA
         * fringe (render/icon_mask.c:306-317). Halos are computed in device pixels
         * but scale 1:1 with logical units inside a node, so the same expression
         * holds here. The +2 absorbs the fringe rounding. */
        const float stroke = c->width > 0.0f ? c->width : 0.0f;
        const float outline = c->outline_width > 0.0f ? c->outline_width : 0.0f;
        const float e = stroke * 0.5f + outline * 2.0f + 2.0f;
        return (optics_gfx_rect){rel_rect.x - e, rel_rect.y - e, rel_rect.w + 2.0f * e,
                                 rel_rect.h + 2.0f * e};
    }
    case OPTICS_UI_DRAW_RECT:
    default:
        return rel_rect;
    }
}

static inline optics_gfx_rect optics_ui__node_paint_bounds(const optics_ui_node *n) {
    if (!n || !n->cmds)
        return (optics_gfx_rect){0, 0, 0, 0};
    optics_gfx_rect d = {0, 0, 0, 0};
    for (uint32_t i = 0; i < n->cmd_count; i++) {
        const optics_ui_draw_cmd *c = &n->cmds[i];
        if (c->kind == OPTICS_UI_DRAW_CLIP_PUSH || c->kind == OPTICS_UI_DRAW_CLIP_POP)
            continue;
        const optics_gfx_rect paint = optics_ui__cmd_paint_extent(n->final_rect, c);
        if (d.w <= 0.0f || d.h <= 0.0f) {
            d = paint;
        } else if (paint.w > 0.0f && paint.h > 0.0f) {
            const float x0 = fminf(d.x, paint.x);
            const float y0 = fminf(d.y, paint.y);
            const float x1 = fmaxf(d.x + d.w, paint.x + paint.w);
            const float y1 = fmaxf(d.y + d.h, paint.y + paint.h);
            d = (optics_gfx_rect){x0, y0, x1 - x0, y1 - y0};
        }
    }
    if (n->has_frame_clip)
        d = optics_ui__rect_intersect(d, n->frame_clip);
    return d;
}

/* Exponential ease toward `target`. Pass the `ui` so a value still in transit
 * marks the frame as animating (`ui->anim_pending`): an input-driven host can
 * then schedule the next frame and let the animation settle instead of
 * freezing mid-fade when input stops. `ui` may be NULL (e.g. tests). Under
 * reduced motion the value resolves to `target` immediately.
 *
 * The value must be **terminal**: within one epsilon of the target it snaps
 * there rather than continuing to approach. A pure exponential ease only
 * arrives at infinity, and the last float ulps are indistinguishable from an
 * animation to every consumer downstream —
 *
 *   * a node's change detection compares `hover_t` against `last_hover_t`, so
 *     a value that still moves by 1e-7 marks the node changed, bumps its
 *     visual revision, and emits damage on every frame, forever;
 *   * `chrome_damaged` then keeps the pacer's active cadence alive;
 *   * the backend therefore stays subscribed to the display pulse train and
 *     commits a bufferless buffer at the panel rate for the rest of the
 *     session, with no pixel ever different.
 *
 * Measured before this snap existed: a static window with the pointer
 * untouched held the client at ~70 wakeups/second and the compositor's
 * `Render/3D` near its animated baseline, because a hover fade that had
 * visually finished never stopped "changing". Snapping within the same epsilon
 * that already defines "arrived" makes a settled value exactly equal to its
 * target, so the next frame's comparison finds no change and quiescence is
 * reachable. */
static inline float optics_ui__approach(optics_ui *ui, float cur, float target, float dt,
                                        float rate) {
    /* Arrived is arrived: the threshold is shared with the `anim_pending` test
     * below so the two can never disagree about whether motion is over. */
    const float OPTICS_UI_ARRIVE_EPS = 0.0015f;
    if (ui && ui->reduced_motion)
        return target;
    float k = dt * rate;
    float next = (k >= 1.0f) ? target : cur + (target - cur) * k;
    if (fabsf(next - target) <= OPTICS_UI_ARRIVE_EPS)
        next = target;
    if (ui && next != target) {
        ui->anim_pending = true;
    }
    return next;
}

/* premultiplied-colour linear lerp via gfx core */
static inline optics_gfx_color optics_ui__lerp_color(optics_gfx_color a, optics_gfx_color b,
                                                     float t) {
    optics_gfx_vec4 la = optics_gfx_color_to_linear(a);
    optics_gfx_vec4 lb = optics_gfx_color_to_linear(b);
    optics_gfx_vec4 r = {
        la.x + (lb.x - la.x) * t,
        la.y + (lb.y - la.y) * t,
        la.z + (lb.z - la.z) * t,
        la.w + (lb.w - la.w) * t,
    };
    return optics_gfx_color_from_linear(r);
}

/* Retarget a premultiplied colour to alpha `a`, rescaling RGB proportionally so
 * the premultiplied sRGB invariant (R, G, B <= A) holds. Replacing only the
 * alpha byte left RGB > A, which decoded as overdriven grey instead of a
 * translucent tint. */
static inline optics_gfx_color optics_ui__color_alpha(optics_gfx_color c, uint8_t a) {
    uint32_t old_a = (c >> 24) & 0xFFu;
    if (old_a == 0 || a == 0)
        return 0;
    float s = (float)a / (float)old_a;
    uint32_t r = (uint32_t)((float)((c >> 16) & 0xFFu) * s + 0.5f);
    uint32_t g = (uint32_t)((float)((c >> 8) & 0xFFu) * s + 0.5f);
    uint32_t b = (uint32_t)((float)(c & 0xFFu) * s + 0.5f);
    if (r > a)
        r = a;
    if (g > a)
        g = a;
    if (b > a)
        b = a;
    return ((uint32_t)a << 24) | (r << 16) | (g << 8) | b;
}

/* Scale a premultiplied colour's RGBA channels by `o` (clamped 0..1) — the
 * fade bake behind optics_ui_set_opacity. Preserves the premultiplied sRGB
 * invariant (R, G, B <= A) required by optics_gfx_decode_premul_srgb. */
static inline optics_gfx_color optics_ui__opacity_color(optics_gfx_color c, float o) {
    if (o <= 0.0f)
        return 0;
    if (o >= 1.0f)
        return c;
    uint32_t a = (uint32_t)((float)((c >> 24) & 0xFFu) * o + 0.5f);
    uint32_t r = (uint32_t)((float)((c >> 16) & 0xFFu) * o + 0.5f);
    uint32_t g = (uint32_t)((float)((c >> 8) & 0xFFu) * o + 0.5f);
    uint32_t b = (uint32_t)((float)(c & 0xFFu) * o + 0.5f);
    return (a << 24) | (r << 16) | (g << 8) | b;
}

#endif /* OPTICS_UI_INTERNAL_H */
