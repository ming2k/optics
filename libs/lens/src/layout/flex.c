/* flex.c — one-dimensional flexbox measure and arrange layout strategy (ADR-0028, ADR-0104). */

#include "../internal.h"

/* Axis projections: "main" follows the container axis, "cross" is perpendicular */
static inline float pt_main(flux_point p, lens_axis a) {
    return a == LENS_ROW ? p.x : p.y;
}
static inline float pt_cross(flux_point p, lens_axis a) {
    return a == LENS_ROW ? p.y : p.x;
}

static inline float node_min_main(const lens_node *n, lens_axis a) {
    return a == LENS_ROW ? n->min_w : n->min_h;
}

static inline float node_max_main(const lens_node *n, lens_axis a) {
    return a == LENS_ROW ? n->max_w : n->max_h;
}

static inline float node_min_cross(const lens_node *n, lens_axis a) {
    return a == LENS_ROW ? n->min_h : n->min_w;
}

static inline float node_max_cross(const lens_node *n, lens_axis a) {
    return a == LENS_ROW ? n->max_h : n->max_w;
}

/* ================================================================== */
/*  Water-filling bounded flex solver                                 */
/* ================================================================== */

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

/* ================================================================== */
/*  Public 1D Flex Strategy Entries                                   */
/* ================================================================== */

flux_point lensi_flex_measure(lens_node *n) {
    float main = 0, cross = 0;
    uint32_t n_children = 0;
    for (lens_node *c = n->first_child; c; c = c->next_sibling) {
        if (c->place == LENS_PLACE_ABS)
            continue;
        flux_point cm = c->measured;
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
    return (n->axis == LENS_ROW) ? (flux_point){main, cross} : (flux_point){cross, main};
}

void lensi_flex_arrange(lens_node *n, flux_rect inner) {
    lens_axis ax = n->axis;
    float inner_main = (ax == LENS_ROW) ? inner.w : inner.h;
    float inner_cross = (ax == LENS_ROW) ? inner.h : inner.w;

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

    /* Reserve scrollbar width so content doesn't render underneath it */
    if (n->is_scroll && ax == LENS_COLUMN && base > inner_main) {
        n->scroll_gutter = n->ui->theme.scrollbar_width;
        inner.w -= n->scroll_gutter;
        if (inner.w < 0.0f)
            inner.w = 0.0f;
        inner_cross = inner.w;
    }

    float used_adjustment = 0;
    for (lens_node *c = n->first_child; c; c = c->next_sibling) {
        if (c->place == LENS_PLACE_ABS)
            continue;
        used_adjustment += flex_adjustment(c, ax, grow_level, true);
        used_adjustment -= flex_adjustment(c, ax, shrink_level, false);
    }
    float remaining = fmaxf(0, free - used_adjustment);
    float gap = n->gap + (n->space_between && cnt > 1 ? remaining / (cnt - 1) : 0);
    float cursor = ((ax == LENS_ROW) ? inner.x : inner.y) +
                   (n->space_between ? 0 : lensi_align_offset(n->align, remaining));
    for (lens_node *c = n->first_child; c; c = c->next_sibling) {
        if (c->place == LENS_PLACE_ABS)
            continue;
        float main_sz = pt_main(c->measured, ax);
        main_sz += flex_adjustment(c, ax, grow_level, true);
        main_sz -= flex_adjustment(c, ax, shrink_level, false);

        if (c->is_scroll && main_sz > inner_main)
            main_sz = inner_main;

        float cross_sz =
            (n->cross == LENS_STRETCH && !c->fit && (ax == LENS_ROW ? c->fixed_h : c->fixed_w) <= 0)
                ? inner_cross
                : pt_cross(c->measured, ax);
        cross_sz = lensi_constrain_extent(cross_sz, node_min_cross(c, ax), node_max_cross(c, ax));
        if (cross_sz > inner_cross && node_min_cross(c, ax) <= inner_cross)
            cross_sz = inner_cross;

        float cross_off = lensi_align_offset(n->cross, inner_cross - cross_sz);
        float cross_pos = ((ax == LENS_ROW) ? inner.y : inner.x) + cross_off;

        flux_rect cr = (ax == LENS_ROW) ? (flux_rect){cursor, cross_pos, main_sz, cross_sz}
                                        : (flux_rect){cross_pos, cursor, cross_sz, main_sz};
        lensi_arrange_node(c, cr);
        cursor += main_sz + gap;
    }

    /* Arrange ABS children (ADR-0060) */
    for (lens_node *c = n->first_child; c; c = c->next_sibling) {
        if (c->place != LENS_PLACE_ABS)
            continue;
        lensi_arrange_node(c, lensi_resolve_abs_rect(n->ui, c));
    }
}
