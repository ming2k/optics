/*
 * Tile-compute winding backdrop (ADR-0120 Phase B3 gate).
 *
 * Assert that per-tile winding computed via two-level prefix sum matches
 * flux_curve_cover_winding on all interior and exterior non-boundary tiles.
 */
#include "test_helpers.h"

#include "../../../libs/flux/src/canvas/canvas_tile.h"
#include <flux/curve.h>
#include <flux/flux.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define W 256u
#define H 256u
#define MAX_SEGS 2048u
#define MAX_INDICES 65536u

static void verify_backdrop_gate(const flux_curve_seg *curve_segs, uint32_t n_curves,
                                 const flux_tile_segment *segs, uint32_t n_segs) {
    flux_curve_cover cover = {
        .segs = curve_segs,
        .count = n_curves,
        .bounds = flux_curve_cover_bounds(curve_segs, n_curves),
    };

    uint32_t tx = 0, ty = 0;
    canvas_tile_grid_dims(W, H, &tx, &ty);
    uint32_t total_tiles = tx * ty;

    flux_tile_desc *tiles = calloc(total_tiles, sizeof(flux_tile_desc));
    uint32_t *indices = malloc(MAX_INDICES * sizeof(uint32_t));
    EXPECT(tiles && indices);
    if (!tiles || !indices) {
        free(tiles);
        free(indices);
        return;
    }

    uint32_t total_indices = 0;
    EXPECT(canvas_tile_bin_segments(segs, n_segs, W, H, tiles, indices, MAX_INDICES,
                                    &total_indices) == FLUX_OK);
    EXPECT(canvas_tile_compute_backdrop(segs, n_segs, W, H, tiles) == FLUX_OK);

    /* Gate B3: per-tile winding matches flux_curve_cover_winding */
    uint32_t checked_tiles = 0;
    for (uint32_t y = 0; y < ty; ++y) {
        float cy = ((float)y + 0.5f) * (float)FLUX_TILE_SIZE;
        for (uint32_t x = 0; x < tx; ++x) {
            float cx = ((float)x + 0.5f) * (float)FLUX_TILE_SIZE;
            uint32_t tidx = y * tx + x;

            /* Interior solid or exterior empty tiles have no intersecting segments */
            if (tiles[tidx].segment_count == 0) {
                int32_t ref_w = flux_curve_cover_winding(cover, cx, cy);
                EXPECT(tiles[tidx].backdrop_winding == ref_w);
                checked_tiles++;
            }
        }
    }
    EXPECT(checked_tiles > 0);

    free(tiles);
    free(indices);
}

int main(void) {
    /* 1. Solid rectangle: interior tiles must have winding = 1, exterior = 0 */
    {
        flux_point p[4] = {{32, 32}, {160, 32}, {160, 160}, {32, 160}};
        flux_curve_seg c_segs[4] = {
            flux_curve_seg_make(FLUX_CURVE_LINE, p[0], (flux_point){96, 32}, p[1]),
            flux_curve_seg_make(FLUX_CURVE_LINE, p[1], (flux_point){160, 96}, p[2]),
            flux_curve_seg_make(FLUX_CURVE_LINE, p[2], (flux_point){96, 160}, p[3]),
            flux_curve_seg_make(FLUX_CURVE_LINE, p[3], (flux_point){32, 96}, p[0]),
        };
        flux_tile_segment t_segs[4] = {
            {p[0].x, p[0].y, p[1].x, p[1].y},
            {p[1].x, p[1].y, p[2].x, p[2].y},
            {p[2].x, p[2].y, p[3].x, p[3].y},
            {p[3].x, p[3].y, p[0].x, p[0].y},
        };
        verify_backdrop_gate(c_segs, 4, t_segs, 4);
    }

    /* 2. Donut (outer CW, inner CCW hole): hole must have winding = 0 */
    {
        flux_point out[4] = {{16, 16}, {192, 16}, {192, 192}, {16, 192}};
        flux_point in[4] = {{64, 64}, {64, 144}, {144, 144}, {144, 64}}; /* CCW */

        flux_curve_seg c_segs[8] = {
            flux_curve_seg_make(FLUX_CURVE_LINE, out[0], (flux_point){104, 16}, out[1]),
            flux_curve_seg_make(FLUX_CURVE_LINE, out[1], (flux_point){192, 104}, out[2]),
            flux_curve_seg_make(FLUX_CURVE_LINE, out[2], (flux_point){104, 192}, out[3]),
            flux_curve_seg_make(FLUX_CURVE_LINE, out[3], (flux_point){16, 104}, out[0]),

            flux_curve_seg_make(FLUX_CURVE_LINE, in[0], (flux_point){64, 104}, in[1]),
            flux_curve_seg_make(FLUX_CURVE_LINE, in[1], (flux_point){104, 144}, in[2]),
            flux_curve_seg_make(FLUX_CURVE_LINE, in[2], (flux_point){144, 104}, in[3]),
            flux_curve_seg_make(FLUX_CURVE_LINE, in[3], (flux_point){104, 64}, in[0]),
        };
        flux_tile_segment t_segs[8] = {
            {out[0].x, out[0].y, out[1].x, out[1].y}, {out[1].x, out[1].y, out[2].x, out[2].y},
            {out[2].x, out[2].y, out[3].x, out[3].y}, {out[3].x, out[3].y, out[0].x, out[0].y},

            {in[0].x, in[0].y, in[1].x, in[1].y},     {in[1].x, in[1].y, in[2].x, in[2].y},
            {in[2].x, in[2].y, in[3].x, in[3].y},     {in[3].x, in[3].y, in[0].x, in[0].y},
        };
        verify_backdrop_gate(c_segs, 8, t_segs, 8);
    }

    /* 3. Self-intersecting 5-pointed star: center winding = 2 */
    {
        const int k = 5;
        flux_point v[5];
        for (int i = 0; i < k; ++i) {
            double a = -1.5707963 + (double)i * (2.0 * 3.14159265358979 / (double)k);
            v[i] = (flux_point){(float)(128.0 + 96.0 * cos(a)), (float)(128.0 + 96.0 * sin(a))};
        }
        const int order[5] = {0, 2, 4, 1, 3};
        flux_curve_seg c_segs[5];
        flux_tile_segment t_segs[5];
        for (int i = 0; i < k; ++i) {
            flux_point p0 = v[order[i]];
            flux_point p1 = v[order[(i + 1) % k]];
            flux_point mid = {(p0.x + p1.x) * 0.5f, (p0.y + p1.y) * 0.5f};
            c_segs[i] = flux_curve_seg_make(FLUX_CURVE_LINE, p0, mid, p1);
            t_segs[i] = (flux_tile_segment){p0.x, p0.y, p1.x, p1.y};
        }
        verify_backdrop_gate(c_segs, 5, t_segs, 5);
    }

    /* 4. Large Circle */
    {
        const uint32_t N = 64;
        flux_curve_seg c_segs[64];
        flux_tile_segment t_segs[64];
        for (uint32_t i = 0; i < N; ++i) {
            double a0 = (double)i * 2.0 * 3.14159265358979 / (double)N;
            double a1 = (double)(i + 1) * 2.0 * 3.14159265358979 / (double)N;
            flux_point p0 = {(float)(128.0 + 80.0 * cos(a0)), (float)(128.0 + 80.0 * sin(a0))};
            flux_point p1 = {(float)(128.0 + 80.0 * cos(a1)), (float)(128.0 + 80.0 * sin(a1))};
            flux_point mid = {(p0.x + p1.x) * 0.5f, (p0.y + p1.y) * 0.5f};
            c_segs[i] = flux_curve_seg_make(FLUX_CURVE_LINE, p0, mid, p1);
            t_segs[i] = (flux_tile_segment){p0.x, p0.y, p1.x, p1.y};
        }
        verify_backdrop_gate(c_segs, N, t_segs, N);
    }

    TEST_SUMMARY();
}
