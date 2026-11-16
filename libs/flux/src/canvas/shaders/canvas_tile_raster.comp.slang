/*
 * canvas_tile_raster.comp.slang — Tiled 16x16 analytic coverage compute shader
 * reading pre-binned segment lists and per-pixel-row backdrops (ADR-0116 / ADR-0120).
 */

#include "canvas_tile_common.slang"

struct PushConstants {
    GpuSegment*    segs;
    GpuTileHeader* tiles;
    int32_t*       row_backdrops;
    uint32_t*      indices;
    float*         cov_data;
    uint32_t       tiles_x;
    uint32_t       tiles_y;
    uint32_t       width;
    uint32_t       height;
    uint32_t       fill_rule; /* 0 = NonZero, 1 = EvenOdd */
    float          ox;
    float          oy;
};

[[vk::push_constant]]
PushConstants pc;

[shader("compute")]
[numthreads(16, 16, 1)]
void main(uint3 groupID : SV_GroupID, uint3 localID : SV_GroupThreadID) {
    uint32_t tx = groupID.x;
    uint32_t ty = groupID.y;
    if (tx >= pc.tiles_x || ty >= pc.tiles_y)
        return;

    uint32_t gx = tx * FLUX_TILE_SIZE + localID.x;
    uint32_t gy = ty * FLUX_TILE_SIZE + localID.y;
    if (gx >= pc.width || gy >= pc.height)
        return;

    uint32_t tidx = ty * pc.tiles_x + tx;
    float area = float(pc.row_backdrops[tidx * FLUX_TILE_SIZE + localID.y]) / FLUX_TILE_BD_SCALE;

    GpuTileHeader td = pc.tiles[tidx];
    float2 pixel = float2(pc.ox + float(gx), pc.oy + float(gy));
    for (uint32_t k = 0u; k < td.count; ++k) {
        uint32_t s_idx = pc.indices[td.offset + k];
        GpuSegment s = pc.segs[s_idx];
        flux_vector_area_segment(pixel, float2(s.x0, s.y0), float2(s.x1, s.y1), area);
    }

    pc.cov_data[gy * pc.width + gx] = flux_vector_area_coverage(area, pc.fill_rule);
}
