/*
 * tess_earcut.c — Pure Ear-Clipping Polygon Tessellator (ADR-0121).
 *
 * Algorithm Vault asset: stateless polygon triangulation.
 */
#include <flux/vault/tess_earcut.h>
#include <math.h>
#include <stdlib.h>

#define MAX_STATIC_POLY 2048

float flux_vault_polygon_signed_area(const flux_point *pts, uint32_t n) {
    if (!pts || n < 3)
        return 0.0f;
    float area = 0.0f;
    for (uint32_t i = 0; i < n; ++i) {
        uint32_t j = (i + 1 == n) ? 0 : (i + 1);
        area += pts[i].x * pts[j].y - pts[j].x * pts[i].y;
    }
    return 0.5f * area;
}

static inline bool point_in_triangle(flux_point p, flux_point a, flux_point b, flux_point c) {
    float cross_ab = (b.x - a.x) * (p.y - a.y) - (b.y - a.y) * (p.x - a.x);
    float cross_bc = (c.x - b.x) * (p.y - b.y) - (c.y - b.y) * (p.x - b.x);
    float cross_ca = (a.x - c.x) * (p.y - c.y) - (a.y - c.y) * (p.x - c.x);

    bool has_neg = (cross_ab < 0.0f) || (cross_bc < 0.0f) || (cross_ca < 0.0f);
    bool has_pos = (cross_ab > 0.0f) || (cross_bc > 0.0f) || (cross_ca > 0.0f);

    return !(has_neg && has_pos);
}

static bool is_convex(flux_point a, flux_point b, flux_point c, bool ccw) {
    float cross = (b.x - a.x) * (c.y - b.y) - (b.y - a.y) * (c.x - b.x);
    return ccw ? (cross > 1e-7f) : (cross < -1e-7f);
}

flux_result flux_vault_earcut_triangulate(const flux_vault_earcut_desc *desc,
                                          flux_vault_earcut_result *out_result) {
    if (!desc || !desc->points || desc->point_count < 3 || !desc->out_indices)
        return FLUX_ERROR_INVALID_ARGUMENT;

    uint32_t n = desc->point_count;
    uint32_t needed_indices = 3 * (n - 2);
    if (desc->index_capacity < needed_indices)
        return FLUX_ERROR_OUT_OF_RANGE;

    float area = flux_vault_polygon_signed_area(desc->points, n);
    if (fabsf(area) < 1e-7f)
        return FLUX_ERROR_INVALID_ARGUMENT;

    bool ccw = (area > 0.0f);

    uint32_t prev_buf[MAX_STATIC_POLY];
    uint32_t next_buf[MAX_STATIC_POLY];
    uint32_t *prev = prev_buf;
    uint32_t *next = next_buf;

    if (n > MAX_STATIC_POLY) {
        prev = (uint32_t *)malloc(n * sizeof(uint32_t));
        next = (uint32_t *)malloc(n * sizeof(uint32_t));
        if (!prev || !next) {
            free(prev);
            free(next);
            return FLUX_ERROR_OUT_OF_MEMORY;
        }
    }

    for (uint32_t i = 0; i < n; ++i) {
        prev[i] = (i == 0) ? (n - 1) : (i - 1);
        next[i] = (i + 1 == n) ? 0 : (i + 1);
    }

    uint32_t curr = 0;
    uint32_t remaining = n;
    uint32_t out_idx = 0;
    uint32_t stall_count = 0;

    while (remaining > 3) {
        uint32_t p_idx = prev[curr];
        uint32_t n_idx = next[curr];

        flux_point a = desc->points[p_idx];
        flux_point b = desc->points[curr];
        flux_point c = desc->points[n_idx];

        bool ear = false;
        if (is_convex(a, b, c, ccw)) {
            ear = true;
            uint32_t test_v = next[n_idx];
            while (test_v != p_idx) {
                if (point_in_triangle(desc->points[test_v], a, b, c)) {
                    ear = false;
                    break;
                }
                test_v = next[test_v];
            }
        }

        if (ear) {
            desc->out_indices[out_idx++] = p_idx;
            desc->out_indices[out_idx++] = curr;
            desc->out_indices[out_idx++] = n_idx;

            next[p_idx] = n_idx;
            prev[n_idx] = p_idx;
            remaining--;
            curr = n_idx;
            stall_count = 0;
        } else {
            curr = n_idx;
            stall_count++;
            if (stall_count > remaining) {
                // Non-simple or self-intersecting polygon stall
                if (n > MAX_STATIC_POLY) {
                    free(prev);
                    free(next);
                }
                return FLUX_ERROR_INVALID_STATE;
            }
        }
    }

    // Emit final triangle
    uint32_t p0 = prev[curr];
    uint32_t p1 = curr;
    uint32_t p2 = next[curr];
    desc->out_indices[out_idx++] = p0;
    desc->out_indices[out_idx++] = p1;
    desc->out_indices[out_idx++] = p2;

    if (n > MAX_STATIC_POLY) {
        free(prev);
        free(next);
    }

    if (out_result) {
        out_result->triangle_count = n - 2;
        out_result->index_count = out_idx;
        out_result->is_clockwise = !ccw;
    }

    return FLUX_OK;
}
