/* tree.c — container stack, child wiring, and the layout-input setters. */

#include "../internal.h"

optics_ui_node *optics_ui__open_container(optics_ui *ui) {
    return ui->cont_top ? ui->cont_stack[ui->cont_top - 1] : nullptr;
}

void optics_ui__link_child(optics_ui *ui, optics_ui_node *n) {
    optics_ui_node *parent = optics_ui__open_container(ui);
    if (!parent || !n || n == parent)
        return;
    ui->last_node = n; /* optics_ui_a11y target */
    if (n->parent == parent && n->last_seen == ui->frame) {
        /* Reusing one retained id for two siblings makes the latter silently
         * disappear: there is only one node to link and one draw list to
         * retain. Surface the caller error for frame diagnostics instead of
         * making a broken list look like a layout-solver failure. */
        ui->duplicate_ids = true;
        return;
    }
    n->parent = parent;
    n->next_sibling = nullptr;
    if (parent->last_child)
        parent->last_child->next_sibling = n;
    else
        parent->first_child = n;
    parent->last_child = n;
    parent->child_count++;
    /* Fold the child id sequence into a per-frame hash so the damage pass
     * (optics_ui__mark_subtree_changed) notices removed/reordered children even
     * when every remaining child is individually unchanged — otherwise a
     * stale display-list record would replay ghost content. */
    parent->child_hash = parent->child_hash * 31 + (uint32_t)(n->id ^ (n->id >> 32));

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
    if (ui->have_next_col_span) {
        n->col_span = ui->next_col_span;
        ui->have_next_col_span = false;
    }
    if (ui->have_next_row_span) {
        n->row_span = ui->next_row_span;
        ui->have_next_row_span = false;
    }
    if (ui->have_next_grid_pos) {
        n->grid_col = ui->next_grid_col;
        n->grid_row = ui->next_grid_row;
        ui->have_next_grid_pos = false;
    }
}

void optics_ui__open_container_push(optics_ui *ui, optics_ui_node *n) {
    /* Internal contract: callers pass a live context and a stored node.
     * The guards are analyzer teeth, not error handling — a NULL here is
     * a library bug, and the overflow path keeps the diagnostic. */
    if (!ui || !n)
        return;
    if (ui->cont_top < OPTICS_UI_CONTAINER_STACK_MAX)
        ui->cont_stack[ui->cont_top++] = n;
    else
        optics_ui__set_overflow(ui);
    if (ui->id_top < OPTICS_UI_ID_STACK_MAX)
        ui->id_stack[ui->id_top++] = n->id; /* children scope under it */
    else
        optics_ui__set_overflow(ui);
}

void optics_ui__open_container_pop(optics_ui *ui) {
    if (ui->cont_top > 1)
        ui->cont_top--; /* never pop the root */
    if (ui->id_top > 1)
        ui->id_top--;
}

/* ---- public container API ---- */

static optics_ui_node *open_flex(optics_ui *ui, optics_ui_axis axis, optics_ui_layout_opts opts) {
    /* Explicit box.id gives the container a stable, label-independent
     * identity; otherwise fall back to the sibling-sequence id. */
    optics_ui_id id = (opts.box.id && opts.box.id[0])
                          ? optics_ui__gen_widget_id(ui, opts.box.id)
                          : optics_ui__gen_container_id(ui, axis == OPTICS_UI_ROW ? "row" : "col");
    optics_ui_node *n = optics_ui__store_touch(ui, id);
    if (!n)
        return nullptr;
    optics_ui__link_child(ui, n); /* link into parent BEFORE pushing */
    optics_ui__node_box(ui, n, &opts.box);
    n->is_container = true;
    n->axis = axis;
    n->align = opts.align;
    n->gap = opts.gap;
    n->pad = opts.pad;
    n->cross = opts.cross;
    n->wrap = opts.wrap;
    n->row_gap = opts.row_gap > 0.0f ? opts.row_gap : opts.gap;

    /* Optional panel background — painted at replay against final_rect,
     * so it fills the container after layout has solved its size.
     * optics_gfx_color is ARGB-packed (alpha in bits 31..24); any non-zero
     * alpha enables the fill. */
    if ((opts.bg >> 24) != 0) {
        optics_ui__drawlist_push(ui, n,
                                 (optics_ui_draw_cmd){.kind = OPTICS_UI_DRAW_RECT,
                                                      .rel = {0, 0, 0, 0},
                                                      .color = opts.bg,
                                                      .corner_radii = opts.corner_radii,
                                                      .corner_curvature = opts.corner_curvature});
    }
    if ((opts.border >> 24) != 0 && opts.border_width > 0.0f) {
        optics_ui__drawlist_push(ui, n,
                                 (optics_ui_draw_cmd){.kind = OPTICS_UI_DRAW_BORDER,
                                                      .rel = {0, 0, 0, 0},
                                                      .color = opts.border,
                                                      .corner_radii = opts.corner_radii,
                                                      .corner_curvature = opts.corner_curvature,
                                                      .width = opts.border_width});
    }

    optics_ui__open_container_push(ui, n);
    return n;
}

void optics_ui_row_begin(optics_ui *ui, const optics_ui_layout_opts *opts) {
    optics_ui_layout_opts default_opts = {
        .gap = ui ? ui->theme.gap : 0, .pad = 0, .cross = OPTICS_UI_STRETCH};
    open_flex(ui, OPTICS_UI_ROW, opts ? *opts : default_opts);
}
void optics_ui_row_end(optics_ui *ui) {
    optics_ui__open_container_pop(ui);
}
void optics_ui_column_begin(optics_ui *ui, const optics_ui_layout_opts *opts) {
    optics_ui_layout_opts default_opts = {
        .gap = ui ? ui->theme.gap : 0, .pad = 0, .cross = OPTICS_UI_STRETCH};
    open_flex(ui, OPTICS_UI_COLUMN, opts ? *opts : default_opts);
}
void optics_ui_column_end(optics_ui *ui) {
    optics_ui__open_container_pop(ui);
}

void optics_ui_wrap_begin(optics_ui *ui, const optics_ui_layout_opts *opts) {
    optics_ui_layout_opts default_opts = {
        .gap = ui ? ui->theme.gap : 0, .pad = 0, .cross = OPTICS_UI_START, .wrap = true};
    optics_ui_layout_opts o = opts ? *opts : default_opts;
    o.wrap = true;
    open_flex(ui, OPTICS_UI_ROW, o);
}
void optics_ui_wrap_end(optics_ui *ui) {
    optics_ui__open_container_pop(ui);
}

static optics_ui_node *open_stack(optics_ui *ui, optics_ui_layout_opts opts) {
    optics_ui_id id = (opts.box.id && opts.box.id[0]) ? optics_ui__gen_widget_id(ui, opts.box.id)
                                                      : optics_ui__gen_container_id(ui, "stack");
    optics_ui_node *n = optics_ui__store_touch(ui, id);
    if (!n)
        return nullptr;
    optics_ui__link_child(ui, n);
    optics_ui__node_box(ui, n, &opts.box);
    n->is_container = true;
    n->is_stack = true;
    n->align = opts.align;
    n->cross = opts.cross;
    n->pad = opts.pad;

    if ((opts.bg >> 24) != 0) {
        optics_ui__drawlist_push(ui, n,
                                 (optics_ui_draw_cmd){.kind = OPTICS_UI_DRAW_RECT,
                                                      .rel = {0, 0, 0, 0},
                                                      .color = opts.bg,
                                                      .corner_radii = opts.corner_radii,
                                                      .corner_curvature = opts.corner_curvature});
    }
    if ((opts.border >> 24) != 0 && opts.border_width > 0.0f) {
        optics_ui__drawlist_push(ui, n,
                                 (optics_ui_draw_cmd){.kind = OPTICS_UI_DRAW_BORDER,
                                                      .rel = {0, 0, 0, 0},
                                                      .color = opts.border,
                                                      .corner_radii = opts.corner_radii,
                                                      .corner_curvature = opts.corner_curvature,
                                                      .width = opts.border_width});
    }

    optics_ui__open_container_push(ui, n);
    return n;
}

void optics_ui_stack_begin(optics_ui *ui, const optics_ui_layout_opts *opts) {
    optics_ui_layout_opts default_opts = {.pad = 0, .cross = OPTICS_UI_STRETCH};
    open_stack(ui, opts ? *opts : default_opts);
}
void optics_ui_stack_end(optics_ui *ui) {
    optics_ui__open_container_pop(ui);
}

static optics_ui_node *open_grid(optics_ui *ui, optics_ui_grid_opts opts) {
    optics_ui_id id = (opts.box.id && opts.box.id[0]) ? optics_ui__gen_widget_id(ui, opts.box.id)
                                                      : optics_ui__gen_container_id(ui, "grid");
    optics_ui_node *n = optics_ui__store_touch(ui, id);
    if (!n)
        return nullptr;
    optics_ui__link_child(ui, n);
    optics_ui__node_box(ui, n, &opts.box);
    n->is_container = true;
    n->is_grid = true;
    n->grid_columns = opts.columns > 0 ? (uint32_t)opts.columns : 1;
    n->gap = opts.col_gap >= 0.0f ? opts.col_gap : 0.0f;
    n->grid_row_gap = opts.row_gap >= 0.0f ? opts.row_gap : 0.0f;
    n->grid_row_height = opts.row_height >= 0.0f ? opts.row_height : 0.0f;
    n->pad = opts.pad >= 0.0f ? opts.pad : 0.0f;
    n->align = opts.align;
    n->cross = opts.cross != 0 ? opts.cross : OPTICS_UI_STRETCH;

    if ((opts.bg >> 24) != 0) {
        optics_ui__drawlist_push(ui, n,
                                 (optics_ui_draw_cmd){.kind = OPTICS_UI_DRAW_RECT,
                                                      .rel = {0, 0, 0, 0},
                                                      .color = opts.bg,
                                                      .corner_radii = opts.corner_radii,
                                                      .corner_curvature = opts.corner_curvature});
    }
    if ((opts.border >> 24) != 0 && opts.border_width > 0.0f) {
        optics_ui__drawlist_push(ui, n,
                                 (optics_ui_draw_cmd){.kind = OPTICS_UI_DRAW_BORDER,
                                                      .rel = {0, 0, 0, 0},
                                                      .color = opts.border,
                                                      .corner_radii = opts.corner_radii,
                                                      .corner_curvature = opts.corner_curvature,
                                                      .width = opts.border_width});
    }

    optics_ui__open_container_push(ui, n);
    return n;
}

void optics_ui_grid_begin(optics_ui *ui, const optics_ui_grid_opts *opts) {
    optics_ui_grid_opts default_opts = {
        .columns = 1,
        .col_gap = ui ? ui->theme.gap : 0,
        .row_gap = ui ? ui->theme.gap : 0,
        .cross = OPTICS_UI_STRETCH,
    };
    open_grid(ui, opts ? *opts : default_opts);
}
void optics_ui_grid_end(optics_ui *ui) {
    optics_ui__open_container_pop(ui);
}

void optics_ui_close(optics_ui *ui) {
    optics_ui__open_container_pop(ui);
}

optics_ui_response optics_ui_pressable_begin(optics_ui *ui, const optics_ui_pressable_opts *opts) {
    optics_ui_response empty = {};
    if (!ui)
        return empty;

    optics_ui_pressable_opts default_opts = {};
    if (!opts)
        opts = &default_opts;

    optics_ui_layout_opts lopts = opts->layout;
    lopts.box = optics_ui__merge_box(lopts.box, opts->box);

    optics_ui_node *n = open_flex(ui, OPTICS_UI_ROW, lopts);
    if (!n)
        return empty;

    bool disabled = lopts.box.disabled;
    for (optics_ui_node *parent = n->parent; parent; parent = parent->parent)
        disabled = disabled || parent->box_disabled;
    optics_ui_response r = optics_ui__interact(ui, n, true, disabled);
    if (opts->mouse_button == OPTICS_UI_MOUSE_RIGHT)
        r.clicked = r.right_clicked;
    else if (opts->mouse_button == OPTICS_UI_MOUSE_MIDDLE)
        r.clicked = r.middle_clicked;
    uint32_t sem_flags =
        (r.focused ? OPTICS_UI_A11Y_FOCUSED : 0) | (disabled ? OPTICS_UI_A11Y_DISABLED : 0);
    optics_ui__node_semantics(ui, n, OPTICS_UI_ROLE_BUTTON, opts->label ? opts->label : "", nullptr,
                              sem_flags);

    if (!disabled) {
        float dt = ui->input.dt_seconds;
        n->hover_t =
            optics_ui__approach(ui, n->hover_t, (r.hovered || r.focused) ? 1.0f : 0.0f, dt, 14.0f);
        n->active_t =
            optics_ui__approach(ui, n->active_t, (ui->active_id == n->id) ? 1.0f : 0.0f, dt, 18.0f);
    }

    float emphasis = fmaxf(n->hover_t * 0.72f, n->active_t);
    if (emphasis > 0.001f) {
        optics_gfx_color transparent = optics_gfx_color_rgba(0, 0, 0, 0);
        optics_gfx_color target =
            n->active_t > n->hover_t * 0.72f ? ui->theme.color_active : ui->theme.color_hover;
        optics_ui__drawlist_push(
            ui, n,
            (optics_ui_draw_cmd){.kind = OPTICS_UI_DRAW_RECT,
                                 .rel = {0, 0, 0, 0},
                                 .color = optics_ui__lerp_color(transparent, target, emphasis),
                                 .corner_radii = lopts.corner_radii,
                                 .corner_curvature = lopts.corner_curvature});
    }
    if (r.focused) {
        float bw = ui->theme.border_width > 0.0f ? ui->theme.border_width : 1.5f;
        optics_ui__drawlist_push(ui, n,
                                 (optics_ui_draw_cmd){.kind = OPTICS_UI_DRAW_BORDER,
                                                      .rel = {0, 0, 0, 0},
                                                      .color = ui->theme.color_accent,
                                                      .corner_radii = lopts.corner_radii,
                                                      .corner_curvature = lopts.corner_curvature,
                                                      .width = bw});
    }

    ui->last_response = r;
    return r;
}

void optics_ui_pressable_end(optics_ui *ui) {
    if (ui)
        optics_ui_close(ui);
}

/* Descriptor fields are applied by every node builder. Legacy layout
 * hints remain supported; explicit nonzero descriptor values take precedence. */
optics_ui_box optics_ui__merge_box(optics_ui_box base, optics_ui_box override) {
#define OVERRIDE(field)                                                                            \
    if (override.field)                                                                            \
    base.field = override.field
    OVERRIDE(id);
    OVERRIDE(flex);
    OVERRIDE(width);
    OVERRIDE(height);
    OVERRIDE(min_width);
    OVERRIDE(max_width);
    OVERRIDE(min_height);
    OVERRIDE(max_height);
    OVERRIDE(col_span);
    OVERRIDE(row_span);
    OVERRIDE(grid_col);
    OVERRIDE(grid_row);
    OVERRIDE(align);
    OVERRIDE(cross);
    OVERRIDE(disabled);
    OVERRIDE(error);
    OVERRIDE(tooltip);
#undef OVERRIDE
    if (override.style.fields)
        base.style = override.style;
    return base;
}

/* Arm the frame's tooltip and measure the box it will paint.
 *
 * Both parts matter. `active` is what replay keys on, and `painted` is what
 * damage reports — the painted box sits *below* the anchor with `2*pad` padding,
 * so the anchor is neither a subset of it nor a superset. Measuring here is
 * forced: the damage walk is a `const ui *` pass and must not re-shape text,
 * because shaping mutates glyph-cache state the next submit depends on.
 *
 * `painted` deliberately survives the `active = false` reset at `optics_ui_begin`, so a
 * dismissed tooltip still knows which band it vacated. */
static void optics_ui__tooltip_arm(optics_ui *ui, const char *text, optics_gfx_rect anchor) {
    const float pad = 4.0f;
    ui->tooltip.active = true;
    ui->tooltip.anchor = anchor;
    ui->tooltip.opacity = ui->opacity;
    ui->tooltip.text = optics_ui__arena_strndup(ui, text, strlen(text));

    const float size = optics_ui__font_px(ui, ui->theme.font_size * 0.85f);
    const optics_ui_text_metrics tm =
        optics_ui__text_measure_label(ui, ui->tooltip.text, size, 0.0f);
    ui->tooltip.painted = (optics_gfx_rect){anchor.x, anchor.y + anchor.h + pad,
                                            tm.width + 2.0f * pad, tm.height + 2.0f * pad};
}

void optics_ui__node_box(optics_ui *ui, optics_ui_node *n, const optics_ui_box *box) {
    n->min_w = box->min_width;
    n->max_w = box->max_width;
    n->min_h = box->min_height;
    n->max_h = box->max_height;
    if (box->col_span > 0)
        n->col_span = (uint32_t)box->col_span;
    if (box->row_span > 0)
        n->row_span = (uint32_t)box->row_span;
    if (box->grid_col > 0)
        n->grid_col = box->grid_col;
    if (box->grid_row > 0)
        n->grid_row = box->grid_row;
    if (box->align != 0)
        n->align = box->align;
    if (box->cross != 0)
        n->cross = box->cross;
    n->box_disabled = box->disabled;
    if (box->flex != 0)
        n->flex_grow = box->flex;
    if (box->width > 0)
        n->fixed_w = box->width;
    if (box->height > 0)
        n->fixed_h = box->height;
    if (box->tooltip && n->has_prev && optics_ui__point_in(ui->input.cursor, n->prev_rect) &&
        !optics_ui__point_clipped_by_scroll(n, ui->input.cursor) &&
        !optics_ui__widget_occluded(ui, n)) {
        optics_ui__tooltip_arm(ui, box->tooltip, n->prev_rect);
    }
}

void optics_ui_space_between(optics_ui *ui) {
    optics_ui_node *n = ui ? optics_ui__open_container(ui) : nullptr;
    if (n)
        n->space_between = true;
}

void optics_ui_fit(optics_ui *ui) {
    optics_ui_node *n = ui ? optics_ui__open_container(ui) : nullptr;
    if (n)
        n->fit = true;
}

/* Positional layout hints for the next node (widget or container). */
void optics_ui_flex(optics_ui *ui, float grow) {
    if (!ui)
        return;
    ui->next_flex = grow;
    ui->have_next_flex = true;
}
void optics_ui_size(optics_ui *ui, float w, float h) {
    if (!ui)
        return;
    ui->next_w = w;
    ui->next_h = h;
    ui->have_next_size = true;
}
void optics_ui_col_span(optics_ui *ui, uint32_t span) {
    if (!ui)
        return;
    ui->next_col_span = span > 0 ? span : 1;
    ui->have_next_col_span = true;
}
void optics_ui_row_span(optics_ui *ui, uint32_t span) {
    if (!ui)
        return;
    ui->next_row_span = span > 0 ? span : 1;
    ui->have_next_row_span = true;
}
void optics_ui_grid_at(optics_ui *ui, int32_t col, int32_t row) {
    if (!ui)
        return;
    ui->next_grid_col = col;
    ui->next_grid_row = row;
    ui->have_next_grid_pos = true;
}

void optics_ui__apply_box(optics_ui *ui, optics_ui_box box) {
    if (box.flex != 0.0f) {
        if (isfinite(box.flex) && box.flex > 0.0f) {
            ui->next_flex = box.flex;
            ui->have_next_flex = true;
        }
    }
    if (box.width != 0.0f || box.height != 0.0f) {
        float w = isfinite(box.width) && box.width > 0.0f ? box.width : 0.0f;
        float h = isfinite(box.height) && box.height > 0.0f ? box.height : 0.0f;
        ui->next_w = w;
        ui->next_h = h;
        ui->have_next_size = (w > 0.0f || h > 0.0f);
    }
    if (box.col_span > 0) {
        ui->next_col_span = (uint32_t)box.col_span;
        ui->have_next_col_span = true;
    }
    if (box.row_span > 0) {
        ui->next_row_span = (uint32_t)box.row_span;
        ui->have_next_row_span = true;
    }
    if (box.grid_col > 0 || box.grid_row > 0) {
        ui->next_grid_col = box.grid_col;
        ui->next_grid_row = box.grid_row;
        ui->have_next_grid_pos = true;
    }
    if (box.disabled)
        ui->next_disabled = true;
    if (box.error)
        ui->next_error = true;
    /* Per-call style atoms (ADR-0061): staged unconditionally; a zero
     * fields mask is a no-op in the cascade merge, so the box's zeroed
     * default needs no guard flag. Drained by optics_ui__style_effective. */
    ui->next_style = box.style;
}

void optics_ui__set_placeholder(optics_ui *ui, const char *text) {
    if (!ui || !text)
        return;
    ui->next_placeholder = optics_ui__arena_strndup(ui, text, strlen(text));
}

/* Raise a tooltip anchored to the most-recently built widget, shown only
 * while it is hovered. Called by the *_ex wrappers after the body runs. */
void optics_ui__tooltip(optics_ui *ui, const char *text) {
    if (!ui || !text || !text[0])
        return;
    optics_ui_response r = optics_ui_get_response(ui);
    if (!r.hovered)
        return;
    optics_ui__tooltip_arm(ui, text, r.rect);
}

void optics_ui_spacer(optics_ui *ui, float size) {
    /* a leaf with a fixed main-axis extent and no draw commands */
    optics_ui_node *parent = optics_ui__open_container(ui);
    optics_ui_id id = optics_ui__gen_container_id(ui, "spacer"); /* sibling-seq unique */
    optics_ui_node *n = optics_ui__store_touch(ui, id);
    if (!n)
        return;
    optics_ui__link_child(ui, n);
    n->is_container = false;
    bool row = parent && parent->axis == OPTICS_UI_ROW;
    n->fixed_w = row ? size : 0;
    n->fixed_h = row ? 0 : size;
    n->measured = (optics_gfx_point){n->fixed_w, n->fixed_h};
}
