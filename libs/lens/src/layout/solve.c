/* solve.c — layout tree solver and lifecycle driver (ADR-0028, ADR-0104). */

#include "../internal.h"

/* ================================================================== */
/*  Pass 1: Measure (bottom-up traversal)                             */
/* ================================================================== */

static flux_point measure(lens_node *n) {
    if (!n->is_container) {
        /* Leaf: explicit size hints win, else intrinsic measured size */
        flux_point m = n->measured;
        if (n->fixed_w > 0)
            m.x = n->fixed_w;
        if (n->fixed_h > 0)
            m.y = n->fixed_h;
        m.x = lensi_constrain_extent(m.x, n->min_w, n->max_w);
        m.y = lensi_constrain_extent(m.y, n->min_h, n->max_h);
        n->measured = m;
        return m;
    }

    for (lens_node *c = n->first_child; c; c = c->next_sibling) {
        measure(c);
    }

    /* Container measure delegated to layout strategy */
    flux_point m;
    if (n->is_grid)
        m = lensi_grid_measure(n);
    else if (n->is_stack)
        m = lensi_stack_measure(n);
    else
        m = lensi_flex_measure(n);
    if (n->fixed_w > 0)
        m.x = n->fixed_w;
    if (n->fixed_h > 0)
        m.y = n->fixed_h;
    m.x = lensi_constrain_extent(m.x, n->min_w, n->max_w);
    m.y = lensi_constrain_extent(m.y, n->min_h, n->max_h);
    n->measured = m;
    return m;
}

/* ================================================================== */
/*  Pass 2: Arrange (top-down traversal)                              */
/* ================================================================== */

/* Placement area for an ABS node: its place_bounds intersected with the
 * display, else the whole display (ADR-0060 item 3/5). */
static flux_rect resolve_place_area(const lens *ui, const lens_node *n) {
    float dw = ui->input.display_size.x;
    float dh = ui->input.display_size.y;
    flux_rect area = {0.0f, 0.0f, dw, dh};
    if (n->has_place_bounds) {
        float right = n->place_bounds.x + n->place_bounds.w;
        float bottom = n->place_bounds.y + n->place_bounds.h;
        area.x = fmaxf(0.0f, n->place_bounds.x);
        area.y = fmaxf(0.0f, n->place_bounds.y);
        right = dw > 0.0f ? fminf(dw, right) : right;
        bottom = dh > 0.0f ? fminf(dh, bottom) : bottom;
        area.w = fmaxf(0.0f, right - area.x);
        area.h = fmaxf(0.0f, bottom - area.y);
    }
    return area;
}

/* Resolve an ABS node's rect from its placement mode against the measured
 * size (ADR-0060): EXACT takes place_rect's top-left, ANCHORED drops below
 * the anchor and flips above on overflow, CENTERED centres on the area;
 * every mode clamps onto the placement area. */
flux_rect lensi_resolve_abs_rect(const lens *ui, const lens_node *n) {
    flux_rect area = resolve_place_area(ui, n);
    float w = n->measured.x;
    float h = n->measured.y;
    float x, y;
    switch (n->mode) {
    case LENS_PLACE_CENTERED:
        x = area.x + ((area.w > w) ? (area.w - w) * 0.5f : 0.0f);
        y = area.y + ((area.h > h) ? (area.h - h) * 0.5f : 0.0f);
        break;
    case LENS_PLACE_ANCHORED: {
        x = n->place_rect.x;
        y = n->place_rect.y + n->place_rect.h; /* below the anchor */
        float area_bottom = area.y + area.h;
        if (area.h > 0.0f && y + h > area_bottom) {
            float above = n->place_rect.y - h;
            if (above >= area.y)
                y = above; /* flip above anchor if it fits */
        }
        break;
    }
    case LENS_PLACE_EXACT:
    default:
        x = n->place_rect.x;
        y = n->place_rect.y;
        break;
    }
    float area_right = area.x + area.w;
    float area_bottom = area.y + area.h;
    if (area.w > 0.0f && x + w > area_right)
        x = area_right - w;
    if (x < area.x)
        x = area.x;
    if (area.h > 0.0f && y + h > area_bottom)
        y = area_bottom - h;
    if (y < area.y)
        y = area.y;
    return (flux_rect){x, y, w, h};
}

void lensi_arrange_node(lens_node *n, flux_rect rect) {
    n->final_rect = rect;

    if (!n->is_container || !n->first_child)
        return;

    flux_rect inner = {
        rect.x + n->pad,
        rect.y + n->pad,
        rect.w - 2.0f * n->pad,
        rect.h - 2.0f * n->pad,
    };
    if (n->is_scroll) {
        inner.x -= n->scroll_x;
        inner.y -= n->scroll_y;
    }

    /* Container arrange delegated to layout strategy */
    if (n->is_grid) {
        lensi_grid_arrange(n, inner);
    } else if (n->is_stack) {
        lensi_stack_arrange(n, inner);
    } else {
        lensi_flex_arrange(n, inner);
    }
}

/* ================================================================== */
/*  Scroll clamping (post-arrange)                                    */
/* ================================================================== */

static void shift_subtree(lens_node *n, float dx, float dy) {
    n->final_rect.x += dx;
    n->final_rect.y += dy;
    n->prev_rect.x += dx;
    n->prev_rect.y += dy;
    for (lens_node *c = n->first_child; c; c = c->next_sibling)
        if (c->place != LENS_PLACE_ABS)
            shift_subtree(c, dx, dy); /* ABS subtrees are placed, not scrolled (ADR-0060) */
}

static void scroll_clamp_node(lens_node *n) {
    if (n->is_scroll && n->first_child) {
        /* Content extent is the union of the FLOW child rects, seeded from
         * the first flow child — not from the viewport corner. Seeding from
         * the viewport inflates the union whenever a large single-frame delta
         * flings every child past the viewport edge (an 800px wheel flick
         * reads as 800px of content) and the clamp then allows overshoot.
         * ABS children are placed outside the flow (ADR-0060) and never
         * count toward the scrollable content. */
        lens_node *seed = NULL;
        for (lens_node *c = n->first_child; c; c = c->next_sibling) {
            if (c->place != LENS_PLACE_ABS) {
                seed = c;
                break;
            }
        }
        float viewport_w = n->final_rect.w - 2.0f * n->pad;
        float viewport_h = n->final_rect.h - 2.0f * n->pad;
        float content_w = viewport_w;
        float content_h = viewport_h;
        if (seed) {
            float min_x = seed->final_rect.x;
            float min_y = seed->final_rect.y;
            float max_x = min_x + seed->final_rect.w;
            float max_y = min_y + seed->final_rect.h;
            for (lens_node *c = seed->next_sibling; c; c = c->next_sibling) {
                if (c->place == LENS_PLACE_ABS)
                    continue;
                if (c->final_rect.x < min_x)
                    min_x = c->final_rect.x;
                if (c->final_rect.y < min_y)
                    min_y = c->final_rect.y;
                float right = c->final_rect.x + c->final_rect.w;
                float bottom = c->final_rect.y + c->final_rect.h;
                if (right > max_x)
                    max_x = right;
                if (bottom > max_y)
                    max_y = bottom;
            }
            float union_w = max_x - min_x;
            float union_h = max_y - min_y;
            if (union_w > content_w)
                content_w = union_w;
            if (union_h > content_h)
                content_h = union_h;
        }

        float max_scroll_x = fmaxf(0.0f, content_w - viewport_w);
        float max_scroll_y = fmaxf(0.0f, content_h - viewport_h);

        float clamped_x = fmaxf(0.0f, fminf(n->scroll_x, max_scroll_x));
        float clamped_y = fmaxf(0.0f, fminf(n->scroll_y, max_scroll_y));

        float dx = n->scroll_x - clamped_x;
        float dy = n->scroll_y - clamped_y;
        if (dx != 0.0f || dy != 0.0f) {
            n->scroll_x = clamped_x;
            n->scroll_y = clamped_y;
            for (lens_node *c = n->first_child; c; c = c->next_sibling)
                if (c->place != LENS_PLACE_ABS)
                    shift_subtree(c, dx, dy);
        }

        /* Layout does not emit (ADR-0059): scrollbar chrome moved to the
         * post-layout finalize walk (skin/scrollbar.c), which reads these
         * solved rects and also persists the thumb geometry for next
         * frame's hit-testing. Only the clamped offsets — layout state —
         * are persisted here. */
        lens_scroll_state *ss = (lens_scroll_state *)lens_node_state(n, sizeof(lens_scroll_state));
        if (ss) {
            ss->offset_x = n->scroll_x;
            ss->offset_y = n->scroll_y;
        }
    }

    for (lens_node *c = n->first_child; c; c = c->next_sibling)
        scroll_clamp_node(c);
}

void lensi_scroll_clamp(lens *ui) {
    if (ui->root)
        scroll_clamp_node(ui->root);
}

/* ================================================================== */
/*  Public Solver Entry Point                                         */
/* ================================================================== */

void lensi_layout_solve(lens *ui) {
    if (!ui->root)
        return;
    (void)measure(ui->root);
    flux_rect display = {0, 0, ui->input.display_size.x, ui->input.display_size.y};
    if (display.w <= 0)
        display.w = ui->root->measured.x;
    if (display.h <= 0)
        display.h = ui->root->measured.y;
    lensi_arrange_node(ui->root, display);
    lensi_scroll_clamp(ui);
}
