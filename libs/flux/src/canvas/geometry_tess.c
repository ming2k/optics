#include "internal.h"

#ifdef FLUX_TESS_DEBUG
#include <stdio.h>
#endif

/* ------------------------------------------------------------------ */
/*  Tessellator — ear clipping with hole bridging                     */
/*                                                                    */
/*  Handles non-self-intersecting concave polygons with true holes    */
/*  (CW contour inside a CCW outer). The algorithm:                   */
/*                                                                    */
/*  1. Classify each contour by signed area: positive = CW (hole),    */
/*     negative = CCW (solid).                                        */
/*  2. For each hole, find the rightmost vertex and bridge it to the  */
/*     nearest visible vertex on the containing solid contour.        */
/*  3. The bridged contours are merged into a single polygon that     */
/*     the ear-clipper handles in a single pass.                      */
/*  4. Disjoint solid contours (no holes) are ear-clipped             */
/*     independently.                                                 */
/* ------------------------------------------------------------------ */

float signed_area(const flux_point *pts, uint32_t n) {
    if (n < 3)
        return 0.0f;
    float a = 0.0f;
    for (uint32_t i = 0, j = n - 1; i < n; j = i++) {
        a += (pts[j].x + pts[i].x) * (pts[j].y - pts[i].y);
    }
    return a * 0.5f;
}

static bool point_in_triangle(flux_point p, flux_point a, flux_point b, flux_point c) {
    float d1 = (p.x - b.x) * (a.y - b.y) - (a.x - b.x) * (p.y - b.y);
    float d2 = (p.x - c.x) * (b.y - c.y) - (b.x - c.x) * (p.y - c.y);
    float d3 = (p.x - a.x) * (c.y - a.y) - (c.x - a.x) * (p.y - a.y);
    bool has_neg = d1 < 0 || d2 < 0 || d3 < 0;
    bool has_pos = d1 > 0 || d2 > 0 || d3 > 0;
    return !(has_neg && has_pos);
}

static float cross2d(flux_point a, flux_point b) {
    return a.x * b.y - a.y * b.x;
}

static bool is_inside_contour(flux_point p, const flux_point *pts, uint32_t n) {
    if (n < 3)
        return false;
    int winding = 0;
    for (uint32_t i = 0, j = n - 1; i < n; j = i++) {
        flux_point a = pts[j], b = pts[i];
        if (a.y <= p.y) {
            if (b.y > p.y && cross2d(pt_sub(b, a), pt_sub(p, a)) > 0.0f)
                winding++;
        } else {
            if (b.y <= p.y && cross2d(pt_sub(b, a), pt_sub(p, a)) < 0.0f)
                winding--;
        }
    }
    return winding != 0;
}

static bool segment_intersects_any(flux_point a, flux_point b, const flux_point *pts, uint32_t n,
                                   uint32_t skip_i, uint32_t skip_j) {
    for (uint32_t i = 0, j = n - 1; i < n; j = i++) {
        if (i == skip_i || i == skip_j || j == skip_i || j == skip_j)
            continue;
        flux_point c = pts[j], d = pts[i];
        float d1 = cross2d(pt_sub(d, c), pt_sub(a, c));
        float d2 = cross2d(pt_sub(d, c), pt_sub(b, c));
        float d3 = cross2d(pt_sub(b, a), pt_sub(c, a));
        float d4 = cross2d(pt_sub(b, a), pt_sub(d, a));
        if (((d1 > 0 && d2 < 0) || (d1 < 0 && d2 > 0)) &&
            ((d3 > 0 && d4 < 0) || (d3 < 0 && d4 > 0)))
            return true;
    }
    return false;
}

struct contour_info {
    uint32_t start;
    uint32_t count;
    bool is_hole;
    float sa;
};

static uint32_t find_rightmost(const flux_point *pts, uint32_t n) {
    uint32_t best = 0;
    for (uint32_t i = 1; i < n; ++i) {
        if (pts[i].x > pts[best].x || (pts[i].x == pts[best].x && pts[i].y < pts[best].y))
            best = i;
    }
    return best;
}

/* `outer_start` identifies the outer contour's span inside all_pts so
 * the blocking scan can skip it — `outer` itself may point at a
 * merged copy outside the contour data. */
static int32_t find_bridge_vertex(const flux_point *outer, uint32_t outer_n, flux_point hole_pt,
                                  uint32_t hole_idx, uint32_t outer_start,
                                  const flux_point *all_pts, const struct contour_info *infos,
                                  uint32_t info_count) {
    float best_dist = 1e30f;
    int32_t best = -1;

    for (uint32_t i = 0; i < outer_n; ++i) {
        flux_point v = outer[i];
        float dx = v.x - hole_pt.x;
        float dy = v.y - hole_pt.y;
        if (dy > 0.0f)
            continue;
        float dist = dx * dx + dy * dy;
        if (dist >= best_dist)
            continue;

        if (segment_intersects_any(hole_pt, v, outer, outer_n, i, UINT32_MAX))
            continue;

        bool blocked = false;
        for (uint32_t ci = 0; ci < info_count; ++ci) {
            if (infos[ci].is_hole)
                continue;
            if (infos[ci].start == outer_start)
                continue;
            if (segment_intersects_any(hole_pt, v, all_pts + infos[ci].start, infos[ci].count,
                                       UINT32_MAX, UINT32_MAX)) {
                blocked = true;
                break;
            }
        }
        if (!blocked) {
            best_dist = dist;
            best = (int32_t)i;
        }
    }

    if (best >= 0)
        return best;

    best_dist = 1e30f;
    for (uint32_t i = 0; i < outer_n; ++i) {
        flux_point v = outer[i];
        float dx = v.x - hole_pt.x;
        float dy = v.y - hole_pt.y;
        float dist = dx * dx + dy * dy;
        if (dist >= best_dist)
            continue;
        if (segment_intersects_any(hole_pt, v, outer, outer_n, i, UINT32_MAX))
            continue;
        best_dist = dist;
        best = (int32_t)i;
    }
    return best;
}

static void ensure_ccw(flux_point *pts, uint32_t n) {
    if (signed_area(pts, n) > 0.0f) {
        for (uint32_t i = 0, j = n - 1; i < j; ++i, --j) {
            flux_point t = pts[i];
            pts[i] = pts[j];
            pts[j] = t;
        }
    }
}

static void ensure_cw(flux_point *pts, uint32_t n) {
    if (signed_area(pts, n) < 0.0f) {
        for (uint32_t i = 0, j = n - 1; i < j; ++i, --j) {
            flux_point t = pts[i];
            pts[i] = pts[j];
            pts[j] = t;
        }
    }
}

static void dedup_contour(flux_point *cpts, uint32_t *n, bool closed) {
    if (*n <= 1)
        return;
    uint32_t write = 0;
    for (uint32_t read = 0; read < *n; ++read) {
        flux_point pt = cpts[read];
        flux_point prev = (write > 0) ? cpts[write - 1] : (closed ? cpts[*n - 1] : pt);
        if (!closed || write > 0 || pt.x != prev.x || pt.y != prev.y) {
            if (write == 0 || pt.x != prev.x || pt.y != prev.y)
                cpts[write++] = pt;
        }
    }
    if (closed && write > 1 && cpts[write - 1].x == cpts[0].x && cpts[write - 1].y == cpts[0].y) {
        write--;
    }
    *n = write;
}

static void remove_trailing_dup(flux_point *cpts, uint32_t *n, flux_canvas_contour *co) {
    if (co->closed && *n > 1 && cpts[*n - 1].x == co->first_x && cpts[*n - 1].y == co->first_y) {
        (*n)--;
    }
}

bool ear_clip_contour(flux_canvas_vertex *verts, uint32_t *v_count, uint32_t verts_cap,
                      flux_mat3x2 tx, flux_color color, flux_point *pts, uint32_t *prev,
                      uint32_t *next, uint32_t n) {
    if (n < 3)
        return true;

    uint32_t cur = 0;
    uint32_t remaining = n;
    uint32_t guard = 0;
    while (remaining > 3) {
        uint32_t p = prev[cur], nx = next[cur];
        flux_point a = pts[p], b = pts[cur], e = pts[nx];

        float cross = (b.x - a.x) * (e.y - a.y) - (b.y - a.y) * (e.x - a.x);
        bool is_ear = cross > 0.0f;

        if (is_ear) {
            for (uint32_t j = next[nx]; j != p; j = next[j]) {
                /* Bridge merging duplicates the two bridge endpoints
                 * (bit-exact copies). A duplicate sitting exactly on
                 * one of the ear's corners is the corner, not a
                 * blocking vertex — skipping it keeps the clip from
                 * stalling at the bridge. */
                flux_point q = pts[j];
                if ((q.x == a.x && q.y == a.y) || (q.x == b.x && q.y == b.y) ||
                    (q.x == e.x && q.y == e.y))
                    continue;
                if (point_in_triangle(q, a, b, e)) {
                    is_ear = false;
                    break;
                }
            }
        }

        if (is_ear) {
            emit_tri(verts, v_count, verts_cap, tx, color, a, b, e);
            next[p] = nx;
            prev[nx] = p;
            --remaining;
            cur = p;
            guard = 0;
        } else {
            cur = nx;
            if (++guard > remaining) {
#ifdef FLUX_TESS_DEBUG
                fprintf(stderr, "ear clip stalled: remaining=%u of %u\n", remaining, n);
                uint32_t k = cur;
                for (uint32_t i = 0; i < remaining && i < 24; ++i) {
                    fprintf(stderr, "  [%u] (%.2f, %.2f)\n", k, pts[k].x, pts[k].y);
                    k = next[k];
                }
#endif
                /* Self-intersecting input: no ear exists. Signal the
                 * caller so it can fall back to stencil-then-cover
                 * (ADR-0014) instead of submitting partial geometry. */
                return false;
            }
        }
    }
    uint32_t p = prev[cur], nx = next[cur];
    emit_tri(verts, v_count, verts_cap, tx, color, pts[p], pts[cur], pts[nx]);
    return true;
}

/* Stencil-then-cover fallback (ADR-0014) for fills the ear clip
 * cannot triangulate (self-intersecting contours). Pass 1 draws a
 * triangle fan per contour into the stencil attachment with
 * INCREMENT_AND_WRAP on front faces / DECREMENT_AND_WRAP on back
 * faces — after it, a pixel's stencil value is its nonzero winding
 * count. Pass 2 covers the fill's bounding quad with the paint where
 * stencil != 0, zeroing it for the next fill in the same pass. */
static void fill_path_stencil_cover(flux_canvas *c, const flux_paint *paint, flux_mat3x2 tx,
                                    const flux_point *pts, const struct contour_info *infos,
                                    uint32_t contour_count) {
    flux_canvas_vertex *verts = c->scratch_verts;
    const uint32_t verts_cap = FLUX_CANVAS_PATH_SCRATCH_CAP * 3;
    uint32_t v_count = 0;

    float min_x = 0, min_y = 0, max_x = 0, max_y = 0;
    bool first = true;

    for (uint32_t ci = 0; ci < contour_count; ++ci) {
        if (infos[ci].count < 3)
            continue;
        const flux_point *cpts = pts + infos[ci].start;
        uint32_t n = infos[ci].count;
        for (uint32_t i = 0; i < n; ++i) {
            if (first || cpts[i].x < min_x)
                min_x = cpts[i].x;
            if (first || cpts[i].y < min_y)
                min_y = cpts[i].y;
            if (first || cpts[i].x > max_x)
                max_x = cpts[i].x;
            if (first || cpts[i].y > max_y)
                max_y = cpts[i].y;
            first = false;
        }
        /* Fan from vertex 0. Winding (and thus facing) is preserved;
         * the stencil ops count it rather than requiring CCW. */
        for (uint32_t i = 1; i + 1 < n; ++i) {
            emit_tri(verts, &v_count, verts_cap, tx, 0u, cpts[0], cpts[i], cpts[i + 1]);
        }
    }
    if (first || v_count == 0)
        return;

    submit_triangles_id(c, nullptr, CANVAS_PIPE_STENCIL_WRITE, verts, v_count);

    /* Cover quad over the (transformed) bounding box. Affine maps it
     * to a parallelogram that still bounds the transformed fill. */
    flux_color color = paint ? paint->color : 0u;
    v_count = 0;
    flux_point bl = {min_x, min_y}, br = {max_x, min_y};
    flux_point tr = {max_x, max_y}, tl = {min_x, max_y};
    emit_tri(verts, &v_count, verts_cap, tx, color, bl, br, tr);
    emit_tri(verts, &v_count, verts_cap, tx, color, bl, tr, tl);

    canvas_pipe_id cover = CANVAS_PIPE_COVER_SOLID;
    if (paint &&
        (paint->kind == FLUX_PAINT_LINEAR_GRADIENT || paint->kind == FLUX_PAINT_RADIAL_GRADIENT))
        cover = CANVAS_PIPE_COVER_GRADIENT;
    submit_triangles_id(c, paint, cover, verts, v_count);
}

void flux_canvas_fill_path(flux_canvas *c, const flux_path *p, const flux_paint *paint) {
    if (!c || !c->recording || !p || !paint)
        return;
    if (p->count == 0)
        return;

    flux_point *pts = c->scratch_pts;
    flux_canvas_contour *cons = c->scratch_contours;

    flux_mat3x2 tx = c->states[c->state_top].transform;
    float pixel_scale = flux_canvas_mat3x2_pixel_scale(tx);
    flatten_multi fm = flatten_path_to_contours(p, pixel_scale, pts, FLUX_CANVAS_PATH_SCRATCH_CAP,
                                                cons, FLUX_CANVAS_MAX_CONTOURS);
    if (fm.contour_count == 0 || fm.point_count < 3)
        return;

    flux_color color = paint->color;
    flux_canvas_vertex *verts = c->scratch_verts;
    const uint32_t verts_cap = FLUX_CANVAS_PATH_SCRATCH_CAP * 3;
    uint32_t v_count = 0;

    uint32_t *lnk_prev = c->scratch_lnk_prev;
    uint32_t *lnk_next = c->scratch_lnk_next;

    struct contour_info infos[FLUX_CANVAS_MAX_CONTOURS];
    uint32_t hole_count = 0;
    for (uint32_t ci = 0; ci < fm.contour_count; ++ci) {
        flux_canvas_contour *co = &cons[ci];
        infos[ci] = (struct contour_info){0};
        if (co->count < 3)
            continue;

        flux_point *cpts = pts + co->start;
        uint32_t n = co->count;

        remove_trailing_dup(cpts, &n, co);
        dedup_contour(cpts, &n, co->closed);
        if (n < 3) {
            co->count = 0;
            continue;
        }
        co->count = n;

        float sa = signed_area(cpts, n);
        bool is_hole = (sa > 0.0f);
        if (!is_hole)
            ensure_ccw(cpts, n);
        else
            ensure_cw(cpts, n);

        infos[ci] = (struct contour_info){
            .start = co->start,
            .count = n,
            .is_hole = is_hole,
            .sa = sa,
        };
        if (is_hole)
            hole_count++;
    }

    /* Snapshot for the stencil fallback: the merge loop below zeroes
     * the counts of holes it consumes, but the fallback needs every
     * original contour. */
    struct contour_info infos_orig[FLUX_CANVAS_MAX_CONTOURS];
    memcpy(infos_orig, infos, sizeof(infos[0]) * fm.contour_count);

    bool stalled = false;

    if (hole_count == 0) {
        for (uint32_t ci = 0; ci < fm.contour_count; ++ci) {
            if (infos[ci].count < 3)
                continue;
            flux_point *cpts = pts + infos[ci].start;
            uint32_t n = infos[ci].count;
            for (uint32_t i = 0; i < n; ++i) {
                lnk_prev[i] = (i == 0) ? n - 1 : i - 1;
                lnk_next[i] = (i + 1 == n) ? 0 : i + 1;
            }
            if (!ear_clip_contour(verts, &v_count, verts_cap, tx, color, cpts, lnk_prev, lnk_next,
                                  n)) {
                stalled = true;
            }
        }
        if (stalled && c->stencil_view != VK_NULL_HANDLE) {
            fill_path_stencil_cover(c, paint, tx, pts, infos, fm.contour_count);
            return;
        }
        submit_triangles(c, paint, verts, v_count);
        return;
    }

    for (uint32_t ci = 0; ci < fm.contour_count; ++ci) {
        if (infos[ci].count < 3 || infos[ci].is_hole)
            continue;
        flux_point *outer_pts = pts + infos[ci].start;
        uint32_t outer_n = infos[ci].count;
        ensure_ccw(outer_pts, outer_n);

        uint32_t merged_cap = outer_n;
        for (uint32_t hj = 0; hj < fm.contour_count; ++hj) {
            if (infos[hj].count < 3 || !infos[hj].is_hole)
                continue;
            flux_point *hole_pts = pts + infos[hj].start;
            uint32_t hole_n = infos[hj].count;
            if (!is_inside_contour(hole_pts[0], outer_pts, outer_n))
                continue;
            merged_cap += hole_n + 2;
        }

        /* Build the merged polygon in storage DISJOINT from the
         * contour data: growing the outer's span in place would
         * shift the tail over later contours — including the very
         * hole points about to be inserted — and could write past
         * the scratch end when the outer starts deep in the array.
         * Prefer the free scratch tail after the flattened points;
         * fall back to a heap buffer for polygons that don't fit
         * (this also lifts the old hard cap on merged size). */
        flux_point *merged;
        uint32_t *m_prev, *m_next;
        flux_point *heap_pts = nullptr;
        uint32_t *heap_lnk = nullptr;
        if (fm.point_count + merged_cap <= FLUX_CANVAS_PATH_SCRATCH_CAP) {
            merged = pts + fm.point_count;
            m_prev = lnk_prev;
            m_next = lnk_next;
        } else {
            heap_pts = flux_internal_alloc(c->device, merged_cap * sizeof(*heap_pts));
            heap_lnk = flux_internal_alloc(c->device, 2u * merged_cap * sizeof(*heap_lnk));
            if (!heap_pts || !heap_lnk) {
                flux_internal_free(c->device, heap_pts);
                flux_internal_free(c->device, heap_lnk);
                FLUX_FAIL(FLUX_ERROR_OUT_OF_MEMORY,
                          "merged-polygon buffer allocation failed; fill dropped");
                continue;
            }
            merged = heap_pts;
            m_prev = heap_lnk;
            m_next = heap_lnk + merged_cap;
        }
        memcpy(merged, outer_pts, outer_n * sizeof(flux_point));

        uint32_t merged_n = outer_n;

        for (uint32_t hj = 0; hj < fm.contour_count; ++hj) {
            if (infos[hj].count < 3 || !infos[hj].is_hole)
                continue;
            flux_point *hole_pts = pts + infos[hj].start;
            uint32_t hole_n = infos[hj].count;

            if (!is_inside_contour(hole_pts[0], merged, merged_n))
                continue;

            ensure_cw(hole_pts, hole_n);

            uint32_t h_rm = find_rightmost(hole_pts, hole_n);
            int32_t bridge = find_bridge_vertex(merged, merged_n, hole_pts[h_rm], hj,
                                                infos[ci].start, pts, infos, fm.contour_count);
            if (bridge < 0)
                continue;

            if (merged_n + hole_n + 2 > merged_cap)
                continue;

            flux_point bridge_pt = merged[bridge];
            flux_point hole_bridge = hole_pts[h_rm];

            memmove(merged + bridge + 1 + hole_n + 2, merged + bridge + 1,
                    (merged_n - bridge - 1) * sizeof(flux_point));

            /* Inserted walk: hole[h], hole[h+1] … hole[h-1], hole[h],
             * outer[b] — exactly hole_n + 2 points, matching the
             * memmove gap above. The bridge endpoints repeat
             * non-adjacently; adjacent duplicates would stall the
             * ear clip on zero-cross corners. */
            merged[bridge + 1] = hole_bridge;
            for (uint32_t k = 0; k + 1 < hole_n; ++k) {
                uint32_t src = (h_rm + 1 + k) % hole_n;
                merged[bridge + 2 + k] = hole_pts[src];
            }
            merged[bridge + 1 + hole_n] = hole_bridge;
            merged[bridge + 2 + hole_n] = bridge_pt;

            merged_n += hole_n + 2;
            infos[hj].count = 0;
        }

        for (uint32_t i = 0; i < merged_n; ++i) {
            m_prev[i] = (i == 0) ? merged_n - 1 : i - 1;
            m_next[i] = (i + 1 == merged_n) ? 0 : i + 1;
        }
        if (!ear_clip_contour(verts, &v_count, verts_cap, tx, color, merged, m_prev, m_next,
                              merged_n)) {
            stalled = true;
        }

        flux_internal_free(c->device, heap_pts);
        flux_internal_free(c->device, heap_lnk);
    }

    for (uint32_t ci = 0; ci < fm.contour_count; ++ci) {
        if (infos[ci].count < 3 || !infos[ci].is_hole)
            continue;
        if (infos[ci].count == 0)
            continue;

        flux_point *cpts = pts + infos[ci].start;
        uint32_t n = infos[ci].count;
        float sa = signed_area(cpts, n);
        if (sa > 0.0f) {
            for (uint32_t i = 0, j = n - 1; i < j; ++i, --j) {
                flux_point t = cpts[i];
                cpts[i] = cpts[j];
                cpts[j] = t;
            }
        }
        for (uint32_t i = 0; i < n; ++i) {
            lnk_prev[i] = (i == 0) ? n - 1 : i - 1;
            lnk_next[i] = (i + 1 == n) ? 0 : i + 1;
        }
        if (!ear_clip_contour(verts, &v_count, verts_cap, tx, color, cpts, lnk_prev, lnk_next, n)) {
            stalled = true;
        }
    }

    if (stalled && c->stencil_view != VK_NULL_HANDLE) {
        /* Drop the partial triangulation and redo the whole fill via
         * stencil winding — it handles holes natively, so the merged
         * pieces that DID triangulate are simply superseded. */
        fill_path_stencil_cover(c, paint, tx, pts, infos_orig, fm.contour_count);
        return;
    }

    submit_triangles(c, paint, verts, v_count);
}
