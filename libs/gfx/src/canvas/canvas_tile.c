/*
 * canvas_tile.c — Tile-compute 2D analytic coverage infrastructure (ADR-0120).
 *
 * Implements GPU segment upload, Y-clipped tile binning, exact per-pixel-row
 * right-to-left backdrop prefix scan, and a tiled CPU reference matching
 * flux_vector_eval.slang bit-for-bit in math.
 */

#include "canvas_tile.h"

#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

static float clamp01(float x) {
    return x < 0.0f ? 0.0f : (x > 1.0f ? 1.0f : x);
}

static float clampf_range(float x, float lo, float hi) {
    return x < lo ? lo : (x > hi ? hi : x);
}

static float clamp01_integral(float u) {
    return (u <= 0.0f) ? 0.0f : ((u >= 1.0f) ? u - 0.5f : 0.5f * u * u);
}

static void vector_area_segment_cpu(float px, float py, float ax, float ay, float bx, float by,
                                    float *area) {
    float y0 = ay - py;
    float y1 = by - py;
    if (y0 == y1)
        return;
    float t0 = clamp01((0.0f - y0) / (y1 - y0));
    float t1 = clamp01((1.0f - y0) / (y1 - y0));
    float height = (t1 - t0) * (y1 - y0);
    if (height <= 0.0f)
        return;
    float dy = (y1 > y0) ? height : -height;
    float x0 = ax - px;
    float x1 = bx - px;
    float u0 = x0 + (x1 - x0) * t0;
    float u1 = x0 + (x1 - x0) * t1;
    float lo = u0 < u1 ? u0 : u1;
    float hi = u0 > u1 ? u0 : u1;
    float mean;
    if (lo >= 1.0f) {
        mean = 1.0f;
    } else if (hi <= 0.0f) {
        mean = 0.0f;
    } else if (hi - lo < 1e-4f) {
        mean = clamp01(0.5f * (lo + hi));
    } else {
        mean = (clamp01_integral(hi) - clamp01_integral(lo)) / (hi - lo);
    }
    *area += dy * mean;
}

static float vector_area_coverage_cpu(float area, optics_gfx_fill_rule rule) {
    float c = fabsf(area);
    if (rule == OPTICS_GFX_FILL_EVEN_ODD) {
        c = c - 2.0f * floorf(c * 0.5f);
        if (c > 1.0f)
            c = 2.0f - c;
    }
    return clamp01(c);
}

/* Clip `s` in Y against [tile_y0, tile_y1) and return its horizontal [xmin, xmax] span.
 * Horizontal segments return true on [tile_y0, tile_y1] for bbox overlap queries. */
static bool seg_y_band_x_span(optics_gfx_tile_segment s, float tile_y0, float tile_y1,
                              float *out_xmin, float *out_xmax) {
    if (!isfinite(s.x0) || !isfinite(s.y0) || !isfinite(s.x1) || !isfinite(s.y1))
        return false;
    float sy_min = fminf(s.y0, s.y1);
    float sy_max = fmaxf(s.y0, s.y1);
    float dy = s.y1 - s.y0;
    if (dy == 0.0f) {
        if (sy_min < tile_y0 || sy_min > tile_y1)
            return false;
        *out_xmin = fminf(s.x0, s.x1);
        *out_xmax = fmaxf(s.x0, s.x1);
        return true;
    }
    if (sy_max <= tile_y0 || sy_min >= tile_y1)
        return false;
    float ta = (tile_y0 - s.y0) / dy;
    float tb = (tile_y1 - s.y0) / dy;
    float t0 = clamp01(fminf(ta, tb));
    float t1 = clamp01(fmaxf(ta, tb));
    if (t1 <= t0)
        return false;
    float xa = s.x0 + (s.x1 - s.x0) * t0;
    float xb = s.x0 + (s.x1 - s.x0) * t1;
    *out_xmin = fminf(xa, xb);
    *out_xmax = fmaxf(xa, xb);
    return true;
}

uint32_t canvas_tile_extract_segments(const optics_gfx_point *pts,
                                      const optics_gfx_canvas_contour *cons, uint32_t ncons,
                                      optics_gfx_tile_segment *out_segs, uint32_t cap,
                                      optics_gfx_rect *out_bounds) {
    if (!pts || !cons || ncons == 0) {
        if (out_bounds)
            *out_bounds = (optics_gfx_rect){0, 0, 0, 0};
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
            optics_gfx_point a = pts[start + k];
            optics_gfx_point b = pts[start + (k + 1) % n];

            /* Skip degenerate zero-length segments */
            if (a.x == b.x && a.y == b.y)
                continue;

            if (out_segs && total < cap) {
                out_segs[total] = (optics_gfx_tile_segment){
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
        if (total == 0) {
            *out_bounds = (optics_gfx_rect){0, 0, 0, 0};
        } else {
            *out_bounds = (optics_gfx_rect){
                .x = min_x,
                .y = min_y,
                .w = max_x - min_x,
                .h = max_y - min_y,
            };
        }
    }
    return total;
}

optics_gfx_result canvas_tile_upload_segments(optics_gfx_device *d,
                                              const optics_gfx_tile_segment *segs, uint32_t count,
                                              optics_gfx_fill_rule rule, optics_gfx_rect bounds,
                                              optics_gfx_buffer **out_buf) {
    if (!d || !out_buf)
        return OPTICS_GFX_ERROR_INVALID_ARGUMENT;

    size_t header_size = sizeof(optics_gfx_tile_segment_header);
    size_t payload_size = (size_t)count * sizeof(optics_gfx_tile_segment);
    size_t total_size = header_size + payload_size;

    /* Pad to 256 bytes for Vulkan uniform/storage alignment */
    total_size = (total_size + 255u) & ~((size_t)255u);

    uint8_t *staging = calloc(1, total_size);
    if (!staging)
        return OPTICS_GFX_ERROR_OUT_OF_MEMORY;

    optics_gfx_tile_segment_header *hdr = (optics_gfx_tile_segment_header *)staging;
    hdr->segment_count = count;
    hdr->fill_rule = (rule == OPTICS_GFX_FILL_EVEN_ODD) ? 1u : 0u;
    hdr->bounds_min_x = bounds.x;
    hdr->bounds_min_y = bounds.y;
    hdr->bounds_max_x = bounds.x + bounds.w;
    hdr->bounds_max_y = bounds.y + bounds.h;

    if (count > 0 && segs)
        memcpy(staging + header_size, segs, payload_size);

    optics_gfx_buffer_desc desc = {
        .type = OPTICS_GFX_TYPE_BUFFER_DESC,
        .size = total_size,
        .usage = OPTICS_GFX_BUFFER_USAGE_STORAGE | OPTICS_GFX_BUFFER_USAGE_TRANSFER_SRC |
                 OPTICS_GFX_BUFFER_USAGE_TRANSFER_DST,
        .location = OPTICS_GFX_BUFFER_HOST_VISIBLE,
        .device_address = true,
        .initial_data = staging,
    };

    optics_gfx_buffer *buf = nullptr;
    optics_gfx_result rc = optics_gfx_buffer_create(d, &desc, &buf);
    free(staging);

    if (rc != OPTICS_GFX_OK)
        return rc;

    if (*out_buf)
        optics_gfx_buffer_release(*out_buf);

    *out_buf = buf;
    return OPTICS_GFX_OK;
}

uint32_t canvas_tile_estimate_max_indices(const optics_gfx_tile_segment *segs, uint32_t seg_count,
                                          uint32_t tile_ox, uint32_t tile_oy, uint32_t tiles_x,
                                          uint32_t tiles_y) {
    if (!segs || seg_count == 0 || tiles_x == 0 || tiles_y == 0)
        return 0;

    uint64_t total = 0;
    const float gy0 = (float)(tile_oy * OPTICS_GFX_TILE_SIZE);
    const float gy1 = (float)((tile_oy + tiles_y) * OPTICS_GFX_TILE_SIZE);
    const float gx0 = (float)(tile_ox * OPTICS_GFX_TILE_SIZE);
    const float gx1 = (float)((tile_ox + tiles_x) * OPTICS_GFX_TILE_SIZE);

    for (uint32_t i = 0; i < seg_count; ++i) {
        optics_gfx_tile_segment s = segs[i];
        float dy = s.y1 - s.y0;
        if (!isfinite(dy) || dy == 0.0f || !isfinite(s.x0) || !isfinite(s.x1))
            continue;
        float sy_min = fmaxf(fminf(s.y0, s.y1), gy0);
        float sy_max = fminf(fmaxf(s.y0, s.y1), gy1);
        if (sy_max <= sy_min)
            continue;
        int ty0 = (int)floorf(sy_min / 16.0f) - (int)tile_oy;
        int ty1 = (int)floorf(sy_max / 16.0f) - (int)tile_oy;
        if (ty0 < 0)
            ty0 = 0;
        if (ty1 >= (int)tiles_y)
            ty1 = (int)tiles_y - 1;
        if (ty0 > ty1)
            continue;
        uint64_t rows = (uint64_t)(ty1 - ty0 + 1);
        float dx =
            clampf_range(fmaxf(s.x0, s.x1), gx0, gx1) - clampf_range(fminf(s.x0, s.x1), gx0, gx1);
        uint64_t x_span = (uint64_t)ceilf(dx / 16.0f);
        uint64_t est = x_span + 2u * rows + 2u;
        uint64_t cap = (uint64_t)tiles_x * rows;
        total += est < cap ? est : cap;
    }
    if (total > UINT32_MAX - 16u)
        return UINT32_MAX;
    return (uint32_t)total;
}

bool canvas_tile_segment_intersects(optics_gfx_tile_segment seg, float tile_x0, float tile_y0,
                                    float tile_x1, float tile_y1) {
    float xmin = 0.0f, xmax = 0.0f;
    if (!seg_y_band_x_span(seg, tile_y0, tile_y1, &xmin, &xmax))
        return false;
    const float eps = 1e-3f;
    return (xmax + eps >= tile_x0) && (xmin - eps < tile_x1);
}

optics_gfx_result canvas_tile_bin_segments(const optics_gfx_tile_segment *segs, uint32_t seg_count,
                                           uint32_t width, uint32_t height,
                                           optics_gfx_tile_desc *out_tiles, uint32_t *out_indices,
                                           uint32_t indices_cap, uint32_t *out_total_indices) {
    if (!out_tiles || width == 0 || height == 0)
        return OPTICS_GFX_ERROR_INVALID_ARGUMENT;

    uint32_t tx_cnt = 0, ty_cnt = 0;
    canvas_tile_grid_dims(width, height, &tx_cnt, &ty_cnt);
    uint32_t num_tiles = tx_cnt * ty_cnt;

    memset(out_tiles, 0, (size_t)num_tiles * sizeof(optics_gfx_tile_desc));

    const float eps = 1e-3f;
    uint32_t total = 0;

    for (uint32_t i = 0; i < seg_count; ++i) {
        optics_gfx_tile_segment s = segs[i];
        float sy_min = fminf(s.y0, s.y1);
        float sy_max = fmaxf(s.y0, s.y1);
        if (!isfinite(sy_min) || !isfinite(sy_max))
            continue;
        int ty0 = (int)floorf((sy_min - eps) / 16.0f);
        int ty1 = (int)floorf((sy_max + eps) / 16.0f);
        if (ty0 < 0)
            ty0 = 0;
        if (ty1 >= (int)ty_cnt)
            ty1 = (int)ty_cnt - 1;
        for (int ty = ty0; ty <= ty1; ++ty) {
            float y0 = (float)(ty * (int)OPTICS_GFX_TILE_SIZE);
            float y1 = y0 + (float)OPTICS_GFX_TILE_SIZE;
            float xmin = 0.0f, xmax = 0.0f;
            if (!seg_y_band_x_span(s, y0, y1, &xmin, &xmax))
                continue;
            int lx0 = (int)floorf((xmin - eps) / 16.0f);
            int lx1 = (int)floorf((xmax + eps) / 16.0f);
            if (lx0 < 0)
                lx0 = 0;
            if (lx1 >= (int)tx_cnt)
                lx1 = (int)tx_cnt - 1;
            for (int tx = lx0; tx <= lx1; ++tx) {
                uint32_t tidx = (uint32_t)ty * tx_cnt + (uint32_t)tx;
                out_tiles[tidx].segment_count++;
                total++;
            }
        }
    }

    uint32_t cursor = 0;
    for (uint32_t t = 0; t < num_tiles; ++t) {
        out_tiles[t].segment_offset = cursor;
        cursor += out_tiles[t].segment_count;
        out_tiles[t].cursor = 0;
    }

    if (out_total_indices)
        *out_total_indices = total;

    if (!out_indices)
        return OPTICS_GFX_OK;

    if (indices_cap < total)
        return OPTICS_GFX_ERROR_OUT_OF_RANGE;

    for (uint32_t i = 0; i < seg_count; ++i) {
        optics_gfx_tile_segment s = segs[i];
        float sy_min = fminf(s.y0, s.y1);
        float sy_max = fmaxf(s.y0, s.y1);
        if (!isfinite(sy_min) || !isfinite(sy_max))
            continue;
        int ty0 = (int)floorf((sy_min - eps) / 16.0f);
        int ty1 = (int)floorf((sy_max + eps) / 16.0f);
        if (ty0 < 0)
            ty0 = 0;
        if (ty1 >= (int)ty_cnt)
            ty1 = (int)ty_cnt - 1;
        for (int ty = ty0; ty <= ty1; ++ty) {
            float y0 = (float)(ty * (int)OPTICS_GFX_TILE_SIZE);
            float y1 = y0 + (float)OPTICS_GFX_TILE_SIZE;
            float xmin = 0.0f, xmax = 0.0f;
            if (!seg_y_band_x_span(s, y0, y1, &xmin, &xmax))
                continue;
            int lx0 = (int)floorf((xmin - eps) / 16.0f);
            int lx1 = (int)floorf((xmax + eps) / 16.0f);
            if (lx0 < 0)
                lx0 = 0;
            if (lx1 >= (int)tx_cnt)
                lx1 = (int)tx_cnt - 1;
            for (int tx = lx0; tx <= lx1; ++tx) {
                uint32_t tidx = (uint32_t)ty * tx_cnt + (uint32_t)tx;
                uint32_t dst = out_tiles[tidx].segment_offset + out_tiles[tidx].cursor++;
                out_indices[dst] = i;
            }
        }
    }

    return OPTICS_GFX_OK;
}

optics_gfx_result canvas_tile_compute_backdrop(const optics_gfx_tile_segment *segs,
                                               uint32_t seg_count, uint32_t width, uint32_t height,
                                               int32_t *out_row_backdrops) {
    if (!out_row_backdrops || width == 0 || height == 0)
        return OPTICS_GFX_ERROR_INVALID_ARGUMENT;

    uint32_t tx_cnt = 0, ty_cnt = 0;
    canvas_tile_grid_dims(width, height, &tx_cnt, &ty_cnt);
    memset(out_row_backdrops, 0, (size_t)tx_cnt * ty_cnt * OPTICS_GFX_TILE_SIZE * sizeof(int32_t));

    const float eps = 1e-3f;
    for (uint32_t i = 0; i < seg_count; ++i) {
        optics_gfx_tile_segment s = segs[i];
        float dy = s.y1 - s.y0;
        if (!isfinite(dy) || dy == 0.0f || !isfinite(s.x0) || !isfinite(s.x1))
            continue;
        float sy_min = fminf(s.y0, s.y1);
        float sy_max = fmaxf(s.y0, s.y1);
        int ty0 = (int)floorf((sy_min - eps) / 16.0f);
        int ty1 = (int)floorf((sy_max + eps) / 16.0f);
        if (ty0 < 0)
            ty0 = 0;
        if (ty1 >= (int)ty_cnt)
            ty1 = (int)ty_cnt - 1;
        for (int ty = ty0; ty <= ty1; ++ty) {
            float y0 = (float)(ty * (int)OPTICS_GFX_TILE_SIZE);
            float y1 = y0 + (float)OPTICS_GFX_TILE_SIZE;
            float xmin = 0.0f, xmax = 0.0f;
            if (!seg_y_band_x_span(s, y0, y1, &xmin, &xmax))
                continue;
            int lx0 = (int)floorf((xmin - eps) / 16.0f);
            if (lx0 <= 0)
                continue;
            if (lx0 > (int)tx_cnt)
                lx0 = (int)tx_cnt;
            uint32_t bd_tile = (uint32_t)ty * tx_cnt + (uint32_t)(lx0 - 1);
            int py_base = ty * (int)OPTICS_GFX_TILE_SIZE;
            int ly0 = (int)floorf(sy_min) - py_base;
            int ly1 = (int)floorf(sy_max) - py_base;
            if (ly0 < 0)
                ly0 = 0;
            if (ly1 > 15)
                ly1 = 15;
            for (int ly = ly0; ly <= ly1; ++ly) {
                float py = (float)(py_base + ly);
                int32_t fp0 =
                    (int32_t)llroundf(clamp01(s.y0 - py) * (float)OPTICS_GFX_TILE_BD_SCALE);
                int32_t fp1 =
                    (int32_t)llroundf(clamp01(s.y1 - py) * (float)OPTICS_GFX_TILE_BD_SCALE);
                out_row_backdrops[bd_tile * OPTICS_GFX_TILE_SIZE + (uint32_t)ly] += (fp1 - fp0);
            }
        }
    }

    for (uint32_t ty = 0; ty < ty_cnt; ++ty) {
        for (uint32_t ly = 0; ly < OPTICS_GFX_TILE_SIZE; ++ly) {
            int32_t acc = 0;
            for (int tx = (int)tx_cnt - 1; tx >= 0; --tx) {
                uint32_t idx = (ty * tx_cnt + (uint32_t)tx) * OPTICS_GFX_TILE_SIZE + ly;
                acc += out_row_backdrops[idx];
                out_row_backdrops[idx] = acc;
            }
        }
    }

    return OPTICS_GFX_OK;
}

optics_gfx_result canvas_tile_rasterize(const optics_gfx_tile_segment *segs, uint32_t seg_count,
                                        optics_gfx_fill_rule rule, float ox, float oy, uint32_t w,
                                        uint32_t h, float *out_cov) {
    if (!out_cov || w == 0 || h == 0)
        return OPTICS_GFX_ERROR_INVALID_ARGUMENT;
    memset(out_cov, 0, (size_t)w * h * sizeof(float));
    if (!segs || seg_count == 0)
        return OPTICS_GFX_OK;

    optics_gfx_tile_segment *local_segs =
        malloc((size_t)seg_count * sizeof(optics_gfx_tile_segment));
    if (!local_segs)
        return OPTICS_GFX_ERROR_OUT_OF_MEMORY;
    for (uint32_t i = 0; i < seg_count; ++i) {
        local_segs[i] = (optics_gfx_tile_segment){
            .x0 = segs[i].x0 - ox,
            .y0 = segs[i].y0 - oy,
            .x1 = segs[i].x1 - ox,
            .y1 = segs[i].y1 - oy,
        };
    }

    uint32_t tx_cnt = 0, ty_cnt = 0;
    canvas_tile_grid_dims(w, h, &tx_cnt, &ty_cnt);
    uint32_t num_tiles = tx_cnt * ty_cnt;

    optics_gfx_tile_desc *tiles = calloc(num_tiles, sizeof(optics_gfx_tile_desc));
    int32_t *row_bd = calloc((size_t)num_tiles * OPTICS_GFX_TILE_SIZE, sizeof(int32_t));
    uint32_t max_indices =
        canvas_tile_estimate_max_indices(local_segs, seg_count, 0u, 0u, tx_cnt, ty_cnt);
    uint32_t *indices = malloc(((size_t)max_indices + 1u) * sizeof(uint32_t));
    if (!tiles || !row_bd || !indices) {
        free(local_segs);
        free(tiles);
        free(row_bd);
        free(indices);
        return OPTICS_GFX_ERROR_OUT_OF_MEMORY;
    }

    uint32_t total_indices = 0;
    optics_gfx_result r = canvas_tile_bin_segments(local_segs, seg_count, w, h, tiles, indices,
                                                   max_indices + 1u, &total_indices);
    if (r == OPTICS_GFX_OK)
        r = canvas_tile_compute_backdrop(local_segs, seg_count, w, h, row_bd);

    if (r == OPTICS_GFX_OK) {
        for (uint32_t ty = 0; ty < ty_cnt; ++ty) {
            for (uint32_t tx = 0; tx < tx_cnt; ++tx) {
                uint32_t tidx = ty * tx_cnt + tx;
                const optics_gfx_tile_desc *td = &tiles[tidx];
                uint32_t py0 = ty * OPTICS_GFX_TILE_SIZE;
                uint32_t px0 = tx * OPTICS_GFX_TILE_SIZE;
                uint32_t ly_end =
                    (py0 + OPTICS_GFX_TILE_SIZE <= h) ? OPTICS_GFX_TILE_SIZE : (h - py0);
                uint32_t lx_end =
                    (px0 + OPTICS_GFX_TILE_SIZE <= w) ? OPTICS_GFX_TILE_SIZE : (w - px0);
                for (uint32_t ly = 0; ly < ly_end; ++ly) {
                    float bd_area = (float)row_bd[tidx * OPTICS_GFX_TILE_SIZE + ly] /
                                    (float)OPTICS_GFX_TILE_BD_SCALE;
                    uint32_t py = py0 + ly;
                    for (uint32_t lx = 0; lx < lx_end; ++lx) {
                        uint32_t px = px0 + lx;
                        float area = bd_area;
                        for (uint32_t k = 0; k < td->segment_count; ++k) {
                            optics_gfx_tile_segment s = local_segs[indices[td->segment_offset + k]];
                            vector_area_segment_cpu((float)px, (float)py, s.x0, s.y0, s.x1, s.y1,
                                                    &area);
                        }
                        out_cov[(size_t)py * w + px] = vector_area_coverage_cpu(area, rule);
                    }
                }
            }
        }
    }

    free(local_segs);
    free(tiles);
    free(row_bd);
    free(indices);
    return r;
}
