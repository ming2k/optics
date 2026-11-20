/*
 * canvas_tile.h — Tile-compute 2D analytic coverage types & helpers (ADR-0120).
 */

#ifndef OPTICS_GFX_CANVAS_TILE_H
#define OPTICS_GFX_CANVAS_TILE_H

#include "internal.h"
#include <stddef.h>
#include <stdint.h>

#define OPTICS_GFX_TILE_SIZE 16u
#define OPTICS_GFX_TILE_BD_SCALE 65536

/* Flattened line segment in physical canvas pixels (16 bytes). */
typedef struct optics_gfx_tile_segment {
    float x0, y0;
    float x1, y1;
} optics_gfx_tile_segment;
static_assert(sizeof(optics_gfx_tile_segment) == 16, "flux_tile_segment must match GPU GpuSegment");

/* Header for GPU Segment SSBO upload (align to 16 bytes for BDA). */
typedef struct optics_gfx_tile_segment_header {
    uint32_t segment_count;
    uint32_t fill_rule; /* 0 = NonZero, 1 = EvenOdd */
    float bounds_min_x, bounds_min_y;
    float bounds_max_x, bounds_max_y;
    uint32_t pad[2];
} optics_gfx_tile_segment_header;

/* Metadata per 16x16 tile (16 bytes; matches GPU GpuTileHeader). */
typedef struct optics_gfx_tile_desc {
    uint32_t segment_offset;
    uint32_t segment_count;
    uint32_t cursor;
    uint32_t pad;
} optics_gfx_tile_desc;
static_assert(sizeof(optics_gfx_tile_desc) == 16, "flux_tile_desc must match GPU GpuTileHeader");

/* Per-draw GPU tile dispatch block read by compute bin/scan and vector fragment shaders. */
typedef struct optics_gfx_gpu_tile_dispatch {
    uint64_t segs;
    uint64_t tile_headers;
    uint64_t row_backdrops;
    uint64_t tile_indices;
    uint32_t seg_count;
    uint32_t max_indices;
    uint32_t tile_ox;
    uint32_t tile_oy;
    uint32_t tiles_x;
    uint32_t tiles_y;
    uint32_t fill_rule;
    uint32_t _pad0;
} optics_gfx_gpu_tile_dispatch;
static_assert(sizeof(optics_gfx_gpu_tile_dispatch) == 64,
              "flux_gpu_tile_dispatch must match GPU GpuTileDispatch");

/* Compute tile grid dimensions for a given pixel width and height. */
static inline void canvas_tile_grid_dims(uint32_t w, uint32_t h, uint32_t *out_tx,
                                         uint32_t *out_ty) {
    if (out_tx)
        *out_tx = (w + OPTICS_GFX_TILE_SIZE - 1u) / OPTICS_GFX_TILE_SIZE;
    if (out_ty)
        *out_ty = (h + OPTICS_GFX_TILE_SIZE - 1u) / OPTICS_GFX_TILE_SIZE;
}

/* Convert flattened contours into closed linear segments and compute tight bounds. */
uint32_t canvas_tile_extract_segments(const optics_gfx_point *pts,
                                      const optics_gfx_canvas_contour *cons, uint32_t ncons,
                                      optics_gfx_tile_segment *out_segs, uint32_t cap,
                                      optics_gfx_rect *out_bounds);

/* Upload segments into a GPU storage buffer.
 * Allocates or reuses *out_buf with usage STORAGE | TRANSFER_SRC | TRANSFER_DST. */
optics_gfx_result canvas_tile_upload_segments(optics_gfx_device *d,
                                              const optics_gfx_tile_segment *segs, uint32_t count,
                                              optics_gfx_fill_rule rule, optics_gfx_rect bounds,
                                              optics_gfx_buffer **out_buf);

/* Conservative upper bound on the number of (tile, segment) bin entries for a GPU dispatch. */
uint32_t canvas_tile_estimate_max_indices(const optics_gfx_tile_segment *segs, uint32_t seg_count,
                                          uint32_t tile_ox, uint32_t tile_oy, uint32_t tiles_x,
                                          uint32_t tiles_y);

/* Precise segment-to-tile intersection test. */
bool canvas_tile_segment_intersects(optics_gfx_tile_segment seg, float tile_x0, float tile_y0,
                                    float tile_x1, float tile_y1);

/* Coarse raster binning: partition segments into 16x16 tiles.
 * Computes per-tile segment counts, offsets, and a flattened list of segment indices. */
optics_gfx_result canvas_tile_bin_segments(const optics_gfx_tile_segment *segs, uint32_t seg_count,
                                           uint32_t width, uint32_t height,
                                           optics_gfx_tile_desc *out_tiles, uint32_t *out_indices,
                                           uint32_t indices_cap, uint32_t *out_total_indices);

/* Compute exact fixed-point (FLUX_TILE_BD_SCALE) per-pixel-row right-side backdrop
 * for every pixel row of every tile (`tiles_x * tiles_y * FLUX_TILE_SIZE` int32_t entries). */
optics_gfx_result canvas_tile_compute_backdrop(const optics_gfx_tile_segment *segs,
                                               uint32_t seg_count, uint32_t width, uint32_t height,
                                               int32_t *out_row_backdrops);

/* End-to-end CPU reference for the tiled rasterizer (binning + per-row backdrop + fine area). */
optics_gfx_result canvas_tile_rasterize(const optics_gfx_tile_segment *segs, uint32_t seg_count,
                                        optics_gfx_fill_rule rule, float ox, float oy, uint32_t w,
                                        uint32_t h, float *out_cov);

#endif /* FLUX_CANVAS_TILE_H */
