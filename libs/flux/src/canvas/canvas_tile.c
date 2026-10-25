/*
 * Tile-compute coverage backend (ADR-0120 Phase B1, B2, B3, B4).
 *
 * Implements segment extraction, SSBO upload, coarse binning,
 * two-level prefix sum winding backdrop, and fine raster area integration.
 */
#include "canvas_tile.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

uint32_t canvas_tile_extract_segments(const flux_point *pts, const flux_canvas_contour *cons,
                                      uint32_t ncons, flux_tile_segment *out_segs, uint32_t cap,
                                      flux_rect *out_bounds) {
    if (!pts || !cons || ncons == 0) {
        if (out_bounds)
            *out_bounds = (flux_rect){0, 0, 0, 0};
        return 0;
    }

    uint32_t total = 0;
    float min_x = 1e30f, min_y = 1e30f;
    float max_x = -1e30f, max_y = -1e30f;

    for (uint32_t c = 0; c < ncons; ++c) {
        uint32_t n = cons[c].count;
        if (n < 2)
            continue;
        uint32_t start = cons[c].start;
        for (uint32_t k = 0; k < n; ++k) {
            flux_point a = pts[start + k];
            flux_point b = pts[start + (k + 1) % n];

            /* Skip degenerate zero-length segments */
            if (a.x == b.x && a.y == b.y)
                continue;

            if (out_segs && total < cap) {
                out_segs[total] = (flux_tile_segment){
                    .x0 = a.x,
                    .y0 = a.y,
                    .x1 = b.x,
                    .y1 = b.y,
                };
            }
            total++;

            if (a.x < min_x)
                min_x = a.x;
            if (a.x > max_x)
                max_x = a.x;
            if (a.y < min_y)
                min_y = a.y;
            if (a.y > max_y)
                max_y = a.y;

            if (b.x < min_x)
                min_x = b.x;
            if (b.x > max_x)
                max_x = b.x;
            if (b.y < min_y)
                min_y = b.y;
            if (b.y > max_y)
                max_y = b.y;
        }
    }

    if (out_bounds) {
        if (total > 0) {
            out_bounds->x = min_x;
            out_bounds->y = min_y;
            out_bounds->w = max_x - min_x;
            out_bounds->h = max_y - min_y;
        } else {
            *out_bounds = (flux_rect){0, 0, 0, 0};
        }
    }

    return total;
}

flux_result canvas_tile_upload_segments(flux_device *d, const flux_tile_segment *segs,
                                        uint32_t count, flux_fill_rule rule, flux_rect bounds,
                                        flux_buffer **out_buf) {
    if (!d || !out_buf)
        return FLUX_ERROR_INVALID_ARGUMENT;

    size_t header_size = sizeof(flux_tile_segment_header);
    size_t payload_size = (size_t)count * sizeof(flux_tile_segment);
    size_t total_size = header_size + payload_size;

    /* Pad to 256 bytes for Vulkan uniform/storage alignment */
    total_size = (total_size + 255u) & ~((size_t)255u);

    uint8_t *staging = calloc(1, total_size);
    if (!staging)
        return FLUX_ERROR_OUT_OF_MEMORY;

    flux_tile_segment_header *hdr = (flux_tile_segment_header *)staging;
    hdr->segment_count = count;
    hdr->fill_rule = (rule == FLUX_FILL_EVEN_ODD) ? 1u : 0u;
    hdr->bounds_min_x = bounds.x;
    hdr->bounds_min_y = bounds.y;
    hdr->bounds_max_x = bounds.x + bounds.w;
    hdr->bounds_max_y = bounds.y + bounds.h;

    if (count > 0 && segs)
        memcpy(staging + header_size, segs, payload_size);

    flux_buffer_desc desc = {
        .type = FLUX_TYPE_BUFFER_DESC,
        .size = total_size,
        .usage = FLUX_BUFFER_USAGE_STORAGE | FLUX_BUFFER_USAGE_TRANSFER_SRC |
                 FLUX_BUFFER_USAGE_TRANSFER_DST,
        .location = FLUX_BUFFER_HOST_VISIBLE,
        .device_address = true,
        .initial_data = staging,
    };

    flux_buffer *buf = nullptr;
    flux_result rc = flux_buffer_create(d, &desc, &buf);
    free(staging);

    if (rc != FLUX_OK)
        return rc;

    if (*out_buf)
        flux_buffer_release(*out_buf);

    *out_buf = buf;
    return FLUX_OK;
}

bool canvas_tile_segment_intersects(flux_tile_segment seg, float tile_x0, float tile_y0,
                                    float tile_x1, float tile_y1) {
    float x0 = seg.x0, y0 = seg.y0;
    float x1 = seg.x1, y1 = seg.y1;

    /* 1. Fast AABB rejection with half-open boundaries: [tile_x0, tile_x1) x [tile_y0, tile_y1) */
    float s_min_x = fminf(x0, x1), s_max_x = fmaxf(x0, x1);
    float s_min_y = fminf(y0, y1), s_max_y = fmaxf(y0, y1);
    if (s_max_x < tile_x0 || s_min_x >= tile_x1 || s_max_y < tile_y0 || s_min_y >= tile_y1)
        return false;

    /* 2. Fast acceptance if either endpoint lies inside the half-open tile */
    if ((x0 >= tile_x0 && x0 < tile_x1 && y0 >= tile_y0 && y0 < tile_y1) ||
        (x1 >= tile_x0 && x1 < tile_x1 && y1 >= tile_y0 && y1 < tile_y1))
        return true;

    /* 3. Separating axis theorem: line L(x, y) = a*x + b*y + c = 0 */
    float a = y1 - y0;
    float b = x0 - x1;
    float c = x1 * y0 - x0 * y1;

    float f00 = a * tile_x0 + b * tile_y0 + c;
    float f10 = a * tile_x1 + b * tile_y0 + c;
    float f01 = a * tile_x0 + b * tile_y1 + c;
    float f11 = a * tile_x1 + b * tile_y1 + c;

    /* If all 4 corners are on the same strict side of the line, no intersection */
    if ((f00 > 0.0f && f10 > 0.0f && f01 > 0.0f && f11 > 0.0f) ||
        (f00 < 0.0f && f10 < 0.0f && f01 < 0.0f && f11 < 0.0f))
        return false;

    return true;
}

flux_result canvas_tile_bin_segments(const flux_tile_segment *segs, uint32_t seg_count,
                                     uint32_t width, uint32_t height, flux_tile_desc *out_tiles,
                                     uint32_t *out_indices, uint32_t indices_cap,
                                     uint32_t *out_total_indices) {
    if (width == 0 || height == 0 || !out_tiles)
        return FLUX_ERROR_INVALID_ARGUMENT;

    uint32_t tiles_x = 0, tiles_y = 0;
    canvas_tile_grid_dims(width, height, &tiles_x, &tiles_y);
    uint32_t total_tiles = tiles_x * tiles_y;

    memset(out_tiles, 0, (size_t)total_tiles * sizeof(flux_tile_desc));

    if (seg_count == 0 || !segs) {
        if (out_total_indices)
            *out_total_indices = 0;
        return FLUX_OK;
    }

    /* Pass 1: count intersecting segments per tile */
    for (uint32_t i = 0; i < seg_count; ++i) {
        flux_tile_segment s = segs[i];
        float s_min_x = fminf(s.x0, s.x1);
        float s_max_x = fmaxf(s.x0, s.x1);
        float s_min_y = fminf(s.y0, s.y1);
        float s_max_y = fmaxf(s.y0, s.y1);

        int tx0 = (int)floorf(s_min_x / (float)FLUX_TILE_SIZE);
        int tx1 = (int)floorf((s_max_x - 1e-6f) / (float)FLUX_TILE_SIZE);
        int ty0 = (int)floorf(s_min_y / (float)FLUX_TILE_SIZE);
        int ty1 = (int)floorf((s_max_y - 1e-6f) / (float)FLUX_TILE_SIZE);

        if (tx1 < tx0)
            tx1 = tx0;
        if (ty1 < ty0)
            ty1 = ty0;

        if (tx0 < 0)
            tx0 = 0;
        if (tx1 > (int)tiles_x - 1)
            tx1 = (int)tiles_x - 1;
        if (ty0 < 0)
            ty0 = 0;
        if (ty1 > (int)tiles_y - 1)
            ty1 = (int)tiles_y - 1;

        for (int ty = ty0; ty <= ty1; ++ty) {
            float tile_y0 = (float)(ty * (int)FLUX_TILE_SIZE);
            float tile_y1 = tile_y0 + (float)FLUX_TILE_SIZE;
            for (int tx = tx0; tx <= tx1; ++tx) {
                float tile_x0 = (float)(tx * (int)FLUX_TILE_SIZE);
                float tile_x1 = tile_x0 + (float)FLUX_TILE_SIZE;
                if (canvas_tile_segment_intersects(s, tile_x0, tile_y0, tile_x1, tile_y1)) {
                    uint32_t tidx = (uint32_t)ty * tiles_x + (uint32_t)tx;
                    out_tiles[tidx].segment_count++;
                }
            }
        }
    }

    /* Prefix sum to assign offsets */
    uint32_t running_offset = 0;
    for (uint32_t t = 0; t < total_tiles; ++t) {
        out_tiles[t].segment_offset = running_offset;
        running_offset += out_tiles[t].segment_count;
    }

    if (out_total_indices)
        *out_total_indices = running_offset;

    if (!out_indices || indices_cap < running_offset)
        return FLUX_OK; /* Caller only queried sizes */

    /* Pass 2: scatter segment indices into their designated tile slots */
    uint32_t *write_pos = calloc(total_tiles, sizeof(uint32_t));
    if (!write_pos)
        return FLUX_ERROR_OUT_OF_MEMORY;

    for (uint32_t i = 0; i < seg_count; ++i) {
        flux_tile_segment s = segs[i];
        float s_min_x = fminf(s.x0, s.x1);
        float s_max_x = fmaxf(s.x0, s.x1);
        float s_min_y = fminf(s.y0, s.y1);
        float s_max_y = fmaxf(s.y0, s.y1);

        int tx0 = (int)floorf(s_min_x / (float)FLUX_TILE_SIZE);
        int tx1 = (int)floorf((s_max_x - 1e-6f) / (float)FLUX_TILE_SIZE);
        int ty0 = (int)floorf(s_min_y / (float)FLUX_TILE_SIZE);
        int ty1 = (int)floorf((s_max_y - 1e-6f) / (float)FLUX_TILE_SIZE);

        if (tx1 < tx0)
            tx1 = tx0;
        if (ty1 < ty0)
            ty1 = ty0;

        if (tx0 < 0)
            tx0 = 0;
        if (tx1 > (int)tiles_x - 1)
            tx1 = (int)tiles_x - 1;
        if (ty0 < 0)
            ty0 = 0;
        if (ty1 > (int)tiles_y - 1)
            ty1 = (int)tiles_y - 1;

        for (int ty = ty0; ty <= ty1; ++ty) {
            float tile_y0 = (float)(ty * (int)FLUX_TILE_SIZE);
            float tile_y1 = tile_y0 + (float)FLUX_TILE_SIZE;
            for (int tx = tx0; tx <= tx1; ++tx) {
                float tile_x0 = (float)(tx * (int)FLUX_TILE_SIZE);
                float tile_x1 = tile_x0 + (float)FLUX_TILE_SIZE;
                if (canvas_tile_segment_intersects(s, tile_x0, tile_y0, tile_x1, tile_y1)) {
                    uint32_t tidx = (uint32_t)ty * tiles_x + (uint32_t)tx;
                    uint32_t dest = out_tiles[tidx].segment_offset + write_pos[tidx]++;
                    out_indices[dest] = i;
                }
            }
        }
    }

    free(write_pos);
    return FLUX_OK;
}

flux_result canvas_tile_compute_backdrop(const flux_tile_segment *segs, uint32_t seg_count,
                                         uint32_t width, uint32_t height,
                                         flux_tile_desc *out_tiles) {
    if (width == 0 || height == 0 || !out_tiles)
        return FLUX_ERROR_INVALID_ARGUMENT;

    uint32_t tiles_x = 0, tiles_y = 0;
    canvas_tile_grid_dims(width, height, &tiles_x, &tiles_y);

    int32_t *deltas = calloc(tiles_x, sizeof(int32_t));
    if (!deltas)
        return FLUX_ERROR_OUT_OF_MEMORY;

    /* Two-level prefix sum:
     * Level 1: In-tile delta accumulation for each tile row
     * Level 2: Reverse prefix scan (from right to left) propagating backdrop */
    for (uint32_t ty = 0; ty < tiles_y; ++ty) {
        memset(deltas, 0, (size_t)tiles_x * sizeof(int32_t));
        float yc = ((float)ty + 0.5f) * (float)FLUX_TILE_SIZE;
        int32_t overflow_delta = 0;

        for (uint32_t i = 0; i < seg_count; ++i) {
            flux_tile_segment s = segs[i];
            float ymin = fminf(s.y0, s.y1);
            float ymax = fmaxf(s.y0, s.y1);

            /* Half-open interval in Y: [ymin, ymax) intersects ray at yc */
            if (yc >= ymin && yc < ymax) {
                float t = (yc - s.y0) / (s.y1 - s.y0);
                float x_int = s.x0 + t * (s.x1 - s.x0);
                int32_t dir = (s.y1 > s.y0) ? 1 : -1;

                if (x_int < 0.0f)
                    continue;

                int tx = (int)floorf(x_int / (float)FLUX_TILE_SIZE);
                if (tx < (int)tiles_x) {
                    deltas[tx] += dir;
                } else {
                    overflow_delta += dir;
                }
            }
        }

        /* Level 2: Prefix scan from right to left */
        int32_t running_w = overflow_delta;
        for (int tx = (int)tiles_x - 1; tx >= 0; --tx) {
            uint32_t tidx = ty * tiles_x + (uint32_t)tx;
            running_w += deltas[tx];
            out_tiles[tidx].backdrop_winding = running_w;
        }
    }

    free(deltas);
    return FLUX_OK;
}

/* Sub-band exact area integration formulation matching coverage_raster.c */
static double tile_span_in_column(float la, float lb, float ra, float rb, float cl, float cr) {
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

static inline float tile_edge_x_at(float x0, float y0, float dxdy, float y) {
    return x0 + (y - y0) * dxdy;
}

static int cmp_float_val(const void *pa, const void *pb) {
    float a = *(const float *)pa;
    float b = *(const float *)pb;
    return (a > b) - (a < b);
}

flux_result canvas_tile_rasterize(const flux_tile_segment *segs, uint32_t seg_count,
                                  flux_fill_rule rule, float ox, float oy, uint32_t w, uint32_t h,
                                  float *out_cov) {
    if (w == 0 || h == 0 || !out_cov)
        return FLUX_ERROR_INVALID_ARGUMENT;

    memset(out_cov, 0, (size_t)w * h * sizeof(float));
    if (seg_count == 0 || !segs)
        return FLUX_OK;

    /* Transform segments into pixel grid coordinates [0, w) x [0, h) */
    typedef struct fine_edge {
        float x0, y0, x1, y1;
        float ytop, ybot;
        float dxdy;
        int dir;
    } fine_edge;

    fine_edge *edges = malloc((size_t)seg_count * sizeof(fine_edge));
    if (!edges)
        return FLUX_ERROR_OUT_OF_MEMORY;

    uint32_t ne = 0;
    for (uint32_t i = 0; i < seg_count; ++i) {
        float x0 = segs[i].x0 - ox;
        float y0 = segs[i].y0 - oy;
        float x1 = segs[i].x1 - ox;
        float y1 = segs[i].y1 - oy;
        if (y0 == y1)
            continue;
        edges[ne].x0 = x0;
        edges[ne].y0 = y0;
        edges[ne].x1 = x1;
        edges[ne].y1 = y1;
        edges[ne].ytop = fminf(y0, y1);
        edges[ne].ybot = fmaxf(y0, y1);
        edges[ne].dxdy = (x1 - x0) / (y1 - y0);
        edges[ne].dir = (y1 > y0) ? 1 : -1;
        ne++;
    }

    if (ne == 0) {
        free(edges);
        return FLUX_OK;
    }

    /* Collect breakpoints */
    uint32_t bp_cap = 2 * ne + h + 8, nbp = 0;
    float *bp = malloc((size_t)bp_cap * sizeof(float));
    fine_edge **act = malloc((size_t)ne * sizeof(fine_edge *));
    if (!bp || !act) {
        free(edges);
        free(bp);
        free(act);
        return FLUX_ERROR_OUT_OF_MEMORY;
    }

    bp[nbp++] = 0.0f;
    bp[nbp++] = (float)h;
    for (uint32_t j = 0; j <= h; ++j) {
        float y = (float)j;
        if (y > 0.0f && y < (float)h)
            bp[nbp++] = y;
    }
    for (uint32_t i = 0; i < ne; ++i) {
        if (edges[i].ytop > 0.0f && edges[i].ytop < (float)h)
            bp[nbp++] = edges[i].ytop;
        if (edges[i].ybot > 0.0f && edges[i].ybot < (float)h)
            bp[nbp++] = edges[i].ybot;
    }

    qsort(bp, nbp, sizeof(float), cmp_float_val);

    uint32_t nu = 0;
    for (uint32_t a = 0; a < nbp; ++a)
        if (nu == 0 || bp[a] - bp[nu - 1] > 1e-6f)
            bp[nu++] = bp[a];

    /* Snap edge endpoints */
    for (uint32_t i = 0; i < ne; ++i) {
        for (int pass = 0; pass < 2; ++pass) {
            float v = (pass == 0) ? edges[i].ytop : edges[i].ybot;
            uint32_t lo = 0, hi = nu;
            while (lo < hi) {
                uint32_t mid = lo + (hi - lo) / 2;
                if (bp[mid] < v)
                    lo = mid + 1;
                else
                    hi = mid;
            }
            float best = bp[0];
            if (lo >= nu)
                best = bp[nu - 1];
            else if (lo == 0)
                best = bp[0];
            else {
                float d0 = fabsf(bp[lo - 1] - v);
                float d1 = fabsf(bp[lo] - v);
                best = (d0 <= d1) ? bp[lo - 1] : bp[lo];
            }
            if (pass == 0)
                edges[i].ytop = best;
            else
                edges[i].ybot = best;
        }
    }

    /* Sub-band exact integration */
    for (uint32_t b = 0; b + 1 < nu; ++b) {
        float ya = bp[b], yb = bp[b + 1];
        if (!(yb > ya))
            continue;

        int r0 = (int)floorf(ya), r1 = (int)floorf(yb);
        if (r0 < 0)
            r0 = 0;
        if (r1 > (int)h - 1)
            r1 = (int)h - 1;

        uint32_t na = 0;
        for (uint32_t i = 0; i < ne; ++i) {
            if (edges[i].ybot > ya && edges[i].ytop < yb)
                act[na++] = &edges[i];
        }
        if (na < 2)
            continue;

        float ym = 0.5f * (ya + yb);
        for (uint32_t i = 0; i < na; ++i) {
            for (uint32_t j = i + 1; j < na; ++j) {
                float xi = tile_edge_x_at(act[i]->x0, act[i]->y0, act[i]->dxdy, ym);
                float xj = tile_edge_x_at(act[j]->x0, act[j]->y0, act[j]->dxdy, ym);
                if (xj < xi) {
                    fine_edge *t = act[i];
                    act[i] = act[j];
                    act[j] = t;
                }
            }
        }

        int winding = 0;
        fine_edge *left = nullptr;
        for (uint32_t i = 0; i < na; ++i) {
            winding += act[i]->dir;
            bool inside = (rule == FLUX_FILL_EVEN_ODD) ? ((winding & 1) != 0) : (winding != 0);
            if (inside && !left) {
                left = act[i];
            } else if (!inside && left) {
                for (int rr = r0; rr <= r1; ++rr) {
                    float rt = (float)rr;
                    float ta = (ya > rt) ? ya : rt;
                    float tb = (yb < rt + 1.0f) ? yb : rt + 1.0f;
                    if (!(tb > ta))
                        continue;

                    float la = tile_edge_x_at(left->x0, left->y0, left->dxdy, ya);
                    float lb = tile_edge_x_at(left->x0, left->y0, left->dxdy, yb);
                    float ra = tile_edge_x_at(act[i]->x0, act[i]->y0, act[i]->dxdy, ya);
                    float rb = tile_edge_x_at(act[i]->x0, act[i]->y0, act[i]->dxdy, yb);

                    float minx = fminf(fminf(la, lb), fminf(ra, rb));
                    float maxx = fmaxf(fmaxf(la, lb), fmaxf(ra, rb));
                    if (!(maxx > minx))
                        continue;

                    int c0 = (int)floorf(minx) - 1;
                    int c1 = (int)ceilf(maxx) + 1;
                    if (c0 < 0)
                        c0 = 0;
                    if (c1 > (int)w - 1)
                        c1 = (int)w - 1;

                    double dt = (double)tb - (double)ta;
                    for (int c = c0; c <= c1; ++c) {
                        float cl = (float)c;
                        double a = tile_span_in_column(la, lb, ra, rb, cl, cl + 1.0f) * dt;
                        if (a != 0.0)
                            out_cov[(size_t)rr * w + c] += (float)a;
                    }
                }
                left = nullptr;
            }
        }
    }

    for (size_t k = 0; k < (size_t)w * h; ++k) {
        float c = out_cov[k];
        if (c < 0.0f)
            c = 0.0f;
        if (c > 1.0f)
            c = 1.0f;
        out_cov[k] = c;
    }

    free(edges);
    free(bp);
    free(act);
    return FLUX_OK;
}
