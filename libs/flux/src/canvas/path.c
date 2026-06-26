/*
 * Path builder. Arena-allocated, dynamically growing.
 * Curves stored as control points; flattening happens at fill time.
 */
#include "internal.h"

#include <math.h>
#include <string.h>

#define FLUX_PATH_INITIAL_CAP 64

static bool path_grow(flux_path *p, flux_arena *arena) {
    uint32_t new_cap = p->capacity ? p->capacity * 2 : FLUX_PATH_INITIAL_CAP;
    flux_path_segment *new_seg = flux_arena_alloc_aligned(
        arena, new_cap * sizeof(flux_path_segment), alignof(flux_path_segment));
    if (!new_seg)
        return false;
    if (p->count > 0)
        memcpy(new_seg, p->segments, p->count * sizeof(flux_path_segment));
    p->segments = new_seg;
    p->capacity = new_cap;
    return true;
}

static void push_segment(flux_path *p, flux_arena *arena, uint32_t op, const float *pts,
                         uint32_t n) {
    if (!p)
        return;
    if (p->count >= p->capacity) {
        if (!path_grow(p, arena)) {
            if (p->dropped == 0) {
                FLUX_FAIL(FLUX_ERROR_OUT_OF_MEMORY, "flux_path arena exhausted; segments dropped");
            }
            p->dropped++;
            return;
        }
    }
    flux_path_segment *s = &p->segments[p->count++];
    s->op = op;
    memset(s->pts, 0, sizeof(s->pts));
    if (pts && n > 0)
        memcpy(s->pts, pts, n * sizeof(float));
}

uint32_t flux_path_dropped_count(const flux_path *p) {
    return p ? p->dropped : 0;
}

flux_result flux_path_create(flux_path **out, flux_arena *arena) {
    if (!out)
        return FLUX_ERROR_INVALID_ARGUMENT;
    *out = nullptr;
    if (!arena) {
        FLUX_FAIL(FLUX_ERROR_INVALID_ARGUMENT, "flux_path_create requires a non-NULL arena");
        return FLUX_ERROR_INVALID_ARGUMENT;
    }
    flux_path *p = flux_arena_alloc_aligned(arena, sizeof(*p), alignof(flux_path));
    if (!p) {
        FLUX_FAIL(FLUX_ERROR_OUT_OF_MEMORY, "arena exhausted while allocating flux_path");
        return FLUX_ERROR_OUT_OF_MEMORY;
    }
    memset(p, 0, sizeof(*p));
    p->arena = arena;
    *out = p;
    return FLUX_OK;
}

void flux_path_move_to(flux_path *p, float x, float y) {
    if (!p)
        return;
    float pts[2] = {x, y};
    push_segment(p, p->arena, FLUX_PATH_MOVE, pts, 2);
    p->cursor_x = x;
    p->cursor_y = y;
}

void flux_path_line_to(flux_path *p, float x, float y) {
    if (!p)
        return;
    float pts[2] = {x, y};
    push_segment(p, p->arena, FLUX_PATH_LINE, pts, 2);
    p->cursor_x = x;
    p->cursor_y = y;
}

void flux_path_quad_to(flux_path *p, float cx, float cy, float x, float y) {
    if (!p)
        return;
    float pts[4] = {cx, cy, x, y};
    push_segment(p, p->arena, FLUX_PATH_QUAD, pts, 4);
    p->cursor_x = x;
    p->cursor_y = y;
}

void flux_path_cubic_to(flux_path *p, float c1x, float c1y, float c2x, float c2y, float x,
                        float y) {
    if (!p)
        return;
    float pts[6] = {c1x, c1y, c2x, c2y, x, y};
    push_segment(p, p->arena, FLUX_PATH_CUBIC, pts, 6);
    p->cursor_x = x;
    p->cursor_y = y;
}

void flux_path_close(flux_path *p) {
    if (!p)
        return;
    push_segment(p, p->arena, FLUX_PATH_CLOSE, nullptr, 0);
}

void flux_path_add_rect(flux_path *p, flux_rect r) {
    if (!p)
        return;
    flux_path_move_to(p, r.x, r.y);
    flux_path_line_to(p, r.x + r.w, r.y);
    flux_path_line_to(p, r.x + r.w, r.y + r.h);
    flux_path_line_to(p, r.x, r.y + r.h);
    flux_path_close(p);
}

void flux_path_add_round_rect(flux_path *p, flux_rect r, float radius) {
    if (!p)
        return;
    if (radius <= 0.0f) {
        flux_path_add_rect(p, r);
        return;
    }
    float rr = radius;
    if (rr > r.w * 0.5f)
        rr = r.w * 0.5f;
    if (rr > r.h * 0.5f)
        rr = r.h * 0.5f;

    if (rr >= r.w * 0.5f - 1e-4f && rr >= r.h * 0.5f - 1e-4f) {
        flux_path_add_circle(p, r.x + r.w * 0.5f, r.y + r.h * 0.5f, rr);
        return;
    }

    const float k = 0.5522847498f * rr;
    float x0 = r.x, y0 = r.y;
    float x1 = r.x + r.w, y1 = r.y + r.h;

    flux_path_move_to(p, x0 + rr, y0);
    flux_path_line_to(p, x1 - rr, y0);
    flux_path_cubic_to(p, x1 - rr + k, y0, x1, y0 + rr - k, x1, y0 + rr);
    flux_path_line_to(p, x1, y1 - rr);
    flux_path_cubic_to(p, x1, y1 - rr + k, x1 - rr + k, y1, x1 - rr, y1);
    flux_path_line_to(p, x0 + rr, y1);
    flux_path_cubic_to(p, x0 + rr - k, y1, x0, y1 - rr + k, x0, y1 - rr);
    flux_path_line_to(p, x0, y0 + rr);
    flux_path_cubic_to(p, x0, y0 + rr - k, x0 + rr - k, y0, x0 + rr, y0);
    flux_path_close(p);
}

void flux_path_add_circle(flux_path *p, float cx, float cy, float radius) {
    if (!p)
        return;
    const float k = 0.5522847498f * radius;
    flux_path_move_to(p, cx + radius, cy);
    flux_path_cubic_to(p, cx + radius, cy + k, cx + k, cy + radius, cx, cy + radius);
    flux_path_cubic_to(p, cx - k, cy + radius, cx - radius, cy + k, cx - radius, cy);
    flux_path_cubic_to(p, cx - radius, cy - k, cx - k, cy - radius, cx, cy - radius);
    flux_path_cubic_to(p, cx + k, cy - radius, cx + radius, cy - k, cx + radius, cy);
    flux_path_close(p);
}
