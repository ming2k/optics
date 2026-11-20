/*
 * canvas_tile_bin.comp.slang — GPU coarse tile binning & per-pixel-row
 * backdrop accumulation (ADR-0120).
 *
 * Compiled twice via -DFOR_SCATTER=1:
 *   count   : counts segments per tile + accumulates fixed-point per-row backdrop
 *   scatter : writes segment indices into prefix-summed tile_indices slices
 */

#include "canvas_tile_common.slang"

struct TilePush {
    GpuTileDispatch* d;
};

[[vk::push_constant]]
TilePush pc;

[shader("compute")]
[numthreads(64, 1, 1)]
void main(uint32_t seg_idx : SV_DispatchThreadID) {
    GpuTileDispatch* d = pc.d;
    if (seg_idx >= d.seg_count)
        return;

    GpuSegment s = d.segs[seg_idx];
    float dy = s.y1 - s.y0;
    if (!isfinite(dy) || dy == 0.0f || !isfinite(s.x0) || !isfinite(s.x1))
        return;

    float sy_min = min(s.y0, s.y1);
    float sy_max = max(s.y0, s.y1);

    int ty0 = max(int(floor(sy_min / 16.0f)) - int(d.tile_oy), 0);
    int ty1 = min(int(floor(sy_max / 16.0f)) - int(d.tile_oy), int(d.tiles_y) - 1);
    if (ty0 > ty1)
        return;

#ifndef FOR_SCATTER
    float sign_probe = 0.0f;
    optics_gfx_vector_area_segment(float2(0.0f, 0.0f), float2(2.0f, 0.0f), float2(2.0f, 1.0f),
                             sign_probe);
    int bd_sign = sign_probe >= 0.0f ? 1 : -1;
#endif

    for (int ty = ty0; ty <= ty1; ++ty) {
        float Y0 = float((int(d.tile_oy) + ty) * 16);
        float Y1 = Y0 + 16.0f;
        if (sy_max <= Y0 || sy_min >= Y1)
            continue;

        int lx0, lx1;
        tile_seg_row_span(s, ty, d.tile_ox, d.tile_oy, d.tiles_x, lx0, lx1);

        for (int tx = lx0; tx <= lx1; ++tx) {
            uint32_t tidx = uint32_t(ty) * d.tiles_x + uint32_t(tx);
#ifndef FOR_SCATTER
            InterlockedAdd(d.tile_headers[tidx].count, 1u);
#else
            uint32_t orig = 0u;
            InterlockedAdd(d.tile_headers[tidx].cursor, 1u, orig);
            uint32_t slot = d.tile_headers[tidx].offset + orig;
            if (slot < d.max_indices)
                d.tile_indices[slot] = seg_idx;
#endif
        }

#ifndef FOR_SCATTER
        if (lx0 > 0) {
            int py_base = (int(d.tile_oy) + ty) * 16;
            int ly0 = max(int(floor(sy_min)) - py_base, 0);
            int ly1 = min(int(floor(sy_max)) - py_base, 15);
            uint32_t bd_tile = uint32_t(ty) * d.tiles_x + uint32_t(lx0 - 1);
            for (int ly = ly0; ly <= ly1; ++ly) {
                float py = float(py_base + ly);
                int fp0 = int(round(clamp(s.y0 - py, 0.0f, 1.0f) * OPTICS_GFX_TILE_BD_SCALE));
                int fp1 = int(round(clamp(s.y1 - py, 0.0f, 1.0f) * OPTICS_GFX_TILE_BD_SCALE));
                int delta = (fp1 - fp0) * bd_sign;
                if (delta != 0)
                    InterlockedAdd(d.row_backdrops[bd_tile * OPTICS_GFX_TILE_SIZE + uint32_t(ly)], delta);
            }
        }
#endif
    }
}
