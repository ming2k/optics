/* node.c — per-node frame reset and the lens_node_* escape hatch (ADR-0031). */

#include "../internal.h"

/* Clear the per-frame fields of a node when it is first touched this
 * frame. Persistent fields (id, phase, has_prev, prev_rect, state,
 * animation) are preserved (ADR-0027). */
void lensi_node_reset_frame(optics_ui_node *n) {
    n->parent = nullptr;
    n->first_child = n->last_child = n->next_sibling = nullptr;
    n->child_count = 0;
    n->child_seq = 0;
    n->last_child_hash = n->child_hash;
    n->child_hash = 0;

    n->is_container = false;
    n->is_scroll = false;
    n->has_frame_clip = false;
    n->is_grid = false;
    n->place = OPTICS_UI_PLACE_FLOW;
    n->band = OPTICS_UI_BAND_BASE;
    n->mode = OPTICS_UI_PLACE_EXACT;
    n->place_rect = (optics_gfx_rect){0, 0, 0, 0};
    n->place_bounds = (optics_gfx_rect){0, 0, 0, 0};
    n->has_place_bounds = false;
    n->transient = false;
    n->interactive = false;
    n->axis = OPTICS_UI_ROW;
    n->gap = n->pad = 0.0f;
    n->cross = OPTICS_UI_STRETCH;
    n->align = OPTICS_UI_START;
    n->space_between = n->fit = n->box_disabled = false;
    n->grid_columns = 0;
    n->grid_row_gap = 0.0f;
    n->grid_row_height = 0.0f;
    n->col_span = 1;
    n->row_span = 1;
    n->grid_col = 0;
    n->grid_row = 0;
    n->resolved_col = 0;
    n->resolved_row = 0;
    n->resolved_col_span = 1;
    n->resolved_row_span = 1;
    n->flex_grow = 0.0f;
    n->fixed_w = n->fixed_h = 0.0f;
    n->min_w = n->max_w = 0.0f;
    n->min_h = n->max_h = 0.0f;
    n->scroll_x = n->scroll_y = 0.0f;
    n->scroll_gutter = 0.0f;

    n->measured = (optics_gfx_point){0, 0};
    n->final_rect = (optics_gfx_rect){0, 0, 0, 0};

    n->last_cmd_hash = n->cmd_hash;
    n->cmd_hash = 0;
    /* The command list is arena memory: the arena is reset wholesale each
     * frame, so the pointer must drop. `cmd_cap` deliberately survives:
     * lensi_node_push_cmd sizes the fresh allocation from it in one step
     * instead of re-walking the 8->16->32 doubling chain every frame
     * (each step is an arena alloc plus a memcpy of everything emitted
     * so far). A node that emitted 32 commands last frame allocates
     * exactly once this frame; the retained cap is bounded by the node's
     * own high-water mark and dies with the node. */
    n->cmds = nullptr;
    n->cmd_count = 0;

    n->semantics = (optics_ui_semantics){};
}

/* ---- escape hatch (frame-scoped borrows) ---- */

optics_ui_id optics_ui_node_id(const optics_ui_node *n) {
    return n ? n->id : 0;
}

optics_gfx_rect optics_ui_node_bounds(const optics_ui_node *n) {
    return n ? n->final_rect : (optics_gfx_rect){0, 0, 0, 0};
}

optics_ui_node_phase optics_ui_node_phase_of(const optics_ui_node *n) {
    return n ? n->phase : OPTICS_UI_NODE_LEAVING;
}

optics_ui_node *optics_ui_node_parent(const optics_ui_node *n) {
    return n ? n->parent : nullptr;
}
optics_ui_node *optics_ui_node_first_child(const optics_ui_node *n) {
    return n ? n->first_child : nullptr;
}
optics_ui_node *optics_ui_node_next_sibling(const optics_ui_node *n) {
    return n ? n->next_sibling : nullptr;
}

void *optics_ui_node_state(optics_ui_node *n, size_t bytes) {
    if (!n || bytes == 0)
        return nullptr;
    if (n->state) {
        /* The API promises a stable address for the lifetime of the node.
         * Growing would invalidate previously-borrowed pointers, so callers
         * must use one fixed state type/size for a given node id. */
        return n->state_bytes == bytes ? n->state : nullptr;
    }
    /* Allocate through the owning context's persistent allocator and zero-init
     * on first touch (ADR-0027). Freed at reap or lens_release. */
    void *mem = lensi_alloc(n->ui, bytes);
    if (!mem) {
        lensi_set_overflow(n->ui);
        return nullptr;
    }
    memset(mem, 0, bytes);
    n->state = mem;
    n->state_bytes = bytes;
    return n->state;
}
