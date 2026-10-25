/*
 * Coverage rasterizer (ADR-0119, stage S1; ADR-0120 Phase A active edge table).
 *
 * Consumes flattened contours and produces an analytic coverage plane: for each
 * pixel, the exact area of the region the fill rule selects. This is the
 * algorithm a coverage backend is built on, and it is deliberately NOT the
 * acceptance harness's method — `tests/flux/coverage_harness.c` clips the
 * polygon to each pixel and sums signed areas, which is easy to trust and far
 * too slow to use. Overlapping the two would make every gate circular; they are
 * kept independent and compared instead (`test_coverage_raster`).
 *
 * Method: sub-band exact integration with an active edge table.
 *
 * The scanline axis is split at every edge endpoint and at every integer row
 * boundary. Inside a sub-band no edge begins or ends, so every active edge spans
 * it and its x is exactly linear in y. Active edges are maintained incrementally
 * via an active edge table (sorted by ytop, active edge list updated at breakpoints),
 * removing the O(bands * vertices) quadratic rescan. Sorting the crossings at the
 * band's midpoint and walking them while accumulating winding direction gives,
 * wherever the running winding selects a region, a span with two straight sides.
 * The span's area inside a pixel column is the exact integral of
 * overlap(xL, xR, [i, i+1)), which is piecewise linear with breakpoints where xL
 * or xR crosses a column boundary.
 *
 * Four correctness details, each of which was a measured defect before it was a
 * comment (ADR-0119 records the numbers):
 *
 *  1. The integration parameter is the BAND's own [0,1], never the row-relative
 *     y. `la`/`lb` span the band, so a row-relative parameter interpolates them
 *     wrongly unless the band happens to fill the entire row — which is why
 *     axis-aligned geometry was exact while curves were not.
 *  2. Edge endpoints are SNAPPED onto the surviving breakpoints. Breakpoint
 *     dedup keeps one representative of a cluster of near-equal values, and
 *     without snapping an edge could end at a band boundary yet still compare as
 *     active in the band below it, leaving that band's winding walk with an odd
 *     crossing count and dropping its whole area.
 *  3. Sub-band splitting handles interior edge crossings using an explicit work
 *     stack bounded by COV_MAX_SPLITS, avoiding quadratic band relocation.
 *  4. A band is accumulated into every row it overlaps, and the crossing order is
 *     taken at the band midpoint rather than at a shared vertex's x.
 *
 * Non-zero and even-odd both work, because winding is accumulated from sorted
 * crossings rather than from an algebraic area sum — so a self-intersecting
 * contour is painted, not cancelled.
 */
#include "internal.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* Bounded splitting. A band needs a split only where two edges cross inside it;
 * the cap turns pathological input into an explicit error rather than unbounded
 * work. */
#define COV_MAX_SPLITS 4096
#define COV_SUBBAND_STACK_CAP 64

typedef struct cov_edge {
    float x0, y0, x1, y1;
    float ytop, ybot; /* snapped to breakpoints */
    float dxdy;       /* (x1 - x0) / (y1 - y0) */
    int dir;          /* +1 descending in y, -1 ascending */
} cov_edge;

typedef struct cov_subband {
    float ya, yb;
} cov_subband;

typedef struct edge_sort_entry {
    float x;
    cov_edge *edge;
} edge_sort_entry;

static inline float edge_x_at(const cov_edge *e, float y) {
    if (e->y1 == e->y0)
        return e->x0;
    return e->x0 + (y - e->y0) * e->dxdy;
}

static int cmp_float(const void *pa, const void *pb) {
    float a = *(const float *)pa;
    float b = *(const float *)pb;
    return (a > b) - (a < b);
}

static int cmp_edge_ytop(const void *pa, const void *pb) {
    const cov_edge *ea = (const cov_edge *)pa;
    const cov_edge *eb = (const cov_edge *)pb;
    if (ea->ytop != eb->ytop)
        return (ea->ytop > eb->ytop) - (ea->ytop < eb->ytop);
    if (ea->ybot != eb->ybot)
        return (ea->ybot > eb->ybot) - (ea->ybot < eb->ybot);
    return (ea->x0 > eb->x0) - (ea->x0 < eb->x0);
}

static int cmp_sort_entry(const void *pa, const void *pb) {
    float xa = ((const edge_sort_entry *)pa)->x;
    float xb = ((const edge_sort_entry *)pb)->x;
    return (xa > xb) - (xa < xb);
}

static inline float snap_to_breakpoint(const float *bp, uint32_t nu, float v) {
    uint32_t lo = 0, hi = nu;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (bp[mid] < v)
            lo = mid + 1;
        else
            hi = mid;
    }
    if (lo >= nu)
        return bp[nu - 1];
    if (lo == 0)
        return bp[0];
    float d0 = fabsf(bp[lo - 1] - v);
    float d1 = fabsf(bp[lo] - v);
    return (d0 <= d1) ? bp[lo - 1] : bp[lo];
}

static void sort_active_edges(cov_edge **act, edge_sort_entry *entries, uint32_t na, float ym) {
    for (uint32_t i = 0; i < na; ++i) {
        entries[i].x = edge_x_at(act[i], ym);
        entries[i].edge = act[i];
    }
    if (na <= 32) {
        /* Insertion sort for small sets: cache-friendly and branch-predictable. */
        for (uint32_t i = 1; i < na; ++i) {
            edge_sort_entry key = entries[i];
            uint32_t j = i;
            while (j > 0 && entries[j - 1].x > key.x) {
                entries[j] = entries[j - 1];
                --j;
            }
            entries[j] = key;
        }
    } else {
        qsort(entries, na, sizeof(edge_sort_entry), cmp_sort_entry);
    }
    for (uint32_t i = 0; i < na; ++i)
        act[i] = entries[i].edge;
}

/* Exact area of the trapezoid between xl(s) and xr(s) — both linear in the BAND
 * parameter s in [0,1] — clipped to column [cl, cr), in s units; the caller
 * scales by the band's extent in t. */
static double span_in_column(float la, float lb, float ra, float rb, float cl, float cr) {
    float ts[6];
    int n = 0;
    ts[n++] = 0.0f;
    ts[n++] = 1.0f;
    const float tv[4] = {cl, cr, cl, cr};
    const float aa[4] = {la, la, ra, ra};
    const float bb[4] = {lb, lb, rb, rb};
    for (int k = 0; k < 4; ++k) {
        if (bb[k] != aa[k]) {
            float t = (tv[k] - aa[k]) / (bb[k] - aa[k]);
            if (t > 0.0f && t < 1.0f && n < 6)
                ts[n++] = t;
        }
    }
    for (int i = 0; i < n; ++i)
        for (int j = i + 1; j < n; ++j)
            if (ts[j] < ts[i]) {
                float t = ts[i];
                ts[i] = ts[j];
                ts[j] = t;
            }
    double area = 0.0;
    for (int k = 0; k + 1 < n; ++k) {
        float u0 = ts[k], u1 = ts[k + 1];
        if (!(u1 > u0))
            continue;
        float l0 = la + u0 * (lb - la), r0 = ra + u0 * (rb - ra);
        float l1 = la + u1 * (lb - la), r1 = ra + u1 * (rb - ra);
        float o0 = (r0 < cr ? r0 : cr) - (l0 > cl ? l0 : cl);
        float o1 = (r1 < cr ? r1 : cr) - (l1 > cl ? l1 : cl);
        if (o0 < 0.0f)
            o0 = 0.0f;
        if (o1 < 0.0f)
            o1 = 0.0f;
        area += 0.5 * ((double)o0 + (double)o1) * ((double)u1 - (double)u0);
    }
    return area;
}

/* Accumulate one span into one row over [t0, t1], returning the area added. */
static double accumulate(float *row, uint32_t w, float ox, float la, float lb, float ra, float rb,
                         float t0, float t1) {
    float minx = fminf(fminf(la, lb), fminf(ra, rb));
    float maxx = fmaxf(fmaxf(la, lb), fmaxf(ra, rb));
    if (!(maxx > minx))
        return 0.0;
    int i0 = (int)floorf(minx - ox) - 1;
    int i1 = (int)ceilf(maxx - ox) + 1;
    if (i0 < 0)
        i0 = 0;
    if (i1 > (int)w - 1)
        i1 = (int)w - 1;
    const double dt = (double)t1 - (double)t0;
    double total = 0.0;
    for (int i = i0; i <= i1; ++i) {
        float cl = ox + (float)i;
        double a = span_in_column(la, lb, ra, rb, cl, cl + 1.0f) * dt;
        if (a != 0.0) {
            row[i] += (float)a;
            total += a;
        }
    }
    return total;
}

flux_result canvas_coverage_rasterize(const flux_point *pts, const flux_canvas_contour *cons,
                                      uint32_t ncons, flux_fill_rule rule, float ox, float oy,
                                      uint32_t w, uint32_t h, float *out_cov) {
    if (!pts || !cons || !out_cov || w == 0 || h == 0)
        return FLUX_ERROR_INVALID_ARGUMENT;

    memset(out_cov, 0, (size_t)w * h * sizeof(float));

    /* Build the edge list. Horizontal edges carry no winding and no area. */
    uint32_t ne = 0, ne_cap = 0;
    cov_edge *edges = nullptr;
    for (uint32_t c = 0; c < ncons; ++c) {
        uint32_t n = cons[c].count;
        if (n < 2)
            continue;
        if (ne + n > ne_cap) {
            uint32_t ncap = (ne + n) * 2 + 64;
            cov_edge *grown = realloc(edges, (size_t)ncap * sizeof(cov_edge));
            if (!grown) {
                free(edges);
                return FLUX_ERROR_OUT_OF_MEMORY;
            }
            edges = grown;
            ne_cap = ncap;
        }
        for (uint32_t k = 0; k < n; ++k) {
            flux_point a = pts[cons[c].start + k];
            flux_point b = pts[cons[c].start + (k + 1) % n];
            if (a.y == b.y)
                continue;
            cov_edge *e = &edges[ne++];
            e->x0 = a.x;
            e->y0 = a.y;
            e->x1 = b.x;
            e->y1 = b.y;
            e->ytop = fminf(a.y, b.y);
            e->ybot = fmaxf(a.y, b.y);
            e->dxdy = (b.x - a.x) / (b.y - a.y);
            e->dir = b.y > a.y ? 1 : -1;
        }
    }
    if (ne == 0) {
        free(edges);
        return FLUX_OK;
    }

    /* Breakpoints: the grid's y extent, every row boundary, and every edge
     * endpoint inside it. */
    uint32_t bp_cap = 2 * ne + h + 8, nbp = 0;
    float *bp = malloc((size_t)bp_cap * sizeof(float));
    if (!bp) {
        free(edges);
        return FLUX_ERROR_OUT_OF_MEMORY;
    }
    const float ylo = oy, yhi = oy + (float)h;
    bp[nbp++] = ylo;
    bp[nbp++] = yhi;
    for (uint32_t j = 0; j <= h; ++j) {
        float y = oy + (float)j;
        if (y > ylo && y < yhi)
            bp[nbp++] = y;
    }
    for (uint32_t i = 0; i < ne; ++i) {
        if (edges[i].ytop > ylo && edges[i].ytop < yhi)
            bp[nbp++] = edges[i].ytop;
        if (edges[i].ybot > ylo && edges[i].ybot < yhi)
            bp[nbp++] = edges[i].ybot;
    }

    /* O(N log N) sorting for breakpoints. */
    qsort(bp, nbp, sizeof(float), cmp_float);

    uint32_t nu = 0;
    for (uint32_t a = 0; a < nbp; ++a)
        if (nu == 0 || bp[a] - bp[nu - 1] > 1e-6f)
            bp[nu++] = bp[a];

    /* Snap every edge endpoint onto a surviving breakpoint in O(ne log nu) time; see (2). */
    uint32_t valid_edges = 0;
    for (uint32_t i = 0; i < ne; ++i) {
        edges[i].ytop = snap_to_breakpoint(bp, nu, edges[i].ytop);
        edges[i].ybot = snap_to_breakpoint(bp, nu, edges[i].ybot);
        if (edges[i].ybot > edges[i].ytop)
            edges[valid_edges++] = edges[i];
    }
    ne = valid_edges;
    if (ne == 0) {
        free(edges);
        free(bp);
        return FLUX_OK;
    }

    /* Sort edges by ytop once into an edge table (ET). */
    qsort(edges, ne, sizeof(cov_edge), cmp_edge_ytop);

    cov_edge **act = malloc((size_t)ne * sizeof(cov_edge *));
    edge_sort_entry *entries = malloc((size_t)ne * sizeof(edge_sort_entry));
    if (!act || !entries) {
        free(edges);
        free(bp);
        free(act);
        free(entries);
        return FLUX_ERROR_OUT_OF_MEMORY;
    }

    flux_result rc = FLUX_OK;
    uint32_t splits = 0;
    uint32_t na = 0;
    uint32_t next_edge = 0;

    cov_subband stack[COV_SUBBAND_STACK_CAP];

    /* Sweep through breakpoints. Between bp[b] and bp[b+1], no edge starts or ends,
     * so every active edge spans the band completely. */
    for (uint32_t b = 0; b + 1 < nu && rc == FLUX_OK; ++b) {
        const float ya = bp[b], yb = bp[b + 1];
        if (!(yb > ya))
            continue;

        /* Retire edges that have ended at or before ya. */
        uint32_t live = 0;
        for (uint32_t i = 0; i < na; ++i) {
            if (act[i]->ybot > ya)
                act[live++] = act[i];
        }
        na = live;

        /* Insert new edges that start at ya. */
        while (next_edge < ne && edges[next_edge].ytop <= ya) {
            if (edges[next_edge].ybot > ya)
                act[na++] = &edges[next_edge];
            next_edge++;
        }

        if (na < 2)
            continue;

        int stack_top = 0;
        stack[stack_top++] = (cov_subband){ya, yb};

        while (stack_top > 0 && rc == FLUX_OK) {
            cov_subband sb = stack[--stack_top];
            const float s_ya = sb.ya, s_yb = sb.yb;
            if (!(s_yb > s_ya))
                continue;

            const float s_ym = 0.5f * (s_ya + s_yb);
            sort_active_edges(act, entries, na, s_ym);

            bool crossed = false;
            for (uint32_t i = 0; i + 1 < na; ++i) {
                if (edge_x_at(act[i], s_yb) > edge_x_at(act[i + 1], s_yb) + 1e-6f) {
                    crossed = true;
                    break;
                }
            }

            if (crossed) {
                if (++splits > COV_MAX_SPLITS) {
                    rc = FLUX_ERROR_OUT_OF_RANGE;
                    break;
                }
                if (!(s_ym > s_ya && s_ym < s_yb) || stack_top + 2 > COV_SUBBAND_STACK_CAP) {
                    rc = FLUX_ERROR_UNSUPPORTED;
                    break;
                }
                /* Push second half then first half to process top-to-bottom */
                stack[stack_top++] = (cov_subband){s_ym, s_yb};
                stack[stack_top++] = (cov_subband){s_ya, s_ym};
                continue;
            }

            /* No crossings: accumulate spans into overlapping rows. */
            int r0 = (int)floorf(s_ya - oy), r1 = (int)floorf(s_yb - oy);
            if (r0 < 0)
                r0 = 0;
            if (r1 > (int)h - 1)
                r1 = (int)h - 1;

            int winding = 0;
            cov_edge *left = nullptr;
            for (uint32_t i = 0; i < na; ++i) {
                winding += act[i]->dir;
                bool inside = (rule == FLUX_FILL_EVEN_ODD) ? ((winding & 1) != 0) : (winding != 0);
                if (inside && !left) {
                    left = act[i];
                } else if (!inside && left) {
                    for (int rr = r0; rr <= r1; ++rr) {
                        float rt = oy + (float)rr;
                        float ta = s_ya > rt ? s_ya : rt;
                        float tb = s_yb < rt + 1.0f ? s_yb : rt + 1.0f;
                        if (!(tb > ta))
                            continue;
                        accumulate(out_cov + (size_t)rr * w, w, ox, edge_x_at(left, s_ya),
                                   edge_x_at(left, s_yb), edge_x_at(act[i], s_ya),
                                   edge_x_at(act[i], s_yb), ta - rt, tb - rt);
                    }
                    left = nullptr;
                }
            }
        }
    }

    if (rc == FLUX_OK) {
        for (size_t k = 0; k < (size_t)w * h; ++k) {
            float c = out_cov[k];
            if (c < 0.0f)
                c = 0.0f;
            if (c > 1.0f)
                c = 1.0f;
            out_cov[k] = c;
        }
    }

    free(edges);
    free(bp);
    free(act);
    free(entries);
    return rc;
}
