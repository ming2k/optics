/*
 * canvas_tile_raster.comp.slang — GPU Tile-Compute Coverage Rasterizer in Slang (ADR-0120 Phase B).
 */

struct GpuSegment {
    float x0, y0;
    float x1, y1;
};

struct GpuTileDesc {
    uint32_t segment_offset;
    uint32_t segment_count;
    int32_t  backdrop_winding;
    uint32_t pad;
};

struct PushConstants {
    GpuSegment*  segs;
    GpuTileDesc* tiles;
    uint32_t*    indices;
    float*       cov_data;
    uint32_t     tiles_x;
    uint32_t     tiles_y;
    uint32_t     width;
    uint32_t     height;
    uint32_t     fill_rule; /* 0 = NonZero, 1 = EvenOdd */
    float        ox;
    float        oy;
};

[[vk::push_constant]]
PushConstants pc;

bool clip_test(float p, float q, inout float t0, inout float t1) {
    if (p == 0.0f) {
        if (q < 0.0f)
            return false;
    } else {
        float r = q / p;
        if (p < 0.0f) {
            if (r > t1)
                return false;
            if (r > t0)
                t0 = r;
        } else {
            if (r < t0)
                return false;
            if (r < t1)
                t1 = r;
        }
    }
    return true;
}

[shader("compute")]
[numthreads(16, 16, 1)]
void main(uint3 groupID : SV_GroupID, uint3 localID : SV_GroupThreadID) {
    uint32_t tx = groupID.x;
    uint32_t ty = groupID.y;
    if (tx >= pc.tiles_x || ty >= pc.tiles_y)
        return;

    uint32_t tidx = ty * pc.tiles_x + tx;
    GpuTileDesc td = pc.tiles[tidx];

    uint32_t lx = localID.x;
    uint32_t ly = localID.y;
    uint32_t gx = tx * 16u + lx;
    uint32_t gy = ty * 16u + ly;
    if (gx >= pc.width || gy >= pc.height)
        return;

    uint32_t out_idx = gy * pc.width + gx;

    // Fast-path: Solid interior or empty exterior tile
    if (td.segment_count == 0u) {
        int32_t w = td.backdrop_winding;
        bool inside = (pc.fill_rule == 1u) ? ((w & 1) != 0) : (w != 0);
        pc.cov_data[out_idx] = inside ? 1.0f : 0.0f;
        return;
    }

    float px_left = float(gx);
    float px_right = px_left + 1.0f;
    float px_top = float(gy);
    float px_bottom = px_top + 1.0f;

    float winding = float(td.backdrop_winding);
    float area = 0.0f;

    for (uint32_t k = 0u; k < td.segment_count; ++k) {
        uint32_t s_idx = pc.indices[td.segment_offset + k];
        GpuSegment s = pc.segs[s_idx];

        float x0 = s.x0 - pc.ox;
        float y0 = s.y0 - pc.oy;
        float x1 = s.x1 - pc.ox;
        float y1 = s.y1 - pc.oy;

        if (y0 == y1)
            continue;

        float ymin = min(y0, y1);
        float ymax = max(y0, y1);
        if (ymax <= px_top || ymin >= px_bottom)
            continue;

        // Clip to pixel horizontal row [px_top, px_bottom]
        float row_t0 = clamp((px_top - y0) / (y1 - y0), 0.0f, 1.0f);
        float row_t1 = clamp((px_bottom - y0) / (y1 - y0), 0.0f, 1.0f);
        float ry0 = lerp(y0, y1, row_t0);
        float ry1 = lerp(y0, y1, row_t1);
        float rx0 = lerp(x0, x1, row_t0);
        float rx1 = lerp(x0, x1, row_t1);

        float dy = ry1 - ry0;
        if (abs(dy) <= 1e-7f)
            continue;

        // Check if entirely to the right
        float row_min_x = min(rx0, rx1);
        float row_max_x = max(rx0, rx1);

        if (row_min_x >= px_right) {
            winding += dy;
            continue;
        }
        if (row_max_x <= px_left) {
            continue;
        }

        // Liang-Barsky parametric clipping to [px_left, px_right] x [px_top, px_bottom]
        float dx = x1 - x0;
        float full_dy = y1 - y0;
        float t_in = 0.0f;
        float t_out = 1.0f;

        if (clip_test(-dx, x0 - px_left, t_in, t_out) &&
            clip_test(dx, px_right - x0, t_in, t_out) &&
            clip_test(-full_dy, y0 - px_top, t_in, t_out) &&
            clip_test(full_dy, px_bottom - y0, t_in, t_out)) {

            float cx0 = lerp(x0, x1, t_in);
            float cy0 = lerp(y0, y1, t_in);
            float cx1 = lerp(x0, x1, t_out);
            float cy1 = lerp(y0, y1, t_out);

            float local_dy = cy1 - cy0;
            float u0 = cx0 - px_left;
            float u1 = cx1 - px_left;

            area += local_dy * (1.0f - 0.5f * (u0 + u1));

            if (full_dy > 0.0f) {
                float y_exit = lerp(y0, y1, t_out);
                if (cx1 >= px_right - 1e-5f && ry1 > y_exit)
                    winding += (ry1 - y_exit);
                float y_enter = lerp(y0, y1, t_in);
                if (cx0 >= px_right - 1e-5f && y_enter > ry0)
                    winding += (y_enter - ry0);
            } else {
                float y_exit = lerp(y0, y1, t_out);
                if (cx1 >= px_right - 1e-5f && ry1 < y_exit)
                    winding += (ry1 - y_exit);
                float y_enter = lerp(y0, y1, t_in);
                if (cx0 >= px_right - 1e-5f && y_enter < ry0)
                    winding += (y_enter - ry0);
            }
        } else {
            if (0.5f * (rx0 + rx1) >= px_right)
                winding += dy;
        }
    }

    float total_coverage = abs(winding + area);
    if (pc.fill_rule == 1u) {
        total_coverage = fmod(total_coverage, 2.0f);
        if (total_coverage > 1.0f)
            total_coverage = 2.0f - total_coverage;
    }

    pc.cov_data[out_idx] = clamp(total_coverage, 0.0f, 1.0f);
}
