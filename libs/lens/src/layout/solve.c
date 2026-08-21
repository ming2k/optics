/* solve.c — two-pass flexbox measure/arrange over the tree (ADR-0028). */

#include "../internal.h"

/* axis helpers: "main" follows the container axis, "cross" is perpendicular */
static float pt_main(flux_point p, lens_axis a) {
    return a == LENS_ROW ? p.x : p.y;
}
static float pt_cross(flux_point p, lens_axis a) {
    return a == LENS_ROW ? p.y : p.x;
}

static float constrain_extent(float value, float min_value, float max_value) {
    if (max_value > 0.0f)
        value = fminf(value, max_value);
    /* A contradictory range resolves in favour of the minimum, matching the
     * rule that content must not collapse below its usability floor. */
    if (min_value > 0.0f)
        value = fmaxf(value, min_value);
    return value;
}

static float node_min_main(const lens_node *n, lens_axis a) {
    return a == LENS_ROW ? n->min_w : n->min_h;
}

static float node_max_main(const lens_node *n, lens_axis a) {
    return a == LENS_ROW ? n->max_w : n->max_h;
}

static float node_min_cross(const lens_node *n, lens_axis a) {
    return a == LENS_ROW ? n->min_h : n->min_w;
}

static float node_max_cross(const lens_node *n, lens_axis a) {
    return a == LENS_ROW ? n->max_h : n->max_w;
}

/* ---- pass 1: measure (bottom-up) ---- */

static flux_point measure(lens_node *n) {
    if (!n->is_container) {
        /* leaf: fixed hint wins, else the size set during build */
        flux_point m = n->measured;
        if (n->fixed_w > 0)
            m.x = n->fixed_w;
        if (n->fixed_h > 0)
            m.y = n->fixed_h;
        m.x = constrain_extent(m.x, n->min_w, n->max_w);
        m.y = constrain_extent(m.y, n->min_h, n->max_h);
        n->measured = m;
        return m;
    }

    float main = 0, cross = 0;
    uint32_t n_children = 0;
    for (lens_node *c = n->first_child; c; c = c->next_sibling) {
        flux_point cm = measure(c);
        /* ABS children are measured (their subtree needs its intrinsic
         * size for placement) but are NOT accumulated into the parent's
         * main/cross extent (ADR-0060). */
        if (c->place == LENS_PLACE_ABS)
            continue;
        main += pt_main(cm, n->axis);
        float cc = pt_cross(cm, n->axis);
        if (cc > cross)
            cross = cc;
        n_children++;
    }
    if (n_children > 1)
        main += n->gap * (float)(n_children - 1);
    main += 2.0f * n->pad;
    cross += 2.0f * n->pad;

    flux_point m = (n->axis == LENS_ROW) ? (flux_point){main, cross} : (flux_point){cross, main};
    if (n->fixed_w > 0)
        m.x = n->fixed_w;
    if (n->fixed_h > 0)
        m.y = n->fixed_h;
    m.x = constrain_extent(m.x, n->min_w, n->max_w);
    m.y = constrain_extent(m.y, n->min_h, n->max_h);
    n->measured = m;
    return m;
}

/* ---- pass 2: arrange (top-down) ---- */

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
static flux_rect resolve_abs_rect(const lens *ui, const lens_node *n) {
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
            float up = n->place_rect.y - h; /* flip above */
            if (up >= area.y)
                y = up;
            else
                y = (area.h > h) ? area_bottom - h : area.y;
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

static float align_offset(lens_align a, float free) {
    switch (a) {
    case LENS_CENTER:
        return free * 0.5f;
    case LENS_END:
        return free;
    case LENS_START:
    case LENS_STRETCH:
    default:
        return 0.0f;
    }
}

/* Return the water-filling level for a bounded flex adjustment. Grow uses
 * flex_grow as its weight; shrink preserves the existing proportional-to-
 * intrinsic-size behaviour. Children that hit a max/min constraint leave
 * their unused share for the remaining flexible siblings.
 *
 * Complexity note (measured, do not "fix" blindly): this looks like an
 * O(n²) loop — iterate until stable, each pass scanning all flexible
 * children. In practice each pass caps every child whose capacity/weight
 * ratio falls in the same tier, so convergence takes a handful of passes
 * even on adversarial ratio ladders (n=500, quadratic capacity ladders:
 * ≤10 passes). A closed-form sort+prefix-scan replacement (O(n log n),
 * exact same result — verified over 400k randomized cases) measured 2–3×
 * SLOWER at every realistic size (8 children: 14 µs vs 5 µs per 100k
 * solves) purely from qsort constant overhead. Keep the iterative form
 * unless a real workload shows otherwise. */
static float flex_level(const lens_node *parent, lens_axis axis, float space, bool grow) {
    if (space <= 0.0f)
        return 0.0f;

    float total_weight = 0.0f;
    for (const lens_node *c = parent->first_child; c; c = c->next_sibling) {
        if (c->place == LENS_PLACE_ABS || c->flex_grow <= 0.0f)
            continue;
        float base = pt_main(c->measured, axis);
        total_weight += grow ? c->flex_grow : base;
    }
    if (total_weight <= 0.0f)
        return 0.0f;

    float level = space / total_weight;
    for (uint32_t iteration = 0; iteration <= parent->child_count; iteration++) {
        float capped_space = 0.0f;
        float active_weight = 0.0f;
        for (const lens_node *c = parent->first_child; c; c = c->next_sibling) {
            if (c->place == LENS_PLACE_ABS || c->flex_grow <= 0.0f)
                continue;
            float base = pt_main(c->measured, axis);
            float weight = grow ? c->flex_grow : base;
            if (weight <= 0.0f)
                continue;

            float capacity;
            if (grow) {
                float maximum = node_max_main(c, axis);
                capacity = maximum > 0.0f ? fmaxf(maximum - base, 0.0f) : INFINITY;
            } else {
                capacity = fmaxf(base - node_min_main(c, axis), 0.0f);
            }

            if (isfinite(capacity) && capacity / weight <= level) {
                capped_space += capacity;
            } else {
                active_weight += weight;
            }
        }

        if (active_weight <= 0.0f)
            return INFINITY;
        float next_level = fmaxf(space - capped_space, 0.0f) / active_weight;
        if (fabsf(next_level - level) <= 0.0001f)
            return next_level;
        level = next_level;
    }
    return level;
}

static float flex_adjustment(const lens_node *n, lens_axis axis, float level, bool grow) {
    if (n->flex_grow <= 0.0f || level <= 0.0f)
        return 0.0f;
    float base = pt_main(n->measured, axis);
    float weight = grow ? n->flex_grow : base;
    if (weight <= 0.0f)
        return 0.0f;

    float capacity;
    if (grow) {
        float maximum = node_max_main(n, axis);
        capacity = maximum > 0.0f ? fmaxf(maximum - base, 0.0f) : INFINITY;
    } else {
        capacity = fmaxf(base - node_min_main(n, axis), 0.0f);
    }
    return fminf(capacity, weight * level);
}

static void arrange(lens_node *n, flux_rect rect) {
    n->final_rect = rect;
    n->prev_rect = rect; /* becomes next frame's hit-test rect */
    n->has_prev = true;

    if (!n->is_container || !n->first_child)
        return;

    lens_axis ax = n->axis;
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
    float inner_main = (ax == LENS_ROW) ? inner.w : inner.h;
    float inner_cross = (ax == LENS_ROW) ? inner.h : inner.w;

    /* base = sum of measured main extents; free = leftover for grow.
     * Flexible children also form the shrink pool when their intrinsic sizes
     * do not fit. Fixed siblings keep their requested size while the flexible
     * content yields the deficit, matching the common sidebar | content |
     * inspector layout. ABS children take no part in the flow accounting
     * (ADR-0060): they are resolved by placement mode afterwards. */
    float base = 0;
    uint32_t cnt = 0;
    for (lens_node *c = n->first_child; c; c = c->next_sibling) {
        if (c->place == LENS_PLACE_ABS)
            continue;
        base += pt_main(c->measured, ax);
        cnt++;
    }
    if (cnt > 1)
        base += n->gap * (float)(cnt - 1);
    float free = inner_main - base;
    float grow_space = free > 0 ? free : 0;
    float shrink_space = free < 0 ? -free : 0;
    float grow_level = flex_level(n, ax, grow_space, true);
    float shrink_level = flex_level(n, ax, shrink_space, false);

    /* reserve scrollbar width so content doesn't render underneath it */
    if (n->is_scroll && ax == LENS_COLUMN && base > inner_main) {
        n->scroll_gutter = n->ui->theme.scrollbar_width;
        inner.w -= n->scroll_gutter;
        if (inner.w < 0.0f)
            inner.w = 0.0f;
        inner_cross = inner.w;
    }

    float cursor = (ax == LENS_ROW) ? inner.x : inner.y;
    for (lens_node *c = n->first_child; c; c = c->next_sibling) {
        if (c->place == LENS_PLACE_ABS)
            continue; /* resolved after the flow children (ADR-0060) */
        float main_sz = pt_main(c->measured, ax);
        main_sz += flex_adjustment(c, ax, grow_level, true);
        main_sz -= flex_adjustment(c, ax, shrink_level, false);

        /* A scroll container must stay within the parent's viewport so its
         * content can actually overflow and trigger scrolling. Without this
         * clamp the container simply grows to its content height and never
         * sees a viewport smaller than its content. */
        if (c->is_scroll && main_sz > inner_main)
            main_sz = inner_main;

        float cross_sz = (n->cross == LENS_STRETCH) ? inner_cross : pt_cross(c->measured, ax);
        cross_sz = constrain_extent(cross_sz, node_min_cross(c, ax), node_max_cross(c, ax));
        if (cross_sz > inner_cross)
            cross_sz = inner_cross;

        float cross_off = align_offset(n->cross, inner_cross - cross_sz);
        float cross_pos = ((ax == LENS_ROW) ? inner.y : inner.x) + cross_off;

        flux_rect cr = (ax == LENS_ROW) ? (flux_rect){cursor, cross_pos, main_sz, cross_sz}
                                        : (flux_rect){cross_pos, cursor, cross_sz, main_sz};
        arrange(c, cr);
        cursor += main_sz + n->gap;
    }

    /* ABS children: measured but excluded from the flow above, each is now
     * resolved by its placement mode and its subtree arranged recursively
     * (ADR-0060). Sibling order = registration order; the recursion keeps
     * single-measure/single-arrange determinism for nested ABS nodes. */
    for (lens_node *c = n->first_child; c; c = c->next_sibling) {
        if (c->place != LENS_PLACE_ABS)
            continue;
        arrange(c, resolve_abs_rect(n->ui, c));
    }
}

/* ---- scroll clamping (post-arrange) ---- */

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
                flux_rect r = c->final_rect;
                if (r.x < min_x)
                    min_x = r.x;
                if (r.y < min_y)
                    min_y = r.y;
                if (r.x + r.w > max_x)
                    max_x = r.x + r.w;
                if (r.y + r.h > max_y)
                    max_y = r.y + r.h;
            }
            content_w = max_x - min_x;
            content_h = max_y - min_y;
        }

        float max_scroll_x = content_w > viewport_w ? content_w - viewport_w : 0.0f;
        float max_scroll_y = content_h > viewport_h ? content_h - viewport_h : 0.0f;

        float old_x = n->scroll_x;
        float old_y = n->scroll_y;
        if (n->scroll_x < 0.0f)
            n->scroll_x = 0.0f;
        if (n->scroll_x > max_scroll_x)
            n->scroll_x = max_scroll_x;
        if (n->scroll_y < 0.0f)
            n->scroll_y = 0.0f;
        if (n->scroll_y > max_scroll_y)
            n->scroll_y = max_scroll_y;

        float dx = old_x - n->scroll_x;
        float dy = old_y - n->scroll_y;
        if (dx != 0.0f || dy != 0.0f) {
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

void lensi_layout_solve(lens *ui) {
    if (!ui->root)
        return;
    measure(ui->root);
    flux_rect display = {
        0,
        0,
        ui->input.display_size.x,
        ui->input.display_size.y,
    };
    if (display.w <= 0)
        display.w = ui->root->measured.x;
    if (display.h <= 0)
        display.h = ui->root->measured.y;
    arrange(ui->root, display);
    lensi_scroll_clamp(ui);
}
