/*
 * canvas_tile_scan.comp.slang — GPU prefix scan for tile offsets and
 * right-to-left per-pixel-row backdrop scan (ADR-0120).
 *
 * Compiled twice via -DFOR_BACKDROP=1:
 *   offsets  : exclusive prefix sum over tile_headers[].count -> .offset
 *   backdrop : inclusive right-to-left suffix scan across each pixel row
 */

#include "canvas_tile_common.slang"

struct TilePush {
    GpuTileDispatch* d;
};

[[vk::push_constant]]
TilePush pc;

#ifndef FOR_BACKDROP

groupshared uint32_t s_vals[256];
groupshared uint32_t s_carry;

[shader("compute")]
[numthreads(256, 1, 1)]
void main(uint32_t tid : SV_GroupThreadID) {
    GpuTileDispatch* d = pc.d;
    uint32_t num_tiles = d.tiles_x * d.tiles_y;
    if (tid == 0u)
        s_carry = 0u;
    GroupMemoryBarrierWithGroupSync();

    for (uint32_t base = 0u; base < num_tiles; base += 256u) {
        uint32_t idx = base + tid;
        s_vals[tid] = idx < num_tiles ? d.tile_headers[idx].count : 0u;
        GroupMemoryBarrierWithGroupSync();

        for (uint32_t stride = 1u; stride < 256u; stride <<= 1u) {
            uint32_t add = tid >= stride ? s_vals[tid - stride] : 0u;
            GroupMemoryBarrierWithGroupSync();
            s_vals[tid] += add;
            GroupMemoryBarrierWithGroupSync();
        }

        uint32_t excl = tid > 0u ? s_vals[tid - 1u] : 0u;
        if (idx < num_tiles) {
            d.tile_headers[idx].offset = s_carry + excl;
            d.tile_headers[idx].cursor = 0u;
        }
        GroupMemoryBarrierWithGroupSync();
        if (tid == 0u)
            s_carry += s_vals[255];
        GroupMemoryBarrierWithGroupSync();
    }
}

#else

[shader("compute")]
[numthreads(64, 1, 1)]
void main(uint32_t row_idx : SV_DispatchThreadID) {
    GpuTileDispatch* d = pc.d;
    uint32_t total_rows = d.tiles_y * OPTICS_GFX_TILE_SIZE;
    if (row_idx >= total_rows)
        return;

    uint32_t ty = row_idx >> 4u;
    uint32_t ly = row_idx & 15u;
    int32_t acc = 0;
    for (int tx = int(d.tiles_x) - 1; tx >= 0; --tx) {
        uint32_t slot = (ty * d.tiles_x + uint32_t(tx)) * OPTICS_GFX_TILE_SIZE + ly;
        acc += d.row_backdrops[slot];
        d.row_backdrops[slot] = acc;
    }
}

#endif
