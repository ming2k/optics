/* node.c — per-node frame reset and the lens_node_* escape hatch (ADR-0008). */

#include "../internal.h"

/* Clear the per-frame fields of a node when it is first touched this
 * frame. Persistent fields (id, phase, has_prev, prev_rect, state,
 * animation) are preserved (ADR-0004). */
void lensi_node_reset_frame(lens_node *n) {
    n->parent = NULL;
    n->first_child = n->last_child = n->next_sibling = NULL;
    n->child_count = 0;
    n->child_seq = 0;

    n->is_container = false;
    n->is_scroll = false;
    n->is_overlay = false;
    n->overlay_anchor = (flux_rect){0, 0, 0, 0};
    n->overlay_bounds = (flux_rect){0, 0, 0, 0};
    n->has_overlay_bounds = false;
    n->axis = LENS_ROW;
    n->gap = n->pad = 0.0f;
    n->cross = LENS_STRETCH;
    n->flex_grow = 0.0f;
    n->fixed_w = n->fixed_h = 0.0f;
    n->min_w = n->max_w = 0.0f;
    n->min_h = n->max_h = 0.0f;
    n->scroll_x = n->scroll_y = 0.0f;

    n->measured = (flux_point){0, 0};
    n->final_rect = (flux_rect){0, 0, 0, 0};

    n->last_cmd_hash = n->cmd_hash;
    n->cmd_hash = 0;
    n->cmds = NULL;
    n->cmd_count = n->cmd_cap = 0;

    n->semantics = (lens_semantics){0};
}

/* ---- escape hatch (frame-scoped borrows) ---- */

lens_id lens_node_id(const lens_node *n) {
    return n ? n->id : 0;
}

flux_rect lens_node_bounds(const lens_node *n) {
    return n ? n->final_rect : (flux_rect){0, 0, 0, 0};
}

lens_node_phase lens_node_phase_of(const lens_node *n) {
    return n ? n->phase : LENS_NODE_LEAVING;
}

lens_node *lens_node_parent(const lens_node *n) {
    return n ? n->parent : NULL;
}
lens_node *lens_node_first_child(const lens_node *n) {
    return n ? n->first_child : NULL;
}
lens_node *lens_node_next_sibling(const lens_node *n) {
    return n ? n->next_sibling : NULL;
}

void *lens_node_state(lens_node *n, size_t bytes) {
    if (!n || bytes == 0)
        return NULL;
    if (n->state) {
        /* The API promises a stable address for the lifetime of the node.
         * Growing would invalidate previously-borrowed pointers, so callers
         * must use one fixed state type/size for a given node id. */
        return n->state_bytes == bytes ? n->state : NULL;
    }
    /* Allocate through the owning context's persistent allocator and zero-init
     * on first touch (ADR-0004). Freed at reap or lens_destroy. */
    void *mem = lensi_alloc(n->ui, bytes);
    if (!mem)
        return NULL;
    memset(mem, 0, bytes);
    n->state = mem;
    n->state_bytes = bytes;
    return n->state;
}
