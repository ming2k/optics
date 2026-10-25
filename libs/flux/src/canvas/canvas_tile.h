/*
 * Tile-compute coverage backend (ADR-0120 Phase B).
 *
 * Consumes flattened contours and produces an analytic coverage plane
 * entirely on the GPU via compute shaders (no MSAA, no stencil attachment,
 * continuous 1x coverage).
 *
 * Phase B1: Segment extraction and SSBO storage buffer transport.
 * Phase B2: Coarse rasterization (16x16 tile binning).
 * Phase B3: Winding backdrop computation (two-level prefix sum).
 * Phase B4: Fine raster per-pixel area integration.
 */
#ifndef FLUX_CANVAS_TILE_H
#define FLUX_CANVAS_TILE_H

#include "internal.h"
#include <flux/core.h>

#define FLUX_TILE_SIZE 16u

/* A single 2D line segment in physical device space, consumable by GPU compute shaders. */
typedef struct flux_tile_segment {
    float x0, y0;
    float x1, y1;
} flux_tile_segment;

/* Header block for the segment SSBO, read by GPU coarse binning compute passes. */
typedef struct flux_tile_segment_header {
    uint32_t segment_count;
    uint32_t fill_rule; /* 0 = NonZero, 1 = EvenOdd */
    float bounds_min_x, bounds_min_y;
    float bounds_max_x, bounds_max_y;
    uint32_t pad[2];
} flux_tile_segment_header;

/* Metadata per 16x16 tile. */
typedef struct flux_tile_desc {
    uint32_t segment_offset;
    uint32_t segment_count;
    int32_t backdrop_winding; /* Phase B3: base winding entering from left */
    uint32_t pad;
} flux_tile_desc;

/* Compute tile grid dimensions for a given pixel width and height. */
static inline void canvas_tile_grid_dims(uint32_t w, uint32_t h, uint32_t *out_tx,
                                         uint32_t *out_ty) {
    if (out_tx)
        *out_tx = (w + FLUX_TILE_SIZE - 1u) / FLUX_TILE_SIZE;
    if (out_ty)
        *out_ty = (h + FLUX_TILE_SIZE - 1u) / FLUX_TILE_SIZE;
}

/* Extract flattened segments from CPU contour points into a host segment buffer.
 * Zero-length segments (coincident endpoints) are filtered out.
 * Returns the number of segments extracted. */
uint32_t canvas_tile_extract_segments(const flux_point *pts, const flux_canvas_contour *cons,
                                      uint32_t ncons, flux_tile_segment *out_segs, uint32_t cap,
                                      flux_rect *out_bounds);

/* Upload segments into a GPU storage buffer.
 * Allocates or reuses *out_buf with usage STORAGE | TRANSFER_SRC. */
flux_result canvas_tile_upload_segments(flux_device *d, const flux_tile_segment *segs,
                                        uint32_t count, flux_fill_rule rule, flux_rect bounds,
                                        flux_buffer **out_buf);

/* Precise segment-to-tile intersection test. */
bool canvas_tile_segment_intersects(flux_tile_segment seg, float tile_x0, float tile_y0,
                                    float tile_x1, float tile_y1);

/* Coarse raster binning: partition segments into 16x16 tiles.
 * Computes per-tile segment counts, offsets, and a flattened list of segment indices. */
flux_result canvas_tile_bin_segments(const flux_tile_segment *segs, uint32_t seg_count,
                                     uint32_t width, uint32_t height, flux_tile_desc *out_tiles,
                                     uint32_t *out_indices, uint32_t indices_cap,
                                     uint32_t *out_total_indices);

/* Compute winding backdrop for every tile via two-level prefix sum (Phase B3).
 * Fills out_tiles[...].backdrop_winding matching flux_curve_cover_winding on all interior tiles. */
flux_result canvas_tile_compute_backdrop(const flux_tile_segment *segs, uint32_t seg_count,
                                         uint32_t width, uint32_t height,
                                         flux_tile_desc *out_tiles);

/* Phase B4: End-to-end tile-compute rasterizer (coarse binning + backdrop + fine raster).
 * Evaluates exact area integration matching coverage_raster.c formulation. */
flux_result canvas_tile_rasterize(const flux_tile_segment *segs, uint32_t seg_count,
                                  flux_fill_rule rule, float ox, float oy, uint32_t w, uint32_t h,
                                  float *out_cov);

#endif /* FLUX_CANVAS_TILE_H */
