/* tree.c — container stack, child wiring, and the layout-input setters. */

#include "../internal.h"

lens_node *lensi_open_container(lens *ui) {
    return ui->cont_top ? ui->cont_stack[ui->cont_top - 1] : NULL;
}

void lensi_link_child(lens *ui, lens_node *n) {
    lens_node *parent = lensi_open_container(ui);
    if (!parent || !n || n == parent)
        return;
    ui->last_node = n; /* lens_a11y target */
    if (n->parent == parent && n->last_seen == ui->frame)
        return; /* already linked this frame */
    n->parent = parent;
    n->next_sibling = NULL;
    if (parent->last_child)
        parent->last_child->next_sibling = n;
    else
        parent->first_child = n;
    parent->last_child = n;
    parent->child_count++;

    /* apply any pending per-widget modifiers */
    if (ui->have_next_flex) {
        n->flex_grow = ui->next_flex;
        ui->have_next_flex = false;
    }
    if (ui->have_next_size) {
        n->fixed_w = ui->next_w;
        n->fixed_h = ui->next_h;
        ui->have_next_size = false;
    }
}

void lensi_open_container_push(lens *ui, lens_node *n) {
    if (ui->cont_top < LENSI_CONTAINER_STACK_MAX)
        ui->cont_stack[ui->cont_top++] = n;
    else
        ui->overflow = true;
    if (ui->id_top < LENSI_ID_STACK_MAX)
        ui->id_stack[ui->id_top++] = n->id; /* children scope under it */
    else
        ui->overflow = true;
}

void lensi_open_container_pop(lens *ui) {
    if (ui->cont_top > 1)
        ui->cont_top--; /* never pop the root */
    if (ui->id_top > 1)
        ui->id_top--;
}

/* ---- public container API ---- */

static void open_flex(lens *ui, lens_axis axis, lens_layout_opts opts) {
    /* Explicit box.id gives the container a stable, label-independent
     * identity; otherwise fall back to the sibling-sequence id. */
    lens_id id = (opts.box.id && opts.box.id[0])
                     ? lensi_gen_widget_id(ui, opts.box.id)
                     : lensi_gen_container_id(ui, axis == LENS_ROW ? "row" : "col");
    lens_node *n = lensi_store_touch(ui, id);
    if (!n)
        return;
    lensi_link_child(ui, n); /* link into parent BEFORE pushing */
    n->is_container = true;
    n->axis = axis;
    n->gap = opts.gap;
    n->pad = opts.pad;
    n->cross = opts.cross;
    /* container's own main-axis grow. A descriptor flex wins; otherwise keep
     * any pending lens_flex(...) link_child already applied, so the documented
     * "lens_flex applies to the next node (widget OR container)" holds for the
     * terse lens_row/lens_column too. */
    if (opts.box.flex != 0)
        n->flex_grow = opts.box.flex;
    if (opts.box.width > 0)
        n->fixed_w = opts.box.width;
    if (opts.box.height > 0)
        n->fixed_h = opts.box.height;
    if (opts.min_width > 0)
        n->min_w = opts.min_width;
    if (opts.max_width > 0)
        n->max_w = opts.max_width;
    if (opts.min_height > 0)
        n->min_h = opts.min_height;
    if (opts.max_height > 0)
        n->max_h = opts.max_height;

    /* Optional panel background — painted at replay against final_rect,
     * so it fills the container after layout has solved its size.
     * flux_color is ARGB-packed (alpha in bits 31..24); any non-zero
     * alpha enables the fill. */
    if ((opts.bg >> 24) != 0) {
        lensi_drawlist_push(ui, n,
                            (lens_draw_cmd){.kind = LENS_DRAW_RECT,
                                            .rel = {0, 0, 0, 0},
                                            .color = opts.bg,
                                            .radius = opts.radius});
    }

    lensi_open_container_push(ui, n);
}

void lens_row_ex(lens *ui, lens_layout_opts opts) {
    open_flex(ui, LENS_ROW, opts);
}
void lens_column_ex(lens *ui, lens_layout_opts opts) {
    open_flex(ui, LENS_COLUMN, opts);
}

void lens_row(lens *ui) {
    lens_row_ex(ui, (lens_layout_opts){.gap = ui->theme.gap, .pad = 0, .cross = LENS_STRETCH});
}
void lens_column(lens *ui) {
    lens_column_ex(ui, (lens_layout_opts){.gap = ui->theme.gap, .pad = 0, .cross = LENS_STRETCH});
}

void lens_close(lens *ui) {
    lensi_open_container_pop(ui);
}

/* ---- descriptor plumbing (internal; drained by the next widget body) ----
 *
 * The *_ex wrappers stage an lens_box's fields here just before invoking
 * the terse widget body, which consumes and clears them. Because a box
 * is applied immediately before the one call it belongs to, there is no
 * "applies to the next/last widget" ambiguity at the public surface. */

/* Positional layout hints for the next node (widget or container). */
void lens_flex(lens *ui, float grow) {
    ui->next_flex = grow;
    ui->have_next_flex = true;
}
void lens_size(lens *ui, float w, float h) {
    ui->next_w = w;
    ui->next_h = h;
    ui->have_next_size = true;
}

void lensi_apply_box(lens *ui, lens_box box) {
    if (box.flex != 0) {
        ui->next_flex = box.flex;
        ui->have_next_flex = true;
    }
    if (box.width != 0 || box.height != 0) {
        ui->next_w = box.width;
        ui->next_h = box.height;
        ui->have_next_size = true;
    }
    if (box.disabled)
        ui->next_disabled = true;
    if (box.error)
        ui->next_error = true;
}

void lensi_set_placeholder(lens *ui, const char *text) {
    if (!ui || !text)
        return;
    size_t len = strlen(text) + 1;
    char *copy = flux_arena_alloc(&ui->arena, len);
    if (copy) {
        memcpy(copy, text, len);
        ui->next_placeholder = copy;
    }
}

/* Raise a tooltip anchored to the most-recently built widget, shown only
 * while it is hovered. Called by the *_ex wrappers after the body runs. */
void lensi_tooltip(lens *ui, const char *text) {
    if (!ui || !text || !text[0])
        return;
    lens_response r = lens_get_response(ui);
    if (!r.hovered)
        return;
    ui->tooltip.active = true;
    ui->tooltip.anchor = r.rect;
    size_t len = strlen(text);
    if (len >= sizeof ui->tooltip.text)
        len = sizeof ui->tooltip.text - 1;
    memcpy(ui->tooltip.text, text, len);
    ui->tooltip.text[len] = '\0';
}

void lens_spacer(lens *ui, float size) {
    /* a leaf with a fixed main-axis extent and no draw commands */
    lens_node *parent = lensi_open_container(ui);
    lens_id id = lensi_gen_container_id(ui, "spacer"); /* sibling-seq unique */
    lens_node *n = lensi_store_touch(ui, id);
    if (!n)
        return;
    lensi_link_child(ui, n);
    n->is_container = false;
    bool row = parent && parent->axis == LENS_ROW;
    n->fixed_w = row ? size : 0;
    n->fixed_h = row ? 0 : size;
    n->measured = (flux_point){n->fixed_w, n->fixed_h};
}
