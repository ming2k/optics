/*
 * canvas_tile_common.slang — shared GPU types and binning math for the
 * 16x16 tiled analytic vector rasterizer (ADR-0116 / ADR-0120).
 */

#ifndef CANVAS_TILE_COMMON_SLANG
#define CANVAS_TILE_COMMON_SLANG

#include "optics_gfx_vector_eval.slang"

#define OPTICS_GFX_TILE_SIZE     16u
#define OPTICS_GFX_TILE_BD_SCALE 65536.0f

struct GpuSegment {
    float x0, y0;
    float x1, y1;
};

struct GpuTileHeader {
    uint32_t offset;
    uint32_t count;
    uint32_t cursor;
    uint32_t _pad;
};

struct GpuTileDispatch {
    GpuSegment*    segs;
    GpuTileHeader* tile_headers;
    int32_t*       row_backdrops;
    uint32_t*      tile_indices;
    uint32_t       seg_count;
    uint32_t       max_indices;
    uint32_t       tile_ox;
    uint32_t       tile_oy;
    uint32_t       tiles_x;
    uint32_t       tiles_y;
    uint32_t       fill_rule;
    uint32_t       _pad0;
};

/* Compute the horizontal tile span [lx0, lx1] touched by segment `s` within
 * tile row `ty` (whose Y bounds are [Y0, Y1)), clamped to the local grid
 * [0, tiles_x). `lx0 > 0` implies every tile left of `lx0` receives `s`'s
 * right-side per-row winding backdrop via `lx0 - 1`. */
void tile_seg_row_span(GpuSegment s, int ty, uint32_t tile_ox, uint32_t tile_oy,
                       uint32_t tiles_x, out int lx0, out int lx1) {
    float Y0 = float((int(tile_oy) + ty) * 16);
    float Y1 = Y0 + 16.0f;
    float dy = s.y1 - s.y0;
    float ta = (Y0 - s.y0) / dy;
    float tb = (Y1 - s.y0) / dy;
    float t0 = clamp(min(ta, tb), 0.0f, 1.0f);
    float t1 = clamp(max(ta, tb), 0.0f, 1.0f);
    if (t1 <= t0) {
        lx0 = 0;
        lx1 = -1;
        return;
    }
    float xa = lerp(s.x0, s.x1, t0);
    float xb = lerp(s.x0, s.x1, t1);
    float xmin = min(xa, xb);
    float xmax = max(xa, xb);

    const float eps = 1e-3f;
    int gx0 = int(floor((xmin - eps) / 16.0f));
    int gx1 = int(floor((xmax + eps) / 16.0f));
    lx0 = clamp(gx0 - int(tile_ox), 0, int(tiles_x));
    lx1 = clamp(gx1 - int(tile_ox), -1, int(tiles_x) - 1);
}

/* Shared pixel area evaluator for both tiled fragment and tile-compute tests. */
float tile_eval_pixel_area(GpuTileDispatch* d, float2 pixel) {
    int px = int(floor(pixel.x));
    int py = int(floor(pixel.y));
    if (px < 0 || py < 0)
        return 0.0f;
    int ltx = (px >> 4) - int(d.tile_ox);
    int lty = (py >> 4) - int(d.tile_oy);
    if (ltx < 0 || lty < 0 || uint32_t(ltx) >= d.tiles_x || uint32_t(lty) >= d.tiles_y)
        return 0.0f;

    uint32_t tidx = uint32_t(lty) * d.tiles_x + uint32_t(ltx);
    uint32_t ly = uint32_t(py) & 15u;
    float area = float(d.row_backdrops[tidx * OPTICS_GFX_TILE_SIZE + ly]) / OPTICS_GFX_TILE_BD_SCALE;

    GpuTileHeader h = d.tile_headers[tidx];
    for (uint32_t k = 0u; k < h.count; ++k) {
        uint32_t s_idx = d.tile_indices[h.offset + k];
        GpuSegment s = d.segs[s_idx];
        optics_gfx_vector_area_segment(pixel, float2(s.x0, s.y0), float2(s.x1, s.y1), area);
    }
    return area;
}

#endif /* CANVAS_TILE_COMMON_SLANG */
